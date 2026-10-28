/*
 * soul.c — 灵魂 limb 实现（W0-3）
 *
 * 线程模型（唯一写者纪律，plan §5R.0 G5）：
 *   soul 任务：唯一调用 kd_step/ke_step/ki_step（1Hz，vTaskDelayUntil
 *              节拍；漂移不追帧——灵魂拍允许丢，不允许雪崩）；
 *   钩子任务： 只置 pending 标志（portENTER_CRITICAL 微临界区）；
 *   消费任务： soul_snapshot() 锁内拷贝副本。
 * ki 现状（W0）：因子仅 affect_motive 接 ke urgency，其余四因子恒 0
 * （time_rhythm/event_hook/presence W3 供给、memory_hook W4 供给）；
 * candidate 仅遥测不上动作——主动对外动作等 W3 思维触发闸。
 */
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "convo_fsm.h"
#include "emote_stage.h"
#include "kaleido_speech.h"
#include "kaleido_core.h"
#include "kaleido_secret.h"
#include "soul.h"
#include "soul_think.h"

#define SOUL_TICK_MS	1000

/* HIL 时间压缩档（'d' 键，debug 门控）：代谢 ×100——V-48a 注入闭环与
 * 后续 W3 点火压缩 soak 的基建；产品态恒用内置冻结表 */
static const struct kd_params s_fast_bench = {
	.metabolic = { 0.02f, 0.01f, 0.03f },
	.pulse = {
		[KD_EVT_ROUND_OK] = 0.15f,
		[KD_EVT_ROUND_FAIL] = 0.03f,
		[KD_EVT_BARGE] = 0.05f,
		[KD_EVT_VOICE] = 0.005f,
		[KD_EVT_TOUCH] = 0.05f,
	},
	.stim_event = 0.10f,
	.rest_recover = 0.01f,
	.band = 0.1f,
};
#define SOUL_VOICE_MIN_MS	5000	/* VOICE 脉冲限频（28 号 §9.3 防刷） */
#define SOUL_TELEM_QUIET_S	15	/* 常态遥测降频（debug 态逐拍） */
#define SOUL_EMO_DWELL		8	/* 片段最短驻留（拍）——防抖动 */
#define SOUL_ASPLEEP_TICKS	600	/* 连续安静 600 拍（拍恒 1s——
					 * 压缩档只加速代谢不改拍频，
					 * 故两模式同为 ~10min）→ 睡着 */

static SemaphoreHandle_t s_lock;	/* 快照拷贝锁 */
static bool s_force_think;		/* 'i' 强制（临界区外 BOOL 单写） */
static uint8_t s_bin_prev;		/* W3 影响分箱（跃迁沿检测） */
static bool s_bin_valid;
static bool s_bin_changed;		/* 跃迁沿（think 启动后清） */
static bool s_configured;		/* OOBE 配置态（start 时缓存——
	 * PSRAM 栈任务禁碰 NVS/flash：cache 关断窗口断言
	 * esp_task_stack_is_sane_cache_disabled，W1 首板实证） */
static struct soul_snap s_snap;
static uint32_t s_ticks;

/* ---- 钩子 pending（临界区承载；BARGE > ROUND_* > VOICE/TOUCH） ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static enum kd_event s_pend_evt;
static enum ke_receipt s_pend_receipt;	/* 一轮结算一次（G1 口径） */
static float s_pend_mood;		/* W2：待转发的有界重评价 */
static int64_t s_last_voice_us;
static uint32_t s_mood_seq;

static void pend_push(enum kd_event evt, enum ke_receipt rc)
{
	portENTER_CRITICAL(&s_mux);
	/* 优先级：BARGE/ROUND_*（社交主信号）粘滞——不被 VOICE/TOUCH
	 * 弱脉冲覆盖（评审 C10/M-03：防止 eat 锚 +0.15 主脉冲被
	 * 5s 限频的 VOICE +0.005 顶掉）；同级最新覆盖（重复事件分拍
	 * 喂，取最近语义）。 */
	if (evt != KD_EVT_NONE) {
		bool weak_new = (evt == KD_EVT_VOICE ||
				 evt == KD_EVT_TOUCH);
		bool weak_pend = (s_pend_evt == KD_EVT_VOICE ||
				  s_pend_evt == KD_EVT_TOUCH);

		if (!weak_new || !s_pend_evt || weak_pend)
			s_pend_evt = evt;
	}
	if (rc != KE_RECEIPT_NONE)
		s_pend_receipt = rc;
	portEXIT_CRITICAL(&s_mux);
}

void soul_on_round(bool ok)
{
	pend_push(ok ? KD_EVT_ROUND_OK : KD_EVT_ROUND_FAIL,
		  ok ? KE_RECEIPT_SUCCESS : KE_RECEIPT_FAIL);
	soul_think_on_event();
}

void soul_on_barge(void)
{
	pend_push(KD_EVT_BARGE, KE_RECEIPT_NONE);
	soul_think_on_event();
	soul_think_note_barge();
}

void soul_on_mood(float delta)
{
	portENTER_CRITICAL(&s_mux);
	s_pend_mood = delta;
	portEXIT_CRITICAL(&s_mux);
}

/* W2：九维自然语言尾段（D-13 合规——只读注入；mood 是唯一回写通道） */
const char *soul_prompt_suffix(void)
{
	static char s_prompt[640];	/* 最坏路径全串 ~470B（gcc
					 * format-truncation 实测口径） */
	struct soul_snap snap;
	const struct ke_state *st;
	const char *v, *a, *u;

	soul_snapshot(&snap);
	st = &snap.emo.state;
	v = st->valence >= 0.15f ? "愉悦偏正" :
	    st->valence <= -0.15f ? "低落偏负" : "平静";
	a = st->arousal >= 0.55f ? "较高（活泼警觉）" :
	    st->arousal >= 0.30f ? "中等" : "低（安静倦怠）";
	u = st->urgency >= 0.5f ? "强（有想主动表达的冲动）" :
	    st->urgency >= 0.10f ? "轻微" : "无";
	snprintf(s_prompt, sizeof(s_prompt),
		 "\n\n[内在状态] 你（墨墨）此刻：心情%s，活力%s，"
		 "表达冲动%s。%s"
		 "若这轮对话明显改变了你的心情，在回复的最末尾附加恰好"
		 "一个 <mood d=\"+0.1\" t=\"6\"/> 标签（d 是 -0.2 到 +0.2 "
		 "的一位小数，心情变好为正；t 是 4 到 8 的整数）；没有明显"
		 "变化则不要附加，标签之外不要解释。",
		 v, a, u,
		 st->ethics_latched ?
		 "（当前处于安抚模式：只能用温柔安抚的语气说话。）" : "");
	return s_prompt;
}

void soul_on_voice(void)
{
	int64_t now = esp_timer_get_time();

	portENTER_CRITICAL(&s_mux);
	if (now - s_last_voice_us < SOUL_VOICE_MIN_MS * 1000) {
		portEXIT_CRITICAL(&s_mux);
		return;				/* 限频：连续在场不无限恢复 */
	}
	s_last_voice_us = now;
	portEXIT_CRITICAL(&s_mux);
	pend_push(KD_EVT_VOICE, KE_RECEIPT_NONE);
	soul_think_on_event();
	soul_think_note_voice();	/* 结算窗证据（不限频） */
}

void soul_fast_drain(bool on)
{
	kd_init(on ? &s_fast_bench : NULL);
	/* kd_init 仅换参数指针（const 表），水平保持——压缩前后连续 */
	soul_think_set_compress(on);	/* 思维时钟同步 ×6（W3 HIL） */
	kf_log("VL,SOUL,FAST,%s\r\n", on ? "on" : "off");
}

void soul_force_think(void)
{
	s_force_think = true;
}

/* 影响分箱：anim_tag 桶 ×urgency 档 ×valence 符号（SELF_REPORT 范本） */
static uint8_t bin_hash(const struct ke_output *e)
{
	static const char *const tags[] = {
		"happy", "relaxed", "angry", "sad", "surprised",
		"confused", "sleepy",
	};
	uint8_t t;

	for (t = 0; t < 7 && strcmp(tags[t], e->anim_tag); t++)
		;
	return (uint8_t)(t * 4 +
			 (e->state.urgency >= 0.5f ? 2 : 0) +
			 (e->state.valence < 0.0f ? 1 : 0));
}

uint32_t soul_tick_count(void)
{
	return s_ticks;
}

void soul_snapshot(struct soul_snap *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!s_lock)
		return;
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE)
		return;			/* 50ms 拿不到=拍正在写，下拍再取 */
	*out = s_snap;
	xSemaphoreGive(s_lock);
}

static bool s_prev_idle(enum convo_state st)
{
	return st == CONVO_IDLE || st == CONVO_OFF;
}

/* ---- W1 表达策略：九维 → 22 段猫伴片段（仅接管 IDLE 底色） ----
 * 主判 anim_tag（valence×arousal），次判用 ethics/urgency/bond/rest/
 * 安静时长微调。映射表维护纪律：改动须同步 soul-test 与 V-48b 判分。
 * 活动四态（倾听/思考/说话）不归本策略——KEX 桥直配优先。 */
static const char *emo_clip_pick(const struct ke_output *emo,
				 const struct kd_output *drv,
				 bool quiet_long)
{
	if (emo->state.ethics_latched)
		return "sigh_20s_20s";		/* 闩锁=安抚低落 */
	if (emo->state.urgency >= 0.5f && emo->state.valence <= -0.15f)
		return "panic_05s_15";
	if (emo->state.valence >= 0.15f)		/* happy */
		return emo->state.arousal >= 0.55f ?
			       (emo->state.social_bond >= 0.5f ?
				"laugh_05s_10" : "smile_05s") :
			       "leisure_05s_";	/* relaxed */
	if (emo->state.valence <= -0.15f)		/* sad/angry */
		return emo->state.arousal >= 0.55f ? "angry_20s" :
						     "sad_05s15s";
	if (emo->state.arousal >= 0.55f)
		return "shocked_05s_";		/* surprised */
	if (emo->state.arousal >= 0.30f)
		return "question_05s";		/* confused */
	if (drv->sev[KD_REST] >= KD_SEV_HIGH)
		return "yawn_20s";		/* 疲态显性化 */
	if (quiet_long)
		return "asleep_215s";		/* 长时无人入睡 */
	return "smile_static";			/* 平静基线 */
}

/* ---- W2 轮起手（评审 M-01 整改：基座缓存防逐轮累积） ---- */
void soul_convo_bind(struct ks_tts_cfg *tts, struct ks_llm_cfg *llm)
{
	static char s_sys_dyn[640];	/* 当轮有效；单轮单任务串行 */
	static const char *s_base;	/* 首轮缓存的人格基座 */

	if (!s_base)
		s_base = llm->system;	/* 首轮原样（此后 llm->system 指
					 * s_sys_dyn，不可再作基座源） */
	if (tts) {
		struct soul_snap snap;

		soul_snapshot(&snap);
		tts->speech_rate = snap.emo.tts_hint.rate;
		tts->pitch = (int)(snap.emo.tts_hint.pitch >= 0 ?
				   snap.emo.tts_hint.pitch + 0.5f :
				   snap.emo.tts_hint.pitch - 0.5f);
		kf_log("VL,SOUL,TTS,tag=%s,rate=%.2f,pitch=%d\r\n",
		       snap.emo.anim_tag, tts->speech_rate, tts->pitch);
	}
	if (llm) {
		snprintf(s_sys_dyn, sizeof(s_sys_dyn), "%s%s",
			 s_base ? s_base : "", soul_prompt_suffix());
		llm->system = s_sys_dyn;
		if (kf_dbg_get()) {
			kf_log("VL,SOUL,PROMPT,len=%d\r\n",
			       (int)strlen(s_sys_dyn));
			kf_log_raw(s_sys_dyn);
			kf_log_raw("\r\n");
		}
	}
}

/* ---- 1Hz tick（唯一写者） ---- */

static void soul_tick(void)
{
	struct kd_inputs din;
	struct kd_output dout;
	struct ke_inputs kin;
	struct ke_output kout;
	struct ki_context ictx;
	struct ki_factors fact;
	struct ki_decision idec;
	struct ki_config cfg = *ki_config_default();
	enum kd_event evt;
	enum ke_receipt rc;
	enum convo_state st = convo_state_get();

	portENTER_CRITICAL(&s_mux);
	evt = s_pend_evt;
	rc = s_pend_receipt;
	s_pend_evt = KD_EVT_NONE;
	s_pend_receipt = KE_RECEIPT_NONE;
	portEXIT_CRITICAL(&s_mux);

	/* 驱力输入（28 号 §9.3 映射）。active 为预留位（评审 C2：当前
	 * 恒代谢口径，rest 代谢不因表达暂停）；quiet=待机拍 rest 恢复 */
	memset(&din, 0, sizeof(din));
	din.tick = s_ticks;
	din.active = (st == CONVO_SPEAKING || st == CONVO_THINKING);
	din.quiet = (st == CONVO_IDLE || st == CONVO_OFF);
	din.event = evt;
	kd_step(s_ticks, &din, &dout);

	portENTER_CRITICAL(&s_mux);
	{
		float m = s_pend_mood;

		s_pend_mood = 0.0f;
		portEXIT_CRITICAL(&s_mux);
		if (m != 0.0f) {
			struct ke_reappraisal r = {
				.delta = m,
				.receipt_id = ++s_mood_seq,
			};

			kf_log("VL,SOUL,MOOD,seq=%u,req=%.2f,rc=%d\r\n",
			       (unsigned)s_mood_seq, m,
			       ke_reappraisal_request(&r));
		}
	}

	/* 情感受态：驱力误差全供（kd 常供 ⇒ have_drive 恒真）；
	 * novelty/uncertainty 无源（W2/W6 接入前保持，D7 位不置） */
	memset(&kin, 0, sizeof(kin));
	kin.have_drive = true;
	kin.drive_error_sum = dout.error_sum;
	kin.drive_error_max = dout.error_max;
	kin.receipt = rc;
	/* §9.3：真实互动拍 → social_bond（ROUND_FAIL 不算——体验差） */
	kin.social_event = (evt == KD_EVT_ROUND_OK || evt == KD_EVT_BARGE ||
			    evt == KD_EVT_VOICE || evt == KD_EVT_TOUCH);
	ke_step(s_ticks, &kin, &kout);

	/* W1 表达策略：IDLE/OFF 且已配置才接管底色（未配置引导/活动
	 * 四态归 KEX 桥）；驻留 8 拍 + 活动结束首拍立即换 */
	{
		static enum convo_state s_prev_st;
		static uint32_t s_last_emo_tick;
		static uint32_t s_quiet_ticks;
		static char s_cur_clip[24];
		bool idle_now = (st == CONVO_IDLE || st == CONVO_OFF);
		bool configured = s_configured;

		if (!idle_now) {
			s_quiet_ticks = 0;
		} else if (evt != KD_EVT_NONE || rc != KE_RECEIPT_NONE) {
			s_quiet_ticks = 0;
		} else if (din.quiet) {
			s_quiet_ticks++;
		}
		if (idle_now && configured) {
			bool activity_ended =
				idle_now && !s_prev_idle(s_prev_st);
			const char *clip = emo_clip_pick(&kout, &dout,
					s_quiet_ticks >= SOUL_ASPLEEP_TICKS);

			if (clip && strcmp(clip, s_cur_clip) != 0 &&
			    (activity_ended || s_ticks - s_last_emo_tick >=
			     SOUL_EMO_DWELL)) {
				strlcpy(s_cur_clip, clip, sizeof(s_cur_clip));
				s_last_emo_tick = s_ticks;
				emote_stage_set_emo(clip);
			}
		}
		s_prev_st = st;
	}

	/* 点火打分（W0 只观测）：affect_motive 接 urgency；candidate 仅遥测 */
	memset(&ictx, 0, sizeof(ictx));
	ictx.user_speaking = convo_active();
	memset(&fact, 0, sizeof(fact));
	fact.affect_motive = kout.state.urgency;
	ki_step(&ictx, &cfg, &fact, &idec);

	/* 发布快照（锁内拷贝） */
	if (s_lock &&
	    xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
		s_snap.tick = s_ticks;
		s_snap.drive = dout;
		s_snap.emo = kout;
		s_snap.ign = idec;
		xSemaphoreGive(s_lock);
	}
	s_ticks++;

	/* W3 思维触发闸：每拍纯检查；云调用在 think 任务（单飞） */
	{
		uint8_t h = bin_hash(&kout);

		if (s_bin_valid && h != s_bin_prev)
			s_bin_changed = true;	/* 跃迁沿（非电平） */
		s_bin_prev = h;
		s_bin_valid = true;
	}
	{
		struct ki_think_ctx tc = {
			.arousal = kout.state.arousal,
			.urgency_max = dout.error_max,
			.pending_command = convo_active(),
			.bin_changed = s_bin_changed,
			.new_event = false,     /* 由 soul_think 维护 */
			.score = idec.score,
		};

		soul_think_gate_ctx(&tc);
		{
			struct ki_think_gate tg;

			ki_think_gate(&tc, &tg);
			bool forced = s_force_think;

			if (forced) {
				s_force_think = false;
				tg.trigger = true;
				tg.blocked_mask = 0;
			}
			if (tg.trigger && s_ticks < 60 &&
			    !forced) {
				tg.trigger = false;  /* 开机静默 60s：
					    * WiFi 未稳/不被打扰 */
				kf_ev("ki", "boot grace");
			}
			if (tg.trigger) {
				if (soul_think_start(tg.reason_mask,
						     forced, idec.score))
					s_bin_changed = false;
				else
					kf_ev("ki", "busy drop");
			} else if (tg.blocked_mask && kf_dbg_get()) {
				static uint32_t s_blk_log;

				if (s_ticks - s_blk_log >= 60) {
					s_blk_log = s_ticks;
					kf_log("VL,KI,GATE,blocked=%u"
					       "\r\n", tg.blocked_mask);
				}
			}
		}
	}

	/* SOUL 遥测：debug 态逐拍；常态 15s 一行（KF 单写者通道） */
	if (kf_dbg_get() || s_ticks % SOUL_TELEM_QUIET_S == 0) {
		kf_log("VL,SOUL,t=%u,st=%d,v=%.3f,a=%.2f,u=%.2f,tag=%s,"
		       "E=%.3f,em=%.2f,sev=%d%d%d,ign=%d,score=%.2f\r\n",
		       (unsigned)s_ticks, (int)st,
		       kout.state.valence, kout.state.arousal,
		       kout.state.urgency, kout.anim_tag,
		       dout.error_sum, dout.error_max,
		       dout.sev[0], dout.sev[1], dout.sev[2],
		       idec.candidate, idec.score);
	}
	kf_beat("soul");
}

static void soul_task(void *arg)
{
	TickType_t last = xTaskGetTickCount();

	(void)arg;
	for (;;) {
		vTaskDelayUntil(&last, pdMS_TO_TICKS(SOUL_TICK_MS));
		soul_tick();
	}
}

int soul_start(void)
{
	static StackType_t *s_stk;
	static StaticTask_t s_tcb;

	if (s_lock)
		return 0;			/* 幂等 */
	s_lock = xSemaphoreCreateMutex();
	if (!s_lock)
		return -1;
	kd_init(NULL);
	kd_reset();
	ke_reset(NULL);
	s_configured = (ksec_state() &
			(KSEC_BIT_ASR | KSEC_BIT_LLM)) ==
		       (KSEC_BIT_ASR | KSEC_BIT_LLM);	/* app_main 上下文
		 * （内部栈）读 NVS 安全；运行期改 key 走 OOBE 重启 */
	memset(&s_snap, 0, sizeof(s_snap));
	/* 灵魂拍低优先级：饥不死（vTaskDelayUntil 让出）、不抢音频/EAF */
	if (!kf_task_psram("soul", 3072, soul_task, NULL, 3, -1,
			   (void **)&s_stk, &s_tcb)) {
		vSemaphoreDelete(s_lock);
		s_lock = NULL;
		kf_log("VL,SOUL,FAIL,task\r\n");
		return -1;
	}
	kf_log("VL,SOUL,START,1hz\r\n");
	kf_ev("soul", "start");
	return 0;
}
