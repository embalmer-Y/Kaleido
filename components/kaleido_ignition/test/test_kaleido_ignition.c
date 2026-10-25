/*
 * test_kaleido_ignition.c — 方案 A 打分/门控/滞回/结算回归（Unity）
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
		.holding = false,
	};

	return c;
}

/* 等权打分：五因子全 0.8 → S=0.8 */
static void scoring_equal_weights(void)
{
	struct ki_decision d;
	struct ki_factors f = { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
	struct ki_context c = base_ctx();

	ki_step(&c, ki_config_default(), &f, &d);
	TEST_ASSERT_TRUE(feq(d.score, 0.8f));
}

/* AND 硬门逐项拦截（勿扰/冷静期/配额/说话） */
static void gates_block(void)
{
	struct ki_decision d;
	struct ki_factors f = { 1, 1, 1, 1, 1 };
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();

	ki_step(&c, &cfg, &f, &d);
	TEST_ASSERT_TRUE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(0, d.gates.gate_mask);

	c.quiet_hours = true;
	ki_step(&c, &cfg, &f, &d);
	TEST_ASSERT_FALSE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(1u << 0, d.gates.gate_mask);

	c = base_ctx();
	c.last_fire_min = 590;      /* 10 分钟前刚说过 */
	ki_step(&c, &cfg, &f, &d);
	TEST_ASSERT_FALSE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(1u << 1, d.gates.gate_mask);

	c = base_ctx();
	c.fired_today = cfg.daily_quota;
	ki_step(&c, &cfg, &f, &d);
	TEST_ASSERT_EQUAL_UINT8(1u << 2, d.gates.gate_mask);

	c = base_ctx();
	c.user_speaking = true;
	ki_step(&c, &cfg, &f, &d);
	TEST_ASSERT_EQUAL_UINT8(1u << 3, d.gates.gate_mask);
}

/* 滞回：T_high 触发后跌到带内不撤防，跌破 T_low 才撤防 */
static void hysteresis_band(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();
	struct ki_factors f = { 0, 0, 0, 0, 0 };

	/* caller 维护 holding：candidate → true；holding && score<t_low → false */
	f.time_rhythm = 0.9f;
	ki_step(&c, &cfg, &f, &d);
	c.holding = d.candidate;
	TEST_ASSERT_TRUE(c.holding);          /* 0.9 ≥ 0.70      */

	f.time_rhythm = 0.58f;                /* 带内（0.5-0.7）  */
	ki_step(&c, &cfg, &f, &d);
	c.holding = d.candidate && true;
	TEST_ASSERT_TRUE(c.holding);          /* 仍持火          */

	f.time_rhythm = 0.4f;                 /* 跌破 T_low       */
	ki_step(&c, &cfg, &f, &d);
	c.holding = false;
	TEST_ASSERT_FALSE(d.candidate);       /* 0.4 < 0.7 撤防   */
}

/* 低主动度惩罚：proactivity=0 → 阈值 +0.10×1 → 0.8 不再触发 0.75 */
static void proactivity_penalty(void)
{
	struct ki_decision d;
	struct ki_config cfg = *ki_config_default();
	struct ki_context c = base_ctx();
	struct ki_factors f = { 0.75f, 0, 0, 0, 0 };

	cfg.proactivity = 0;
	ki_step(&c, &cfg, &f, &d);
	TEST_ASSERT_FALSE(d.candidate);       /* 0.75 < 0.80      */
	cfg.proactivity = 5;
	ki_step(&c, &cfg, &f, &d);
	TEST_ASSERT_TRUE(d.candidate);        /* 0.75 ≥ 0.70      */
}

/* 结算反馈：忽略 → 30→60→120→240 封顶；回应 → 复位 30 */
static void settle_feedback(void)
{
	ki_settle(true);
	TEST_ASSERT_EQUAL_UINT16(30, ki_cooldown_current());
	TEST_ASSERT_EQUAL_UINT16(60, ki_settle(false));
	TEST_ASSERT_EQUAL_UINT16(120, ki_settle(false));
	TEST_ASSERT_EQUAL_UINT16(240, ki_settle(false));
	TEST_ASSERT_EQUAL_UINT16(240, ki_settle(false)); /* 封顶 */
	TEST_ASSERT_EQUAL_UINT16(30, ki_settle(true));
}

/* 跨午夜冷静期回绕：昨晚 23:50 说过，今晨 00:05（15 分钟）未过 30 */
static void cooldown_midnight_wrap(void)
{
	struct ki_decision d;
	struct ki_factors f = { 1, 1, 1, 1, 1 };
	struct ki_context c = base_ctx();

	c.now_min = 5;                 /* 00:05            */
	c.last_fire_min = 1430;        /* 昨日 23:50       */
	ki_step(&c, ki_config_default(), &f, &d);
	TEST_ASSERT_FALSE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(1u << 1, d.gates.gate_mask);
	c.now_min = 35;                /* 00:35，45 分钟   */
	ki_step(&c, ki_config_default(), &f, &d);
	TEST_ASSERT_TRUE(d.candidate);
}

void ignition_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(scoring_equal_weights);
	RUN_TEST(gates_block);
	RUN_TEST(hysteresis_band);
	RUN_TEST(proactivity_penalty);
	RUN_TEST(settle_feedback);
	RUN_TEST(cooldown_midnight_wrap);
	UNITY_END();
}
