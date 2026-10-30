/*
 * kaleido_emotion.c — 九维感受态动力学实现
 *
 * 移植自 Galatea L05 affect（MIT；vendor-licenses.md 义务 1：保留出处）。
 * 全部确定性：零 RNG、零墙钟、零分配。
 * 返工记录（评审 A1/S6/G1/G2/B2/B5，2026-10-25；W0-2 对源偏差修正
 * D1-D7，2026-10-28，依据 docs/research/28 §4.3 逐项审计）：
 *  - A1：valence 差分先算后更新 s_prev（!have_drive 不更新）
 *  - S6：积分器移文件级 static，ke_reset 全清（含 init 注入）
 *  - G1：回执三态（无事件拍保持）
 *  - B2：ke_step 拆三段
 *  - D1：drive_error_sum 语义（源 E=Σw·e 无归一；mean 为移植发明，
 *        曾把 valence 域压 1/3 且永不饱和）
 *  - D2/D3：伦理阈仿真定档 −0.35/3 拍（见下注）+ ke_ethics_clear()
 *  - D4：重评价一次性精确施加 + applied 审计（TTL 斜坡废）
 *  - D5：!have_drive 拍 arousal/urgency 保持（源缺席语义，禁衰减）
 *  - D6：ke_hysteresis 半带宽（源 band=0.1 ⇒ 0.55/0.45 边界含）
 *  - D7：novelty/uncertainty 有效性位（无效拍保持，禁主动衰减）
 *
 * 伦理阈定档注（D2/D3，V-48a 仿真）：契约域 E∈[0,3]（三驱力等权）
 * 内瞬时全崩 raw=−3 → 轨迹 −0.75/−0.5625/−0.4219（连续 3 拍 ≤−0.35）
 * 恰好闩锁；双驱力崩最深 −0.5 仅 2 拍不触发——单轮失败不误闩。
 * 可达性收敛（评审 C9）：kd 脉冲全为正向恢复、代谢 1e-4 量级，现
 * 动力学下唯一自然通路是 reappraisal 连续负注入（≥6 拍 −0.2 →
 * published 稳态 −0.45 以下）——符合"极端持续负面=认知层持续自贬"
 * 安全底座语义。源阈 −0.8/64 拍双条件为构造不可达，Kaleido 事件直注
 * 语义下按上法定档，正式值留 H6 产品评审。
 */
#include "kaleido_emotion.h"

#include <string.h>

/* ---- 冻结参数 ---- */

#define KE_TAU_INT	4.0f
#define KE_TAU_DEC	16.0f
#define KE_URG_ON	0.10f
#define KE_URG_OFF	0.05f /* 激活门迟滞：Kaleido 附加（防点火抖动），
				 非源语义（源纯比较 >0.1） */
#define KE_ETHICS_V	-0.35f /* 仿真定档，见文件头注 */
#define KE_ETHICS_N	3u     /* 连续 ≤V 拍数 */
#define KE_REAP_MAX	0.2f
#define KE_BOND_DECAY	1024.0f /* ≈17min 时间常数："冷得慢"陪伴语义；
				 * 无冻结来源，参数评审 H6 一并复核 */
#define KE_BOND_STEP	0.125f

/* ---- 引擎状态（文件级，ke_reset 可全清：S6）---- */

static struct ke_state s_st;
static bool s_inited;
static bool s_urg_on;
static uint32_t s_ethics_cnt;
static float s_prev_e;
static float s_prev_emax;      /* D5：缺席拍 arousal/urgency 的保持源 */
static float s_val, s_urg, s_per, s_ctl, s_nov, s_unc, s_bond;
static float s_reap_pending;   /* 待施加（下一拍生效） */
static uint64_t s_reap_receipt;
static float s_reap_applied;   /* 最近一次实际施加量（D4 审计） */
static bool s_reap_done;

float ke_lowpass(float s, float x, float tau)
{
	return s + (x - s) / tau;
}

/* 半带宽迟滞（D6）：ON ≥ 0.5+band/2、OFF ≤ 0.5−band/2，边界含 */
bool ke_hysteresis(bool on, float x, float band)
{
	float h = band * 0.5f;

	if (!on && x >= 0.5f + h)
		return true;
	if (on && x <= 0.5f - h)
		return false;
	return on;
}

int ke_reappraisal_request(const struct ke_reappraisal *req)
{
	if (!req || req->receipt_id == 0)
		return -1;
	if (req->delta > KE_REAP_MAX || req->delta < -KE_REAP_MAX)
		return -2;
	s_reap_pending = req->delta;   /* 覆盖未生效的上一请求（单槽） */
	s_reap_receipt = req->receipt_id;
	s_reap_done = false;
	return 0;
}

bool ke_reappraisal_receipt(uint64_t receipt_id)
{
	return receipt_id != 0 && receipt_id == s_reap_receipt &&
	       s_reap_done;
}

float ke_reappraisal_applied(void)
{
	return s_reap_applied;
}

void ke_ethics_clear(void)
{
	s_st.ethics_latched = false;
	s_ethics_cnt = 0;
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

/* valence：内稳态误差变化率负值（A1：先差分后更新）。
 * 误差水平只表达在 arousal/urgency；valence 只表达变化率
 * （"持续饥渴不难过，恶化中才难过"，源测试钉死语义） */
static void step_valence(const struct ke_inputs *in)
{
	float raw_v = 0.0f;

	if (in->have_drive) {
		raw_v = -(in->drive_error_sum - s_prev_e);
		s_prev_e = in->drive_error_sum;
	}
	s_val = ke_lowpass(s_val, raw_v, KE_TAU_INT);
	s_st.valence = clampf(s_val, -1.0f, 1.0f);
}

/* arousal/urgency/persistence/controllability/novelty/uncertainty/bond */
static void step_rest(const struct ke_inputs *in)
{
	float urg_raw;
	float vabs = s_st.valence < 0 ? -s_st.valence : s_st.valence;

	/* D5：缺席拍保持（源 dynamics.py:96-108——误差保持 ⇒ 积分器
	 * 保持；arousal/urgency 吃 s_prev_emax，缺席拍不更新它即保持） */
	urg_raw = (s_prev_emax - KE_URG_ON) / (1.0f - KE_URG_ON);

	if (urg_raw < 0.0f)
		urg_raw = 0.0f;
	s_urg = ke_lowpass(s_urg, urg_raw, KE_TAU_INT);
	s_st.urgency = clampf(s_urg, 0.0f, 1.0f);
	s_urg_on = s_urg_on ? (s_st.urgency > KE_URG_OFF) :
			       (s_st.urgency > KE_URG_ON);

	s_st.arousal = clampf(ke_lowpass(s_st.arousal, s_prev_emax,
					 KE_TAU_INT), 0.0f, 1.0f);

	s_per = ke_lowpass(s_per, clampf(vabs, 0.0f, 0.8f) / 0.8f,
			   KE_TAU_DEC);
	s_st.persistence = clampf(s_per, 0.0f, 1.0f);

	/* G1：三态回执——无事件拍保持 controllability；一轮结算一次 */
	if (in->receipt != KE_RECEIPT_NONE) {
		float x = in->receipt == KE_RECEIPT_SUCCESS ? 1.0f :
							 0.0f;

		s_ctl = ke_lowpass(s_ctl, x, KE_TAU_DEC);
	}
	s_st.controllability = clampf(s_ctl, 0.0f, 1.0f);

	/* D7：有效性位——无效拍保持（禁喂 0 造成的主动衰减） */
	if (in->have_residual)
		s_nov = ke_lowpass(s_nov, in->sensory_residual_mean,
				   KE_TAU_INT);
	s_st.novelty = clampf(s_nov, 0.0f, 1.0f);

	if (in->have_uncertainty)
		s_unc = ke_lowpass(s_unc, in->belief_uncertainty_mean,
				   KE_TAU_INT);
	s_st.uncertainty = clampf(s_unc, 0.0f, 1.0f);

	if (in->social_event)
		s_bond = clampf(s_bond + KE_BOND_STEP, 0.0f, 1.0f);
	else
		s_bond = ke_lowpass(s_bond, 0.0f, KE_BOND_DECAY);
	s_st.social_bond = clampf(s_bond, 0.0f, 1.0f);

	/* 伦理闩锁：低 valence 连拍计数，命中即粘滞（安全语义；
	 * 解锁唯 ke_ethics_clear） */
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
	/* D4：重评价一次性施加（进积分器，随本拍低通走） */
	if (!s_reap_done && s_reap_receipt != 0) {
		s_val += s_reap_pending;
		s_reap_applied = s_reap_pending;
		s_reap_pending = 0.0f;
		s_reap_done = true;
	}
	if (in->have_drive)
		s_prev_emax = in->drive_error_max;   /* D5：缺席拍保持 */
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
	s_prev_emax = 0.0f;
	s_val = s_urg = s_per = s_ctl = s_nov = s_unc = s_bond = 0.0f;
	s_reap_pending = 0.0f;
	s_reap_receipt = 0;
	s_reap_applied = 0.0f;
	s_reap_done = false;
	if (init)
		s_st = *init;
}
