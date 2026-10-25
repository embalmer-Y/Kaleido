/*
 * test_kaleido_emotion.c — 九维动力学黄金轨迹回归（Unity）
 *
 * 期望值全部公式手算（评审 A2 返工：逐拍推导 + 缺口用例补齐）。
 * 主机回归入口见 tools/host-tests/run_all.sh；板端 soul-test 复核。
 */
#include <math.h>
#include <string.h>

#include "kaleido_emotion.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 低通原语：s + (x−s)/τ 三步手算 */
static void test_lowpass_primitive(void)
{
	float s = 0.0f;

	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.25f));
	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.4375f));
	s = ke_lowpass(s, 0.0f, 16.0f);
	TEST_ASSERT_TRUE(feq(s, 0.4375f * 15.0f / 16.0f));
}

static void test_hysteresis_primitive(void)
{
	bool on = false;

	TEST_ASSERT_FALSE(ke_hysteresis(on, 0.55f, 0.1f));
	on = ke_hysteresis(on, 0.65f, 0.1f);
	TEST_ASSERT_TRUE(on);
	TEST_ASSERT_TRUE(ke_hysteresis(on, 0.55f, 0.1f));
	TEST_ASSERT_FALSE(ke_hysteresis(on, 0.30f, 0.1f));
}

/*
 * 黄金 1：恒定驱力误差 e=0.5（A1 修复后的正确轨迹）
 *  拍0：raw_v = -(0.5-0) = -0.5 → val = -0.5/4 = -0.125
 *  拍1起：raw_v = 0 → val 向 0 衰减 ×0.75/拍
 */
static void golden_constant_error(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_mean = 0.5f,
		.drive_error_max = 0.5f,
	};
	struct ke_output out;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.125f));
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.09375f));
	ke_step(2, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.0703125f));
	/* arousal 三拍：0.125/0.21875/0.2890625（<0.3 → sleepy 带） */
	TEST_ASSERT_TRUE(feq(out.state.arousal, 0.2890625f));
	TEST_ASSERT_EQUAL_STRING("sleepy", out.anim_tag);
}

/* 误差跳变（0→1）：valence 单拍 dip = -0.25（可达域上界证据） */
static void golden_error_step_jump(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	in.drive_error_mean = 1.0f;
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.25f));
}

/* urgency 迟滞：0.10 激活 / 0.05 释放（边界） */
static void golden_urgency_gate(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;
	int i;

	ke_reset(NULL);
	for (i = 0; i < 8; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_FALSE(out.ignition_candidate);
	/* e_max=0.9 灌 16 拍 → urgency 收敛 0.888… > 0.10 → 激活 */
	in.drive_error_max = 0.9f;
	for (i = 8; i < 24; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.ignition_candidate);
	/* 释放边界：撤压后 urgency 指数衰减，>0.05 期间保持激活 */
	in.drive_error_max = 0.0f;
	ke_step(24, &in, &out);
	ke_step(25, &in, &out);
	TEST_ASSERT_TRUE(out.state.urgency > 0.05f);
	TEST_ASSERT_TRUE(out.ignition_candidate);
}

/* 重评价：越界拒、上界生效、回执、two_step 渐进（非瞬时） */
static void golden_reappraisal_channel(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;
	struct ke_reappraisal r = {
		.delta = 0.5f, .ttl = 4, .receipt_id = 42,
	};
	float v0, v1, v2;

	ke_reset(NULL);
	TEST_ASSERT_EQUAL(-2, ke_reappraisal_request(&r));
	r.delta = 0.2f;
	TEST_ASSERT_EQUAL(0, ke_reappraisal_request(&r));
	ke_step(0, &in, &out);
	v0 = out.state.valence;
	ke_step(1, &in, &out);
	v1 = out.state.valence;
	TEST_ASSERT_TRUE(v1 > v0);          /* 两级低通逐拍递增 */
	ke_step(2, &in, &out);
	v2 = out.state.valence;
	TEST_ASSERT_TRUE(v2 > v1);
	TEST_ASSERT_FALSE(ke_reappraisal_receipt(42)); /* TTL 未尽 */
	ke_step(3, &in, &out);
	ke_step(4, &in, &out);
	TEST_ASSERT_TRUE(ke_reappraisal_receipt(42));
	TEST_ASSERT_TRUE(out.state.valence > 0.0f);
}

/* 伦理闩锁：seed -0.95 + 阶跃，≤-0.2 维持约 6 拍；N=4 → 第 4 拍闩锁粘滞 */
static void golden_ethics_latch(void)
{
	struct ke_state seed = { .valence = -0.95f };
	struct ke_inputs in = { .have_drive = true,
				.drive_error_mean = 1.0f };
	struct ke_output out;
	int i;

	ke_reset(&seed);
	for (i = 0; i < 3; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_FALSE(out.state.ethics_latched); /* 3 拍未达 */
	ke_step(3, &in, &out);
	TEST_ASSERT_TRUE(out.state.ethics_latched);  /* 4 拍闩锁   */
	in.drive_error_mean = 0.0f;                  /* 好转不清锁 */
	for (i = 4; i < 12; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.state.ethics_latched);
}

/* ke_reset 全清（S6）：残值不得泄漏到下一局 */
static void golden_reset_clears(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_mean = 1.0f,
		.drive_error_max = 1.0f,
		.social_event = true,
	};
	struct ke_output out;
	int i;

	ke_reset(NULL);
	for (i = 0; i < 10; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.state.social_bond > 0.9f);
	ke_reset(NULL);
	ke_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.social_bond, 0.125f));
	TEST_ASSERT_TRUE(feq(out.state.urgency, 0.25f)); /* e_max=1 */
}

/* !have_drive：s_prev 不更新，valence 按 raw_v=0 衰减（保持语义） */
static void golden_no_drive_hold(void)
{
	struct ke_inputs in = { .have_drive = true,
				.drive_error_mean = 1.0f };
	struct ke_output out;
	float v;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	v = out.state.valence;
	in.have_drive = false;
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(out.state.valence > v); /* 向 0 衰减非跳变 */
}

/* social_bond：互动增强 + 慢衰减（真断言版） */
static void golden_social_bond(void)
{
	struct ke_inputs in = { 0 };
	struct ke_output out;
	int i;

	ke_reset(NULL);
	in.social_event = true;
	for (i = 0; i < 8; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.state.social_bond > 0.99f);
	in.social_event = false;
	for (i = 8; i < 40; i++)
		ke_step(i, &in, &out);
	/* τ=1024：32 拍衰减 ≈ 1-(32/1024) → 仍 >0.95（慢谱语义） */
	TEST_ASSERT_TRUE(out.state.social_bond > 0.95f);
}

void emotion_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_lowpass_primitive);
	RUN_TEST(test_hysteresis_primitive);
	RUN_TEST(golden_constant_error);
	RUN_TEST(golden_error_step_jump);
	RUN_TEST(golden_urgency_gate);
	RUN_TEST(golden_reappraisal_channel);
	RUN_TEST(golden_ethics_latch);
	RUN_TEST(golden_reset_clears);
	RUN_TEST(golden_no_drive_hold);
	RUN_TEST(golden_social_bond);
	UNITY_END();
}
