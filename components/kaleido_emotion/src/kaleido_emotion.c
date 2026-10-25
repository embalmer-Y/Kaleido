/*
 * kaleido_emotion.c — 九维感受态动力学实现
 *
 * 移植自 Galatea L05 affect（MIT；见 vendor-licenses.md 义务 1：
 * 本文件头保留原项目出处）。全部确定性：零 RNG、零墙钟、零分配。
 */
#include "kaleido_emotion.h"

#include <string.h>

/* ---- 冻结参数（Galatea c2_affect.profile.yaml + Kaleido 裁剪）---- */

#define KE_TAU_INT	4.0f   /* 快谱积分                */
#define KE_TAU_DEC	16.0f  /* 慢谱衰减                */
#define KE_URG_BAND	0.1f   /* urgency 激活带（迟滞）  */
#define KE_ETHICS_V	-0.9f  /* 伦理闩锁 valence 阈     */
#define KE_ETHICS_N	32u    /* 连续拍数才闩锁          */
#define KE_REAP_MAX	0.2f   /* 重评价有界              */
#define KE_REAP_TTL	8u     /* 重评价生效窗（拍）      */
#define KE_BOND_DECAY	1024.0f /* social_bond 慢衰减    */
#define KE_BOND_STEP	0.125f /* 互动命中单次增量上界    */

static struct ke_state s_st;
static float s_prev_error_mean;   /* valence 的 E 变化率基线 */
static bool s_urg_on;             /* urgency 迟滞态          */
static uint32_t s_ethics_cnt;
static uint8_t s_reap_ttl;
static float s_reap_delta;
static uint64_t s_reap_receipt;
static bool s_reap_done;

/* 重评价 two_step 构造式：低通²后回注（18号 §2.2 two_step） */
static float s_reap_stage1;

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
		return -2;                    /* 有界拒绝         */
	s_reap_delta = req->delta;
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

/* valence×arousal 平面 → 基础情绪（25号 §2.3 plane_map 的固化为默认） */
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

void ke_step(uint64_t tick, const struct ke_inputs *in,
	     struct ke_output *out)
{
	float e_mean = s_prev_error_mean;
	float e_max = 0.0f;
	static float val_int;   /* valence 低通级            */
	static float urg_int;
	static float per_int;
	static float ctl_int;
	static float nov_int;
	static float unc_int;
	static float bond_int;
	static bool inited;

	if (!inited) {
		val_int = s_st.valence;
		urg_int = s_st.urgency;
		per_int = s_st.persistence;
		ctl_int = s_st.controllability;
		nov_int = s_st.novelty;
		unc_int = s_st.uncertainty;
		bond_int = s_st.social_bond;
		inited = true;
	}

	/* 输入缺省语义：驱力无效 → 保持上一有效值 */
	if (in->have_drive) {
		e_mean = in->drive_error_mean;
		e_max = in->drive_error_max;
		s_prev_error_mean = e_mean;
	}

	/* valence：内稳态误差变化率的负值（改善正/恶化负） */
	float raw_v = -(e_mean - s_prev_error_mean);
	val_int = ke_lowpass(val_int, raw_v, KE_TAU_INT);
	if (s_reap_ttl) {
		/* two_step：两级低通后作为有界回注 */
		s_reap_stage1 = ke_lowpass(0.0f, s_reap_delta,
					   KE_TAU_INT);
		val_int = ke_lowpass(val_int, s_reap_stage1,
				     KE_TAU_INT);
		s_reap_ttl--;
		if (!s_reap_ttl)
			s_reap_done = true;
	}
	s_st.valence = clampf(val_int, -1.0f, 1.0f);

	/* arousal / urgency（band 上方线性归一；迟滞防抖） */
	float urg_raw = (e_max - KE_URG_BAND) / (1.0f - KE_URG_BAND);

	urg_raw = urg_raw > 0.0f ? urg_raw : 0.0f;
	urg_int = ke_lowpass(urg_int, urg_raw, KE_TAU_INT);
	s_st.urgency = clampf(urg_int, 0.0f, 1.0f);
	/* 激活门 0.10 / 释放门 0.05（迟滞防抖，承 Q-L05-03 语义） */
	s_urg_on = s_urg_on ? (s_st.urgency > 0.05f) :
			       (s_st.urgency > 0.10f);

	s_st.arousal = clampf(ke_lowpass(s_st.arousal, e_max,
					 KE_TAU_INT), 0.0f, 1.0f);

	/* persistence：|valence|/0.8 慢谱；伦理闩锁判定 */
	per_int = ke_lowpass(per_int,
			     clampf(s_st.valence < 0 ?
				    -s_st.valence : s_st.valence,
				    0.0f, 0.8f) / 0.8f, KE_TAU_DEC);
	s_st.persistence = clampf(per_int, 0.0f, 1.0f);
	if (s_st.valence <= KE_ETHICS_V) {
		if (s_ethics_cnt < KE_ETHICS_N)
			s_ethics_cnt++;
		if (s_ethics_cnt >= KE_ETHICS_N)
			s_st.ethics_latched = true;
	} else {
		s_ethics_cnt = 0;
	}

	/* controllability：执行回执（无事件拍保持） */
	if (in->action_receipt_success)
		ctl_int = ke_lowpass(ctl_int, 1.0f, KE_TAU_DEC);
	else if (!in->action_receipt_success)
		ctl_int = ke_lowpass(ctl_int, 0.0f, KE_TAU_DEC * 4.0f);
	s_st.controllability = clampf(ctl_int, 0.0f, 1.0f);

	/* novelty / uncertainty：感觉/信念误差供给 */
	s_st.novelty = clampf(ke_lowpass(s_st.novelty,
					 in->sensory_residual_mean,
					 KE_TAU_INT), 0.0f, 1.0f);
	s_st.uncertainty = clampf(ke_lowpass(s_st.uncertainty,
					     in->belief_uncertainty_mean,
					     KE_TAU_INT), 0.0f, 1.0f);

	/* social_bond：互动命中 +0.125 上界 1.0，慢衰减向 0 */
	if (in->social_event)
		bond_int = ke_lowpass(bond_int, bond_int + KE_BOND_STEP,
				      1.0f);
	else
		bond_int = ke_lowpass(bond_int, 0.0f, KE_BOND_DECAY);
	s_st.social_bond = clampf(bond_int, 0.0f, 1.0f);

	(void)tick;
	if (!out)
		return;
	out->state = s_st;
	strlcpy(out->anim_tag, anim_tag(s_st.valence, s_st.arousal),
		sizeof(out->anim_tag));
	out->tts_hint.rate = 1.0f + 0.2f * s_st.arousal;
	out->tts_hint.pitch = 2.0f * s_st.valence;
	out->ignition_candidate = s_urg_on && s_st.urgency > 0.1f;
	out->hyst_saturated = 0;
}

const struct ke_state *ke_state_snapshot(void)
{
	return &s_st;
}

void ke_reset(const struct ke_state *init)
{
	memset(&s_st, 0, sizeof(s_st));
	s_prev_error_mean = 0.0f;
	s_urg_on = false;
	s_ethics_cnt = 0;
	s_reap_ttl = 0;
	s_reap_delta = 0.0f;
	s_reap_receipt = 0;
	s_reap_done = false;
	if (init)
		s_st = *init;
}
