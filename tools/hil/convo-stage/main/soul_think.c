/*
 * soul_think.c — 思维轮实现（W3；29 号 §9 协议安全要点逐条落地）
 *
 * 安全边界（每条可审计）：
 *  - 唯一出口：LLM 产出只经 ki_intent_parse 进固件，无字段可写 ke/ki
 *    内部态（情绪唯一回写=<mood> 协议，W2 已有通道）。
 *  - 拒非降级：parse 非 OK/非 LONG → 不出声 + 码落痕，绝不猜。
 *  - 出口门：ki_intent_fire（quiet/cooldown/quota/speaking/ethics/tts）
 *    只门动作不门思维；'i' 强制仅落痕 forced=1 并越过执行否决，
 *    parse/沉默语义照常（HIL 也要能看到自主沉默）。
 *  - 结算六态：仅 IGNORED 翻倍；SILENT 需新意才再想（收敛判据）。
 *  - 去重窗：content 与上一次实播相同且 <5min → 拒（幂等轻量版）。
 *
 * 时间口径：W3 用 uptime 分钟（mod 1440 对齐 ki 的当日分钟算术）；
 * SNTP 随 W6b 供给后切真实时钟。'd' 压缩档把思维时钟 ×6（10min 间隔
 * → 实际 100s），供 HIL 自然触发演示；说话冷却不压缩（产品语义）。
 */
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "convo_fsm.h"
#include "kaleido_core.h"
#include "kaleido_ignition.h"
#include "kaleido_speech.h"
#include "soul.h"
#include "soul_think.h"
#include "clause_pump.h"

#define THINK_STACK		(16 * 1024)	/* 内部栈：TLS/HTTPS 深度 */
#define THINK_PRIO		4
#define THINK_REPLY_MAX		512
#define THINK_SETTLE_S		20	/* 播完后人声观察窗 */
#define THINK_DEDUP_MIN		5	/* content 去重窗（分钟） */
#define THINK_CLOCK_SCALE	6	/* 'd' 压缩：思维时钟 ×6 */

extern void on_tts_pcm(void *user, const uint8_t *pcm, size_t len);
extern size_t pb_pending(void);

static struct ks_tts_cfg *s_tts;
static struct ks_llm_cfg *s_llm;
static volatile bool s_busy;
static volatile bool s_new_event;
static volatile bool s_barge_hit;
static volatile bool s_voice_seen;

static uint8_t s_reason;                /* 触发 reason_mask（启动快照） */
static bool s_forced;
static float s_trig_score;              /* 触发时 ki_step 分（新意门基准） */
static bool s_inject;                   /* HIL：假意图直注（跳深思 LLM） */
static struct ki_intent s_inject_intent;
static bool s_inj_json_mode;            /* HIL：坏 JSON 直注（走 parse 拒绝原路径，'H'） */
static char s_inj_json[160];

static uint32_t s_last_think_min;
static uint8_t s_thought_today;
static bool s_last_silent;
static float s_last_silent_score;
static uint32_t s_last_fire_min;
static uint8_t s_fired_today;
static char s_last_content[KI_CONTENT_MAX + 1];
static uint32_t s_last_content_min;
static bool s_compress;                 /* 'd' 压缩态（soul.c 同步） */
static TaskHandle_t s_handle;           /* 常驻 think 任务（init 创建、
					 * 挂起待命——评审教训：不在 soul
					 * tick 里现场 xTaskCreate 16KB，
					 * 曾与 wakenet AFE 大块分配窗口
					 * 相撞致早期 Store fault） */
/* 静态内部栈（缺陷 I 整改）：CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y
 * 下动态栈在 AFE 吃满内部内存后会落 PSRAM → TLS 关 cache 窗口即 fault
 * （板级纪律：碰 TLS/flash 的任务栈必须内部 RAM，编译期保留） */
static StackType_t s_thk_stack[THINK_STACK / sizeof(StackType_t)];
static StaticTask_t s_thk_tcb;

static uint32_t uptime_min(void)
{
	return (uint32_t)(esp_timer_get_time() / 60000000ll) % 1440u;
}

static void think_task(void *arg);

void soul_think_init(struct ks_tts_cfg *tts, struct ks_llm_cfg *llm)
{
	s_tts = tts;
	s_llm = llm;
	s_handle = xTaskCreateStatic(think_task, "think",
				     THINK_STACK / sizeof(StackType_t),
				     NULL, THINK_PRIO, s_thk_stack,
				     &s_thk_tcb);
	if (!s_handle)
		kf_log("VL,KI,FAIL,task\r\n");    /* 任务自阻塞于 notify */
}

void soul_think_gate_ctx(struct ki_think_ctx *tc)
{
	if (!tc)
		return;
	tc->now_min = s_compress ? uptime_min() * THINK_CLOCK_SCALE :
				   uptime_min();
	/* last_think_min 同尺缩放，保证间隔算术同域 */
	tc->last_think_min = s_last_think_min *
			     (s_compress ? THINK_CLOCK_SCALE : 1u);
	tc->thought_today = s_thought_today;
	tc->last_was_silent = s_last_silent;
	tc->last_silent_score = s_last_silent_score;
}

void soul_think_on_event(void)
{
	s_new_event = true;
}

void soul_think_note_voice(void)
{
	s_voice_seen = true;
}

void soul_think_note_barge(void)
{
	s_barge_hit = true;
}

bool soul_think_busy(void)
{
	return s_busy;
}

void soul_think_set_compress(bool on)
{
	s_compress = on;
}

/* ---- think 任务（一次性；单飞） ---- */

static void think_log(const char *ev, const char *detail)
{
	kf_log("VL,KI,%s,%s\r\n", ev, detail);
	kf_ev("ki", "%s %s", ev, detail);
}

/* LLM 整句收集回调（不切子句）。游标必须随调用实例（缺陷 L 陪跑崩：
 * 曾用 static size_t off 跨思维轮累计——reply[0]=0 清缓冲不清 off，
 * 累计超 THINK_REPLY_MAX 后 size 下溢成无界写，静态区被回复文本碾过，
 * 调度器任务列表损坏→Load access fault（MTVAL=文本碎片）。缓冲满
 * 返回 false 中止流，parse 拒截尾 */
struct think_reply {
	char *buf;
	size_t off;
};

static bool collect_cb(void *user, const char *sent, bool final)
{
	struct think_reply *r = user;

	(void)final;
	if (r->off >= THINK_REPLY_MAX - 1)
		return false;
	r->off += strlcpy(r->buf + r->off, sent, THINK_REPLY_MAX - r->off);
	return true;
}

/* 深思 prompt：状态+驱力告警+独处+typed 协议（29 号 §9.2） */
static void think_prompt(char *buf, size_t n)
{
	struct soul_snap snap;
	const char *v, *a, *u;
	static const char *const sev_name[] = { "正常", "低", "高", "危急" };

	soul_snapshot(&snap);
	v = snap.emo.state.valence >= 0.15f ? "愉悦偏正" :
	    snap.emo.state.valence <= -0.15f ? "低落偏负" : "平静";
	a = snap.emo.state.arousal >= 0.55f ? "较高" :
	    snap.emo.state.arousal >= 0.30f ? "中等" : "低";
	u = snap.emo.state.urgency >= 0.5f ? "强" :
	    snap.emo.state.urgency >= 0.10f ? "轻微" : "无";
	snprintf(buf, n,
		 "[主动行为决策] 你是墨墨，现在没有人在和你说话。"
		 "内在状态：心情%s、活力%s、表达冲动%s。"
		 "驱力告警：social=%s、stim=%s、rest=%s。"
		 "%s"
		 "请决定此刻是否主动说一句话。只输出一行 JSON，格式："
		 "{\"act\":\"speak\",\"content\":\"不超过25字的一句话\","
		 "\"confidence\":0.8,\"evidence\":[\"drive:social\"],"
		 "\"reason\":\"一句话理由\"}。"
		 "act 只能是 speak 或 stay_silent；没有充分理由必须 "
		 "stay_silent（content 留空字符串）；content 不要出现"
		 "数字；evidence 至少一条，前缀只能是 "
		 "drive|affect|mem|dialog|bin。",
		 v, a, u,
		 sev_name[snap.drive.sev[0]], sev_name[snap.drive.sev[1]],
		 sev_name[snap.drive.sev[2]],
		 snap.emo.state.ethics_latched ?
		 "（安抚模式：任何输出只能温柔安抚。）" : "");
}

/* TTS 直说（不经 ASR 轮；复用全局播放流）。false=TTS 失败（TTL 口径） */
static bool speak_over_air(const char *text)
{
	if (ks_tts_open(s_tts, &(struct ks_tts_events){
				.on_pcm = on_tts_pcm,
				.on_error = tts_err_telemetry }) != 0)
		return false;
	ks_tts_feed(text, false, 15000);
	ks_tts_feed(NULL, true, 15000);
	ks_tts_close();
	return true;
}

static void think_task(void *arg)
{
	static char prompt[896];
	static char reply[THINK_REPLY_MAX];
	struct think_reply rb = { .buf = reply, .off = 0 };

	(void)arg;
	for (;;) {
	/* 通知门控：任务创建后阻塞于此——启动参数（s_inject 等）在唤醒
	 * 前已就绪。修复：创建即抢跑（prio 高于 app_main）曾致首跑直入
	 * LLM 路径又被挂起在 TLS 中间，僵尸会话污染后续 resume。 */
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	struct ks_llm_cfg dl;
	struct soul_snap snap;
	struct ki_intent intent;
	struct ki_decision dec;
	enum ki_parse_err pe;
	uint32_t now = uptime_min();
	bool forced = s_forced;
	int rc;

	(void)arg;
	reply[0] = 0;
	if (s_inject) {
		intent = s_inject_intent;       /* HIL 假意图：跳深思 */
		goto executed;
	}
	if (s_inj_json_mode) {
		/* HIL 坏 JSON：跳 LLM，parse 拒绝路径与自然轮同一套 */
		strlcpy(reply, s_inj_json, sizeof(reply));
		goto parsed;
	}
	think_prompt(prompt, sizeof(prompt));
	kf_log("VL,KI,THINK,reason=%u,forced=%d,score=%.2f\r\n",
	       s_reason, forced, s_trig_score);
	/* 深思 LLM：provider 沿用主链（url/key/model/no_think），prompt
	 * 换深思协议，整句收集不切子句 */
	dl = *s_llm;
	dl.system = prompt;
	dl.user_msg = "（内心思考）";
	dl.cut_clauses = false;
		rc = ks_llm_chat(&dl, collect_cb, &rb, NULL);
	if (rc != 0 || !reply[0]) {
		think_log("LLMFAIL", "deliberation");
		ki_settle2(KI_SETTLE_SILENT);
		s_last_silent = true;
		goto out;
	}
	if (kf_dbg_get())
		kf_log("VL,KI,RAW,%.120s\r\n", reply);

parsed:
	pe = ki_intent_parse(reply, strlen(reply), &intent);
	if (pe != KI_PARSE_OK && pe != KI_PARSE_CONTENT_LONG) {
		char d[48];

		snprintf(d, sizeof(d), "parse=%d", (int)pe);
		think_log("REJECT", d);         /* 拒非降级：绝不猜 */
		ki_settle2(KI_SETTLE_SILENT);
		s_last_silent = true;
		goto out;
	}
	if (intent.act == KI_STAY_SILENT ||
	    intent.confidence < KI_CONF_FLOOR) {
		char d[64];

		snprintf(d, sizeof(d), "conf=%.2f,reason=%.24s",
			 intent.confidence, intent.reason);
		think_log("SILENT", d);         /* AI 自主决定不说 */
		ki_settle2(KI_SETTLE_SILENT);
		s_last_silent = true;
		s_last_silent_score = s_trig_score;
		goto out;
	}

	/* 去重窗（幂等轻量版） */
	if (!strncmp(intent.content, s_last_content,
		     sizeof(intent.content)) &&
	    now - s_last_content_min < THINK_DEDUP_MIN) {
		think_log("REJECT", "duplicate");
		ki_settle2(KI_SETTLE_SILENT);
		s_last_silent = true;
		goto out;
	}

	/* 出口硬门（只门动作；forced 落痕并越过执行否决） */
	soul_snapshot(&snap);
	{
		struct ki_fire_ctx fc = {
			.user_speaking = convo_active(),
			.ethics_latched = snap.emo.state.ethics_latched,
			.tts_ready = s_tts != NULL,
			.now_min = now,
			.last_fire_min = s_last_fire_min,
			.fired_today = s_fired_today,
		};

		ki_intent_fire(&intent, &fc, ki_config_default(), &dec);
	}
	if (dec.gates.gate_mask && !forced) {
		char d[48];

		snprintf(d, sizeof(d), "fire_mask=%u", dec.gates.gate_mask);
		think_log("GATE", d);
		ki_settle2(KI_SETTLE_GATE_BLOCKED);
		goto out;
	}
	if (dec.gates.gate_mask)
		think_log("FORCE", "gate bypassed (hil)");

executed:
	/* 执行：TTS 直说 + 播放排空（barge 可打断） */
	soul_convo_bind(s_tts, NULL);
	kf_log("VL,KI,SPEAK,%s\r\n", intent.content);
	kf_ev("ki", "speak %s", intent.content);
	if (!speak_over_air(intent.content)) {
		think_log("TTL", "tts open fail");
		ki_settle2(KI_SETTLE_TTL_EXPIRED);
		s_last_silent = true;
		goto out;
	}
	for (int i = 0; i < 100 && pb_pending(); i++) {
		if (s_barge_hit)
			break;                  /* aio_tx_abort 已排干 */
		vTaskDelay(pdMS_TO_TICKS(200));
	}

	/* 结算：打断 > 人声 > 忽略 */
	if (s_barge_hit) {
		ki_settle2(KI_SETTLE_INTERRUPTED);
		think_log("SETTLE", "interrupted");
	} else {
		s_voice_seen = false;
		for (int i = 0; i < THINK_SETTLE_S * 5 && !s_barge_hit;
		     i++) {
			if (s_voice_seen)
				break;
			vTaskDelay(pdMS_TO_TICKS(200));
		}
		if (s_voice_seen || s_barge_hit) {
			ki_settle2(KI_SETTLE_RESPONDED);
			think_log("SETTLE", "responded");
		} else {
			ki_settle2(KI_SETTLE_IGNORED);
			kf_log("VL,KI,SETTLE,ignored,cooldown=%u"
			       "\r\n", ki_cooldown_current());
		}
	}
	strlcpy(s_last_content, intent.content, sizeof(s_last_content));
	s_last_content_min = now;
	s_fired_today++;
	s_last_fire_min = now;
	s_last_silent = false;
	goto out;

out:
	s_inject = false;
	s_inj_json_mode = false;
	s_thought_today++;
	s_last_think_min = now;
	s_new_event = false;
	s_barge_hit = false;
	s_busy = false;
	}       /* 回到 notify 门控待命 */
}

/* HIL 假意图直注：跳深思 LLM，出口门/执行/结算同一套（'h' 键） */
bool soul_think_inject(const struct ki_intent *intent)
{
	if (s_busy || !s_handle || !intent || intent->act != KI_SPEAK)
		return false;
	s_inject_intent = *intent;
	s_inject = true;
	s_reason = 0;
	s_forced = true;
	s_trig_score = 0.0f;
	s_busy = true;
	xTaskNotifyGive(s_handle);
	return true;
}

/* HIL 坏 JSON 直注（'H' 键）：parse 拒绝路径板上演练用 */
bool soul_think_inject_json(const char *js)
{
	if (s_busy || !s_handle || !js || !js[0])
		return false;
	strlcpy(s_inj_json, js, sizeof(s_inj_json));
	s_inj_json_mode = true;
	s_forced = false;
	s_reason = 0;
	s_trig_score = 0.0f;
	s_busy = true;
	xTaskNotifyGive(s_handle);
	return true;
}

bool soul_think_start(uint8_t reason_mask, bool forced, float score)
{
	if (s_busy || !s_handle)
		return false;
	s_reason = reason_mask;
	s_forced = forced;
	s_trig_score = score;
	s_busy = true;
	xTaskNotifyGive(s_handle);
	return true;
}
