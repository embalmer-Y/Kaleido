/*
 * kaleido_emotion.c — 九维感受态动力学实现
 *
 * 移植自 Galatea L05 affect（MIT；vendor-licenses.md 义务 1：保留出处）。
 * 全部确定性：零 RNG、零墙钟、零分配。
 * 返工记录（评审 A1/S6/G1/G2/B2/B5，2026-10-25）：
 *  - A1：valence 差分先算后更新 s_prev（!have_drive 不更新）
 *  - S6：积分器移文件级 static，ke_reset 全清（含 init 注入）
 *  - G1：回执三态（无事件拍保持）
 *  - G2：two_step stage1 跨拍保持
 *  - B5：tts rate 修为 [0.8,1.2]；删除无效 static/死标志
 *  - B2：ke_step 拆三段
 */
#include "kaleido_emotion.h"

#include <string.h>

/* ---- 冻结参数 ---- */

#define KE_TAU_INT	4.0f
#define KE_TAU_DEC	16.0f
#define KE_URG_ON	0.10f
#define KE_URG_OFF	0.05f
#define KE_ETHICS_V	-0.20f /* 可达域阈值：0→1 误差跳变单拍 dip≈-0.25；
				  正式阈值待负责人裁决（产品评审 H6） */
#define KE_ETHICS_N	4u  /* 可达窗口：阶跃后 ≤-0.2 约 6 拍，取 4 */
#define KE_REAP_MAX	0.2f
#define KE_REAP_TTL	8u
#define KE_BOND_DECAY	1024.0f
#define KE_BOND_STEP	0.125f

/* ---- 引擎状态（文件级，ke_reset 可全清：S6）---- */

static struct ke_state s_st;
static bool s_inited;
static bool s_urg_on;
static uint32_t s_ethics_cnt;
static float s_prev_e;
static float s_val, s_urg, s_per, s_ctl, s_nov, s_unc, s_bond;
static uint8_t s_reap_ttl;
static float s_reap_delta;
static float s_reap_stage1;   /* two_step 第一级（跨拍保持：G2） */
static uint64_t s_reap_receipt;
static bool s_reap_done;

float ke_lowpass(float s, float x, float tau)
{
	return s + (x - s) / tau;
}

bool ke_hysteresis(bool on, float x, float band)
{
	if (!on && x > 0.5f + band)
		return true;
	if (on && x < 0.5f - band)
		return false;
	return on;
}

int ke_reappraisal_request(const struct ke_reappraisal *req)
{
	if (!req || req->ttl == 0 || req->receipt_id == 0)
		return -1;
	if (req->delta > KE_REAP_MAX || req->delta < -KE_REAP_MAX)
		return -2;
	s_reap_delta = req->delta;
	s_reap_stage1 = 0.0f;
	s_reap_ttl = req->ttl > KE_REAP_TTL ? KE_REAP_TTL : req->ttl;
	s_reap_receipt = req->receipt_id;
	s_reap_done = false;
	return 0;
}

bool ke_reappraisal_receipt(uint64_t receipt_id)
{
	return receipt_id != 0 && receipt_id == s_reap_receipt &&
	       s_reap_done;
}

static const char *anim_tag(float v, float a)
{
	if (v >= 0.15f)
		return a >= 0.55f ? "happy" : "relaxed";
	if (v <= -0.15f)
		return a >= 0.55f ? "angry" : "sad";
	if (a >= 0.55f)
		return "surprised";
	if (a >= 0.30f)
		return "confused";
	return "sleepy";
}

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

/* valence：内稳态误差变化率负值（A1：先差分后更新）+ two_step 重评价 */
static void step_valence(const struct ke_inputs *in)
{
	float raw_v = 0.0f;

	if (in->have_drive) {
		raw_v = -(in->drive_error_mean - s_prev_e);
		s_prev_e = in->drive_error_mean;
	}
	s_val = ke_lowpass(s_val, raw_v, KE_TAU_INT);
	if (s_reap_ttl) {
		s_reap_stage1 = ke_lowpass(s_reap_stage1, s_reap_delta,
					   KE_TAU_INT);
		s_val = ke_lowpass(s_val, s_reap_stage1, KE_TAU_INT);
		s_reap_ttl--;
		if (!s_reap_ttl)
			s_reap_done = true;
	}
	s_st.valence = clampf(s_val, -1.0f, 1.0f);
}

/* arousal/urgency/persistence/controllability/novelty/uncertainty/bond */
static void step_rest(const struct ke_inputs *in)
{
	float e_max = in->have_drive ? in->drive_error_max : 0.0f;
	float urg_raw = (e_max - KE_URG_ON) / (1.0f - KE_URG_ON);
	float vabs = s_st.valence < 0 ? -s_st.valence : s_st.valence;

	if (urg_raw < 0.0f)
		urg_raw = 0.0f;
	s_urg = ke_lowpass(s_urg, urg_raw, KE_TAU_INT);
	s_st.urgency = clampf(s_urg, 0.0f, 1.0f);
	s_urg_on = s_urg_on ? (s_st.urgency > KE_URG_OFF) :
			       (s_st.urgency > KE_URG_ON);

	s_st.arousal = clampf(ke_lowpass(s_st.arousal, e_max,
					 KE_TAU_INT), 0.0f, 1.0f);

	s_per = ke_lowpass(s_per, clampf(vabs, 0.0f, 0.8f) / 0.8f,
			   KE_TAU_DEC);
	s_st.persistence = clampf(s_per, 0.0f, 1.0f);

	/* G1：三态回执——无事件保持，成功→1，失败→0（τ=τ_dec） */
	if (in->receipt != KE_RECEIPT_NONE) {
		float x = in->receipt == KE_RECEIPT_SUCCESS ? 1.0f :
							 0.0f;

		s_ctl = ke_lowpass(s_ctl, x, KE_TAU_DEC);
	}
	s_st.controllability = clampf(s_ctl, 0.0f, 1.0f);

	s_st.novelty = clampf(ke_lowpass(s_st.novelty,
					 in->sensory_residual_mean,
					 KE_TAU_INT), 0.0f, 1.0f);
	s_st.uncertainty = clampf(ke_lowpass(s_st.uncertainty,
						 in->belief_uncertainty_mean,
						 KE_TAU_INT), 0.0f, 1.0f);

	if (in->social_event)
		s_bond = clampf(s_bond + KE_BOND_STEP, 0.0f, 1.0f);
	else
		s_bond = ke_lowpass(s_bond, 0.0f, KE_BOND_DECAY);
	s_st.social_bond = clampf(s_bond, 0.0f, 1.0f);

	/* 伦理闩锁：低 valence 连拍计数，命中即粘滞（安全语义） */
	if (s_st.valence <= KE_ETHICS_V) {
		if (s_ethics_cnt < KE_ETHICS_N)
			s_ethics_cnt++;
		if (s_ethics_cnt >= KE_ETHICS_N)
			s_st.ethics_latched = true;
	} else {
		s_ethics_cnt = 0;
	}
}

void ke_step(uint64_t tick, const struct ke_inputs *in,
	     struct ke_output *out)
{
	if (!in)
		return;
	if (!s_inited) {
		s_val = s_st.valence;
		s_urg = s_st.urgency;
		s_per = s_st.persistence;
		s_ctl = s_st.controllability;
		s_nov = s_st.novelty;
		s_unc = s_st.uncertainty;
		s_bond = s_st.social_bond;
		s_inited = true;
	}
	step_valence(in);
	step_rest(in);
	(void)tick;
	if (!out)
		return;
	out->state = s_st;
	strlcpy(out->anim_tag, anim_tag(s_st.valence, s_st.arousal),
		sizeof(out->anim_tag));
	out->tts_hint.rate = 0.8f + 0.4f * s_st.arousal; /* [0.8,1.2] */
	out->tts_hint.pitch = 2.0f * s_st.valence;
	out->ignition_candidate = s_urg_on && s_st.urgency > KE_URG_ON;
	out->hyst_saturated = 0;
}

const struct ke_state *ke_state_snapshot(void)
{
	return &s_st;
}

void ke_reset(const struct ke_state *init)
{
	memset(&s_st, 0, sizeof(s_st));
	s_inited = false;
	s_urg_on = false;
	s_ethics_cnt = 0;
	s_prev_e = 0.0f;
	s_val = s_urg = s_per = s_ctl = s_nov = s_unc = s_bond = 0.0f;
	s_reap_ttl = 0;
	s_reap_delta = 0.0f;
	s_reap_stage1 = 0.0f;
	s_reap_receipt = 0;
	s_reap_done = false;
	if (init)
		s_st = *init;
}
