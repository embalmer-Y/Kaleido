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
#include "kaleido_core.h"
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

static SemaphoreHandle_t s_lock;	/* 快照拷贝锁 */
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
