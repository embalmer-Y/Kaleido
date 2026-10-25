/*
 * kaleido_ignition.c — 方案 A 实现（纯函数式，零分配零 RNG）
 *
 * 返工记录（评审 S3/S4/B1，2026-10-25）：
 *  - S3：判空先于任何 cfg 解引用
 *  - S4：武装态收归组件内部；惩罚只抬 t_high，滞回判定只做一次
 *  - B1：结算基于"最近一次生效配置"（persona 覆写可被结算尊重）
 */
#include "kaleido_ignition.h"

#include <string.h>

#define KI_PROAC_MIN	2u
#define KI_PROAC_PENALTY 0.10f

static const struct ki_config s_def = {
	.w = { 0.20f, 0.20f, 0.20f, 0.20f, 0.20f },
	.t_high = 0.70f,
	.t_low = 0.50f,
	.cooldown_min = 30,
	.cooldown_max = 240,
	.daily_quota = 4,
	.proactivity = 5,
};

static struct ki_config s_cfg_last; /* 最近一次 ki_step 生效配置  */
static bool s_cfg_seen;
static bool s_armed = true;         /* 滞回武装态（fire 后撤防）  */
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
	uint32_t delta = ctx->now_min >= ctx->last_fire_min ?
			 ctx->now_min - ctx->last_fire_min :
			 1440 - ctx->last_fire_min + ctx->now_min;

	return delta >= cooldown;
}

/* 主动度统一缩放（22号 §5.3）：低档抬阈值、降配额；额度下限 1 */
static float t_high_effective(const struct ki_config *cfg)
{
	float t = cfg->t_high;

	if (cfg->proactivity < KI_PROAC_MIN) {
		uint8_t steps = (KI_PROAC_MIN - cfg->proactivity + 1) / 2;

		t += KI_PROAC_PENALTY * steps;
	}
	return t;
}

static uint8_t quota_effective(const struct ki_config *cfg)
{
	uint8_t q = cfg->daily_quota;

	if (cfg->proactivity < 10)
		q = (uint8_t)((q * cfg->proactivity + 9) / 10);
	return q ? q : 1;
}

void ki_step(const struct ki_context *ctx, const struct ki_config *cfg,
	     const struct ki_factors *f, struct ki_decision *out)
{
	const float *v;
	float s = 0.0f;
	float t_high;
	bool over_high;

	if (!ctx || !cfg || !f || !out)
		return;                       /* S3：先判空           */
	memset(out, 0, sizeof(*out));
	v = (const float *)f;
	for (int i = 0; i < KI_FACTOR_CNT; i++)
		s += cfg->w[i] * clampf(v[i], 0.0f, 1.0f);
	out->score = s;
	out->f = *f;
	s_cfg_last = *cfg;
	s_cfg_seen = true;

	t_high = t_high_effective(cfg);

	/* 滞回（一次性判定，惩罚只抬阈：S4）：
	 *  armed && s ≥ t_high → candidate（fire 后调用方应视为撤防）
	 *  !armed && s < t_low → 重新武装                                */
	over_high = s_armed && s >= t_high;
	if (!s_armed && s < cfg->t_low)
		s_armed = true;

	{
		bool g_quiet = !ctx->quiet_hours;
		bool g_cool = cooldown_passed(ctx, s_cooldown);
		bool g_quota = ctx->fired_today < quota_effective(cfg);
		bool g_speak = !ctx->user_speaking;

		out->gates.gate_mask =
			(g_quiet ? 0 : 1u << 0) | (g_cool ? 0 : 1u << 1) |
			(g_quota ? 0 : 1u << 2) | (g_speak ? 0 : 1u << 3);
		out->candidate = over_high && g_quiet && g_cool &&
				 g_quota && g_speak;
		if (out->candidate)
			s_armed = false;          /* fire 即撤防       */
	}
}

uint16_t ki_settle(bool responded)
{
	uint16_t base = s_cfg_seen ? s_cfg_last.cooldown_min :
				     s_def.cooldown_min;
	uint16_t cap = s_cfg_seen ? s_cfg_last.cooldown_max :
				    s_def.cooldown_max;

	if (responded) {
		s_cooldown = base;
	} else {
		s_cooldown *= 2;
		if (s_cooldown > cap)
			s_cooldown = cap;
	}
	return s_cooldown;
}

uint16_t ki_cooldown_current(void)
{
	return s_cooldown;
}
