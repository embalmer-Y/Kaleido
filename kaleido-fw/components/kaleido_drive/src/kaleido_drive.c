/*
 * kaleido_drive.c — 三驱力内稳态实现（28 号 §9.1 草案落地）
 *
 * 返工记录（W0-1，2026-10-28）：
 *  - 水平更新 level ← clamp01(level − metabolic + inflow)；无回归项
 *  - 误差 e = 1 − level 不钳位（单侧性由值域构造保证）
 *  - 聚合 sum/max 无归一（源 pipeline.py:102 同构；D1 修复的供给侧）
 *
 * 参数锚（28 号 §9.2，C 级起步值——冻结表集中管理，产品调参走
 * kd_init 覆写 + 版本化 profile，不散落魔术数）：
 *  - social 代谢 2e-4/拍（满→HIGH 约 42min）；stim 1e-4（~1.4h，
 *    比 social 慢——独处发呆可接受）；rest 3e-4（连续表达 ~1h 显疲态）
 *  - 脉冲锚 m0 action_energy_delta：ROUND_OK +0.15（=eat）、
 *    BARGE/TOUCH +0.05、ROUND_FAIL +0.03（互动发生但体验差）、
 *    VOICE +0.005/拍（=look 的 1/20；调用方限频 ≥5s 防刷——源对应物
 *    是 eat 的 stored_food 前置："恢复永远需要前置条件"）
 *  - stim 事件 +0.10（ROUND_OK/BARGE）；rest 静息恢复 +0.01
 */
#include "kaleido_drive.h"

#include <stddef.h>

static const struct kd_params s_def = {
	.metabolic = { 2e-4f, 1e-4f, 3e-4f },
	.pulse = {
		[KD_EVT_NONE]	   = 0.0f,
		[KD_EVT_ROUND_OK]  = 0.15f,
		[KD_EVT_ROUND_FAIL] = 0.03f,
		[KD_EVT_BARGE]	   = 0.05f,
		[KD_EVT_VOICE]	   = 0.005f,
		[KD_EVT_TOUCH]	   = 0.05f,
	},
	.stim_event = 0.10f,
	.rest_recover = 0.01f,
	.band = 0.1f,
};

static const struct kd_params *s_p;
static float s_level[KD_N];
static float s_prev_error[KD_N];

static float clamp01(float x)
{
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

void kd_init(const struct kd_params *p)
{
	s_p = p ? p : &s_def;
}

void kd_reset(void)
{
	int d;

	if (!s_p)
		s_p = &s_def;
	for (d = 0; d < KD_N; d++) {
		s_level[d] = 1.0f;      /* 满水平 = 初始零误差（源 yaml:26） */
		s_prev_error[d] = 0.0f;
	}
}

/* 源 severity_of：≥0.8 CRITICAL；≥0.5 HIGH；> band LOW；否则 NONE */
static enum kd_sev severity_of(float e, float band)
{
	if (e >= 0.8f)
		return KD_SEV_CRITICAL;
	if (e >= 0.5f)
		return KD_SEV_HIGH;
	if (e > band)
		return KD_SEV_LOW;
	return KD_SEV_NONE;
}

void kd_step(uint64_t tick, const struct kd_inputs *in,
	     struct kd_output *out)
{
	int d;
	enum kd_event evt;

	if (!in || !out)
		return;
	if (!s_p)
		kd_init(NULL);
	/* 显式无符号比较（评审 C6：不依赖枚举符号实现定义） */
	evt = (unsigned)in->event < KD_EVT_CNT ? in->event : KD_EVT_NONE;

	for (d = 0; d < KD_N; d++) {
		float inflow = 0.0f;
		float level, e;

		/* 恢复项：rest 走 quiet/charging；social 走事件脉冲；
		 * stim 只认 ROUND_OK/BARGE（好奇心事件语义，28 号 §9.2） */
		if (d == KD_REST) {
			if (in->quiet || in->charging)
				inflow = s_p->rest_recover;
		} else if (d == KD_SOCIAL) {
			inflow = s_p->pulse[evt];
		} else if (evt == KD_EVT_ROUND_OK || evt == KD_EVT_BARGE) {
			inflow = s_p->stim_event;
		}

		level = clamp01(s_level[d] - s_p->metabolic[d] + inflow);
		s_level[d] = level;
		e = 1.0f - level;   /* 单侧性由值域构造保证，不钳位 */

		out->level[d] = level;
		out->error[d] = e;
		out->sev[d] = severity_of(e, s_p->band);
		out->trend[d] = e - s_prev_error[d];
		s_prev_error[d] = e;
	}
	out->error_sum = out->error[KD_SOCIAL] + out->error[KD_STIM] +
			 out->error[KD_REST];
	out->error_max = out->error[KD_SOCIAL] > out->error[KD_STIM] ?
			 (out->error[KD_SOCIAL] > out->error[KD_REST] ?
			  out->error[KD_SOCIAL] : out->error[KD_REST]) :
			 (out->error[KD_STIM] > out->error[KD_REST] ?
			  out->error[KD_STIM] : out->error[KD_REST]);
	(void)tick;
}
