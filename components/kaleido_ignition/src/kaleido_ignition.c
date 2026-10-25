/*
 * kaleido_ignition.c — 方案 A 实现（纯函数式，零分配零 RNG）
 */
#include "kaleido_ignition.h"

#include <string.h>

#define KI_PROAC_MIN	2u   /* 低于此主动度上浮阈值     */
#define KI_PROAC_PENALTY 0.10f /* 每降 2 档 +0.10 阈值  */

static const struct ki_config s_def = {
	.w = { 0.20f, 0.20f, 0.20f, 0.20f, 0.20f },
	.t_high = 0.70f,
	.t_low = 0.50f,
	.cooldown_min = 30,
	.cooldown_max = 240,
	.daily_quota = 4,
	.proactivity = 5,
};

static uint16_t s_cooldown = 30;

const struct ki_config *ki_config_default(void)
{
	return &s_def;
}

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static bool cooldown_passed(const struct ki_context *ctx,
			    uint16_t cooldown)
{
	/* 当日分钟回绕（跨午夜）以 1440 补齐 */
	uint32_t delta = ctx->now_min >= ctx->last_fire_min ?
			 ctx->now_min - ctx->last_fire_min :
			 1440 - ctx->last_fire_min + ctx->now_min;

	return delta >= cooldown;
}

void ki_step(const struct ki_context *ctx, const struct ki_config *cfg,
	     const struct ki_factors *f, struct ki_decision *out)
{
	const float *v = (const float *)f;
	float s = 0.0f;
	float t_high = cfg->t_high;

	if (!ctx || !cfg || !f || !out)
		return;
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < KI_FACTOR_CNT; i++)
		s += cfg->w[i] * clampf(v[i], 0.0f, 1.0f);
	out->score = s;
	out->f = *f;

	/* 滞回：holding 复位在 T_low 之下才重新武装 */
	bool over_high = s >= t_high;

	if (ctx->holding && s < cfg->t_low)
		over_high = false; /* 本拍撤防；下拍重新比较     */

	/* 主动度惩罚：低主动度 → 阈值上浮（等价降频） */
	if (cfg->proactivity < KI_PROAC_MIN) {
		uint8_t steps = (KI_PROAC_MIN - cfg->proactivity + 1) / 2;

		t_high += KI_PROAC_PENALTY * steps;
		over_high = s >= t_high;
	}

	/* AND 硬门（22号 §5.1-4） */
	bool g_quiet = !ctx->quiet_hours;
	bool g_cool = cooldown_passed(ctx, s_cooldown);
	bool g_quota = ctx->fired_today < cfg->daily_quota;
	bool g_speak = !ctx->user_speaking;

	out->gates.gate_mask =
		(g_quiet ? 0 : 1u << 0) | (g_cool ? 0 : 1u << 1) |
		(g_quota ? 0 : 1u << 2) | (g_speak ? 0 : 1u << 3);
	out->candidate = over_high && g_quiet && g_cool && g_quota &&
			 g_speak;
}

uint16_t ki_settle(bool responded)
{
	if (responded) {
		s_cooldown = s_def.cooldown_min;
	} else {
		s_cooldown *= 2;
		if (s_cooldown > s_def.cooldown_max)
			s_cooldown = s_def.cooldown_max;
	}
	return s_cooldown;
}

uint16_t ki_cooldown_current(void)
{
	return s_cooldown;
}
