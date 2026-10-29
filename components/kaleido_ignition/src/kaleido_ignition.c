/*
 * kaleido_ignition.c — 方案 A 实现（纯函数式，零分配零 RNG）
 *
 * 返工记录（评审 S3/S4/B1，2026-10-25）：
 *  - S3：判空先于任何 cfg 解引用
 *  - S4：武装态收归组件内部；惩罚只抬 t_high，滞回判定只做一次
 *  - B1：结算基于"最近一次生效配置"（persona 覆写可被结算尊重）
 */
#include "kaleido_ignition.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ---- W3：思维触发闸（纯函数；全部状态由 ctx 供给，评审 C3 纪律） ---- */

#ifndef KI_MEM_HOOK_T
#define KI_MEM_HOOK_T	0.5f	/* km 检索 top1 触发阈（分数≤1.0：
				 * recency 0.5 + strength 0.3 + 命中 0.2） */
#endif

void ki_think_gate(const struct ki_think_ctx *ctx,
		   struct ki_think_gate *out)
{
	bool idle, low_ar, salient;

	if (!ctx || !out)
		return;
	memset(out, 0, sizeof(*out));

	idle = ctx->urgency_max <= 0.0f && !ctx->pending_command;
	low_ar = ctx->arousal < 0.1f;       /* 2σ 带锚（p4_beta 冻结） */
	salient = ctx->score >= KI_SALIENT_T;

	out->reason_mask =
		(idle ? 1u << 0 : 0) | (low_ar ? 1u << 1 : 0) |
		(ctx->bin_changed ? 1u << 2 : 0) |
		(salient ? 1u << 3 : 0) |
		(ctx->new_event ? 1u << 4 : 0) |
		(ctx->mem_hook >= KI_MEM_HOOK_T ? 1u << 5 : 0);
	out->trigger = idle || low_ar || ctx->bin_changed || salient ||
		       ctx->new_event ||
		       ctx->mem_hook >= KI_MEM_HOOK_T;
	if (!out->trigger)
		return;

	if (ctx->quiet_hours)
		out->blocked_mask |= 1u << 0;
	if (ctx->now_min - ctx->last_think_min < KI_THINK_GAP_MIN)
		out->blocked_mask |= 1u << 1;
	if (ctx->thought_today >= KI_THINK_DAILY_MAX)
		out->blocked_mask |= 1u << 2;
	/* 新意门：上轮沉默后，无跃迁/新事件、分数无起色 → 不再想 */
	if (ctx->last_was_silent && !ctx->bin_changed &&
	    !ctx->new_event && ctx->score <= ctx->last_silent_score)
		out->blocked_mask |= 1u << 3;

	if (out->blocked_mask)
		out->trigger = false;
}

/* ---- 意图解析（拒非降级；零依赖手解固定 schema） ---- */

/* 取 "key":"value" 的 value 起止（处理 \" \\ \n \uXXXX 最小转义；
 * 返回 value 长度，end 写结束指针（指向闭引号），找不到返回 <0） */
static int json_string(const char *json, const char *key,
		       const char **val, const char **end)
{
	char pat[24];
	const char *p;
	size_t n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (!p)
		return -1;
	p += strlen(pat);
	while (*p == ' ' || *p == ':')
		p++;
	if (*p != '"')
		return -1;
	p++;
	*val = p;
	while (p[n] && p[n] != '"') {
		if (p[n] == '\\')
			n++;            /* 跳过转义字符本体 */
		n++;
	}
	if (p[n] != '"')
		return -1;
	*end = p + n;
	return (int)n;
}

/* 数值字段（非引号）：取 ": <float>" 到 ',' / '}' 止 */
static int json_float(const char *json, const char *key, float *out)
{
	char pat[24];
	const char *p;
	char buf[24];
	size_t n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (!p)
		return -1;
	p += strlen(pat);
	while (*p == ' ' || *p == ':')
		p++;
	while (p[n] && p[n] != ',' && p[n] != '}' && n < sizeof(buf) - 1)
		n++;
	if (!n)
		return -1;
	memcpy(buf, p, n);
	buf[n] = 0;
	*out = strtof(buf, NULL);
	return 0;
}

/* evidence 前缀词表 → mask（无前缀命中的串不计入；≥1 条才算有据） */
static const char *const ki_evid_names[5] = {
	"drive", "affect", "mem", "dialog", "bin",
};

static uint8_t evidence_mask_of(const char *json)
{
	const char *arr = strstr(json, "evidence");
	uint8_t mask = 0;
	size_t i;

	if (!arr)
		return 0;
	for (i = 0; i < 5; i++) {
		char pat[16];

		snprintf(pat, sizeof(pat), "\"%s:", ki_evid_names[i]);
		if (strstr(arr, pat))
			mask |= 1u << i;
	}
	return mask;
}

enum ki_parse_err ki_intent_parse(const char *json, size_t len,
				  struct ki_intent *out)
{
	const char *cv, *ce;
	int cn;
	char act[16];
	float conf = -1.0f;

	if (!json || !out || len == 0)
		return KI_PARSE_JSON_BAD;
	memset(out, 0, sizeof(*out));
	out->ttl_ticks = 30;
	if (!strstr(json, "{") || !strstr(json, "\"act\""))
		return KI_PARSE_JSON_BAD;

	/* act */
	cn = json_string(json, "act", &cv, &ce);
	if (cn < 0 || (size_t)cn >= sizeof(act))
		return KI_PARSE_JSON_BAD;
	memcpy(act, cv, (size_t)cn);
	act[cn] = 0;
	if (!strcmp(act, "speak"))
		out->act = KI_SPEAK;
	else if (!strcmp(act, "stay_silent"))
		out->act = KI_STAY_SILENT;
	else if (!strcmp(act, "act"))
		out->act = KI_ACT;
	else
		return KI_PARSE_ACT_ILLEGAL;

	/* evidence 前缀先扫（CONTENT_LONG 提前返回也要带出去） */
	out->evidence_mask = evidence_mask_of(json);

	/* confidence（缺省视为 0 → 低于下限走 SILENT 处置） */
	if (json_float(json, "confidence", &conf) < 0)
		conf = 0.0f;
	out->confidence = conf < 0.0f ? 0.0f :
			  (conf > 1.0f ? 1.0f : conf);

	/* content（speak 必填；>80B 截断留痕，UTF-8 不劈半字） */
	cn = json_string(json, "content", &cv, &ce);
	if (out->act == KI_SPEAK) {
		if (cn <= 0)
			return KI_PARSE_CONTENT_EMPTY;
		if (cn > KI_CONTENT_MAX) {
			int cut = KI_CONTENT_MAX;

			while (cut > 0 && (((unsigned char)cv[cut]) &
					   0xC0) == 0x80)
				cut--;
			memcpy(out->content, cv, (size_t)cut);
			out->content[cut] = 0;
			return KI_PARSE_CONTENT_LONG;
		}
		memcpy(out->content, cv, (size_t)cn);
		out->content[cn] = 0;
		/* 数字主张无 mem 引用 → 拒（UncitedClaim 移植） */
		if (strpbrk(out->content, "0123456789") &&
		    !(out->evidence_mask & (1u << 2)))
			return KI_PARSE_UNCITED_NUMBER;
	}

	/* evidence ≥1（C-5：无内生证据不可成**承诺**——沉默是一等输出
	 * 不需要证据，否则"想了但决定不说"永远被拒） */
	if (out->act != KI_STAY_SILENT && out->evidence_mask == 0)
		return KI_PARSE_NO_EVIDENCE;

	/* reason（可选叙事锚） */
	cn = json_string(json, "reason", &cv, &ce);
	if (cn > 0) {
		size_t cut = (size_t)cn < sizeof(out->reason) - 1 ?
			     (size_t)cn : sizeof(out->reason) - 1;

		memcpy(out->reason, cv, cut);
		out->reason[cut] = 0;
	}
	return KI_PARSE_OK;
}

/* ---- 出口硬门（纯函数；原 ki_step 四门+ethics/tts） ---- */

void ki_intent_fire(const struct ki_intent *in,
		    const struct ki_fire_ctx *ctx,
		    const struct ki_config *cfg,
		    struct ki_decision *out)
{
	bool g_quiet, g_cool, g_quota, g_speak, g_ethics, g_tts;
	uint32_t delta;
	uint8_t q = quota_effective(cfg);

	if (!in || !ctx || !cfg || !out)
		return;
	memset(out, 0, sizeof(*out));
	delta = ctx->now_min >= ctx->last_fire_min ?
		ctx->now_min - ctx->last_fire_min :
		1440 - ctx->last_fire_min + ctx->now_min;
	g_quiet = !ctx->quiet_hours;
	g_cool = delta >= s_cooldown;
	g_quota = ctx->fired_today < q;
	g_speak = !ctx->user_speaking;
	g_ethics = !ctx->ethics_latched;
	g_tts = ctx->tts_ready;
	out->gates.gate_mask =
		(g_quiet ? 0 : 1u << 0) | (g_cool ? 0 : 1u << 1) |
		(g_quota ? 0 : 1u << 2) | (g_speak ? 0 : 1u << 3) |
		(g_ethics ? 0 : 1u << 4) | (g_tts ? 0 : 1u << 5);
	out->candidate = g_quiet && g_cool && g_quota && g_speak &&
			 g_ethics && g_tts;
	out->score = in->confidence;
}

/* ---- 结算 v2（六态；仅 IGNORED 翻倍） ---- */

uint16_t ki_settle2(enum ki_settle st)
{
	uint16_t base = s_cfg_seen ? s_cfg_last.cooldown_min :
				     s_def.cooldown_min;
	uint16_t cap = s_cfg_seen ? s_cfg_last.cooldown_max :
				    s_def.cooldown_max;

	switch (st) {
	case KI_SETTLE_RESPONDED:
		s_cooldown = base;
		break;
	case KI_SETTLE_IGNORED:
		s_cooldown = s_cooldown * 2;
		if (s_cooldown > cap)
			s_cooldown = cap;
		break;
	default:        /* INTERRUPTED/GATE_BLOCKED/SILENT/TTL_EXPIRED：
			 * 合法不罚（禁把未解决归 FAILED） */
		break;
	}
	return s_cooldown;
}
