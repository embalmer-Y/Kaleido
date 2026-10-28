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
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "convo_fsm.h"
#include "emote_stage.h"
#include "kaleido_core.h"
#include "kaleido_secret.h"
#include "soul.h"

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
#define SOUL_ASPLEEP_TICKS	600	/* 连续安静 600 拍（~10min；压缩档
					 * 10s）→ 睡着（asleep_215s） */

static SemaphoreHandle_t s_lock;	/* 快照拷贝锁 */
static bool s_configured;		/* OOBE 配置态（start 时缓存——
	 * PSRAM 栈任务禁碰 NVS/flash：cache 关断窗口断言
	 * esp_task_stack_is_sane_cache_disabled，W1 首板实证） */
static struct soul_snap s_snap;
static uint32_t s_ticks;

/* ---- 钩子 pending（临界区承载；BARGE > ROUND_* > VOICE/TOUCH） ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static enum kd_event s_pend_evt;
static enum ke_receipt s_pend_receipt;	/* 一轮结算一次（G1 口径） */
static int64_t s_last_voice_us;

static void pend_push(enum kd_event evt, enum ke_receipt rc)
{
	portENTER_CRITICAL(&s_mux);
	if (evt != KD_EVT_NONE) {
		/* 强信号覆盖弱信号；ROUND_* 互不覆盖对方（最新结算生效） */
		if (evt == KD_EVT_BARGE || s_pend_evt != KD_EVT_BARGE)
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
}

void soul_on_barge(void)
{
	pend_push(KD_EVT_BARGE, KE_RECEIPT_NONE);
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
}

void soul_fast_drain(bool on)
{
	kd_init(on ? &s_fast_bench : NULL);
	/* kd_init 仅换参数指针（const 表），水平保持——压缩前后连续 */
	kf_log("VL,SOUL,FAST,%s\r\n", on ? "on" : "off");
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

	/* 驱力输入（28 号 §9.3 映射：表达中=active 代谢；待机=quiet 恢复） */
	memset(&din, 0, sizeof(din));
	din.tick = s_ticks;
	din.active = (st == CONVO_SPEAKING || st == CONVO_THINKING);
	din.quiet = (st == CONVO_IDLE || st == CONVO_OFF);
	din.event = evt;
	kd_step(s_ticks, &din, &dout);

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
			bool activity_ended = idle_now && !s_prev_idle(s_prev_st);
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
