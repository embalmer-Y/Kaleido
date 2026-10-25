/*
 * test_kaleido_ignition.c — 方案 A 打分/门控/滞回/结算回归（Unity）
 *
 * 评审 A2 返工：单因子需过权重（×0.2）后与阈值比较——五因子同值
 * 使 S=v，期望值可独立推导。
 */
#include <math.h>
#include <string.h>

#include "kaleido_ignition.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

static struct ki_context base_ctx(void)
{
	struct ki_context c = {
		.now_min = 600,
		.last_fire_min = 0,
		.fired_today = 0,
	};

	return c;
}

static void arm(struct ki_decision *d, struct ki_config *cfg,
		struct ki_context *c, float v)
{
	/* 复位武装：低分拍（<t_low）先行 */
	ki_step(c, cfg, &(struct ki_factors){ 0, 0, 0, 0, 0 }, d);
	ki_step(c, cfg,
		&(struct ki_factors){ v, v, v, v, v }, d);
}

/* 五因子全 0.8 → S=0.8 ≥ 0.70 → 触发；同分不重复 */
static void scoring_and_fire(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();

	arm(&d, &cfg, &c, 0.8f);
	TEST_ASSERT_TRUE(feq(d.score, 0.8f));
	TEST_ASSERT_TRUE(d.candidate);
	ki_step(&c, &cfg,
		&(struct ki_factors){ 0.8f, 0.8f, 0.8f, 0.8f, 0.8f }, &d);
	TEST_ASSERT_FALSE(d.candidate);
}

/* AND 硬门逐项拦截 */
static void gates_block(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();

	arm(&d, &cfg, &c, 1.0f);
	TEST_ASSERT_TRUE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(0, d.gates.gate_mask);

	c.quiet_hours = true;
	arm(&d, &cfg, &c, 1.0f);
	TEST_ASSERT_FALSE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(1u << 0, d.gates.gate_mask);

	c = base_ctx();
	c.last_fire_min = 590;
	arm(&d, &cfg, &c, 1.0f);
	TEST_ASSERT_EQUAL_UINT8(1u << 1, d.gates.gate_mask);

	c = base_ctx();
	c.fired_today = cfg.daily_quota;
	arm(&d, &cfg, &c, 1.0f);
	TEST_ASSERT_EQUAL_UINT8(1u << 2, d.gates.gate_mask);

	c = base_ctx();
	c.user_speaking = true;
	arm(&d, &cfg, &c, 1.0f);
	TEST_ASSERT_EQUAL_UINT8(1u << 3, d.gates.gate_mask);
}

/* 滞回：fire→撤防；带内不武装；跌破 t_low 重新武装后再触发 */
static void hysteresis_band(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();

	arm(&d, &cfg, &c, 0.9f);
	TEST_ASSERT_TRUE(d.candidate);
	ki_step(&c, &cfg,
		&(struct ki_factors){ 0.9f, 0.9f, 0.9f, 0.9f, 0.9f }, &d);
	TEST_ASSERT_FALSE(d.candidate);       /* 同分不重复          */
	ki_step(&c, &cfg,
		&(struct ki_factors){ 0.58f, 0.58f, 0.58f,
				     0.58f, 0.58f }, &d);
	TEST_ASSERT_FALSE(d.candidate);       /* 带内不武装          */
	ki_step(&c, &cfg,
		&(struct ki_factors){ 0.75f, 0.75f, 0.75f,
				     0.75f, 0.75f }, &d);
	TEST_ASSERT_FALSE(d.candidate);       /* 高于阈但未武装      */
	ki_step(&c, &cfg,
		&(struct ki_factors){ 0.4f, 0.4f, 0.4f, 0.4f, 0.4f }, &d);
	ki_step(&c, &cfg,
		&(struct ki_factors){ 0.75f, 0.75f, 0.75f,
				     0.75f, 0.75f }, &d);
	TEST_ASSERT_TRUE(d.candidate);        /* 重新武装后可触发    */
}

/* 低主动度惩罚：阈值 +0.10 → 0.75 不触发；正常档触发 */
static void proactivity_penalty(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();

	cfg.proactivity = 0;
	arm(&d, &cfg, &c, 0.75f);
	TEST_ASSERT_FALSE(d.candidate);       /* 0.75 < 0.80         */
	cfg.proactivity = 5;
	arm(&d, &cfg, &c, 0.75f);
	TEST_ASSERT_TRUE(d.candidate);        /* 0.75 ≥ 0.70         */
}

/* proactivity 缩放配额（B1 关联）：quota=4, pro=5 → 生效 2 */
static void proactivity_scales_quota(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();

	cfg.proactivity = 5;
	c.fired_today = 2;
	arm(&d, &cfg, &c, 1.0f);
	TEST_ASSERT_EQUAL_UINT8(1u << 2, d.gates.gate_mask);
	c.fired_today = 1;
	arm(&d, &cfg, &c, 1.0f);
	TEST_ASSERT_EQUAL_UINT8(0, d.gates.gate_mask & (1u << 2));
}

/* NULL 参数防御（S3）：不得崩溃 */
static void null_guards(void)
{
	struct ki_decision d;

	ki_step(NULL, ki_config_default(), NULL, &d);
	ki_step(&(struct ki_context){ 0 }, NULL, NULL, &d);
	TEST_ASSERT_FALSE(d.candidate);
}

/* 结算反馈：忽略翻倍封顶、回应复位（基于生效配置 B1） */
static void settle_feedback(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();

	ki_settle(true);
	TEST_ASSERT_EQUAL_UINT16(30, ki_cooldown_current());
	TEST_ASSERT_EQUAL_UINT16(60, ki_settle(false));
	TEST_ASSERT_EQUAL_UINT16(120, ki_settle(false));
	TEST_ASSERT_EQUAL_UINT16(240, ki_settle(false));
	TEST_ASSERT_EQUAL_UINT16(240, ki_settle(false));
	TEST_ASSERT_EQUAL_UINT16(30, ki_settle(true));
	cfg.cooldown_min = 60;                 /* persona 覆写        */
	ki_step(&c, &cfg, &(struct ki_factors){ 0, 0, 0, 0, 0 }, &d);
	ki_settle(true);
	TEST_ASSERT_EQUAL_UINT16(60, ki_cooldown_current());
}

/* 跨午夜冷静期回绕 */
static void cooldown_midnight_wrap(void)
{
	struct ki_decision d;
	struct ki_context c = base_ctx();

	/* 归一化：上一用例可能改写 s_cfg_last/s_cooldown（顺序独立） */
	ki_step(&c, ki_config_default(),
		&(struct ki_factors){ 0, 0, 0, 0, 0 }, &d);
	ki_settle(true);
	c.now_min = 5;
	c.last_fire_min = 1430;
	arm(&d, (struct ki_config *)ki_config_default(), &c, 1.0f);
	TEST_ASSERT_EQUAL_UINT8(1u << 1, d.gates.gate_mask);
	c.now_min = 35;
	arm(&d, (struct ki_config *)ki_config_default(), &c, 1.0f);
	TEST_ASSERT_TRUE(d.candidate);
}

void ignition_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(scoring_and_fire);
	RUN_TEST(gates_block);
	RUN_TEST(hysteresis_band);
	RUN_TEST(proactivity_penalty);
	RUN_TEST(proactivity_scales_quota);
	RUN_TEST(null_guards);
	RUN_TEST(settle_feedback);
	RUN_TEST(cooldown_midnight_wrap);
	UNITY_END();
}
