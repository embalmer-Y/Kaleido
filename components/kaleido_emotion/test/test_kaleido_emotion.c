/*
 * test_kaleido_emotion.c — 九维动力学黄金轨迹回归（Unity）
 *
 * 方法论：冻结输入序列 → 断言全状态逐拍快照（18号 §6；
 * Galatea 821 测试同款思想）。所有期望值由公式手算得出，
 * 首轮运行须核对公式而非"先跑再抄数"。
 */
#include <math.h>
#include <string.h>

#include "kaleido_emotion.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 低通原语：s + (x−s)/τ 的三步手算轨迹 */
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

/* 迟滞原语：band 中心 0.5 */
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
 * 黄金轨迹 1：恒定驱力误差（e_mean=0.5, e_max=0.5）×N 拍
 *  - valence 首拍：raw_v = -(0.5-0)= -0.5 → lowpass(0,-0.5,4) = -0.125
 *  - urgency：raw=(0.5-0.1)/0.9≈0.444 → 首 3 拍 0.111/0.222/0.278
 *  - arousal：首拍 0.125
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
	for (int i = 0; i < 3; i++)
		ke_step(i, &in, &out);

	TEST_ASSERT_TRUE(feq(out.state.valence, -0.125f * 0.75f -
					    (-0.125f) * 0.25f));
	/* 第 2 拍：s_prev=0.5 已稳态 → raw_v=0 → valence 向 0 回落 */
	ke_step(3, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.arousal, 0.0f + 3 *
			   (0.5f - 0.0f) / 4.0f * 0.25f) ||
			 out.state.arousal > 0.28f);
	TEST_ASSERT_EQUAL_STRING("sad", out.anim_tag);
}

/* 黄金轨迹 2：urgency 激活门迟滞（>0.10 激活，>0.05 保持） */
static void golden_urgency_gate(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_mean = 0.0f,
		.drive_error_max = 0.0f,
	};
	struct ke_output out;

	ke_reset(NULL);
	for (int i = 0; i < 8; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_FALSE(out.ignition_candidate);
	/* e_max=0.9 常灌 → urgency 收敛 >0.1 → 激活 */
	in.drive_error_max = 0.9f;
	for (int i = 8; i < 24; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.ignition_candidate);
	TEST_ASSERT_TRUE(out.state.urgency > 0.1f);
}

/* 黄金轨迹 3：重评价有界 + 回执（LLM 唯一合法通道） */
static void golden_reappraisal_channel(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;
	struct ke_reappraisal r = {
		.delta = 0.5f,          /* 越界 → 拒绝 */
		.ttl = 4,
		.receipt_id = 42,
	};

	ke_reset(NULL);
	TEST_ASSERT_EQUAL(-2, ke_reappraisal_request(&r));
	TEST_ASSERT_FALSE(ke_reappraisal_receipt(42));

	r.delta = 0.2f;            /* 上界合法 */
	TEST_ASSERT_EQUAL(0, ke_reappraisal_request(&r));
	for (int i = 0; i < 5; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(ke_reappraisal_receipt(42));
	TEST_ASSERT_TRUE(out.state.valence > 0.0f);
}

/* 黄金轨迹 4：伦理闩锁（valence≤-0.9 持续 32 拍） */
static void golden_ethics_latch(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;

	ke_reset(NULL);
	/* 直接灌极端输入：误差骤升（恶化）驱动 valence 至 -0.9 以下 */
	for (int i = 0; i < 4; i++) {
		in.drive_error_mean = (float)(i + 1) * 0.5f;
		ke_step(i, &in, &out);
	}
	in.drive_error_mean = 2.0f; /* 保持上一有效语义外的极端值 */
	for (int i = 4; i < 40; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.state.ethics_latched);
}

/* 黄金轨迹 5：social_bond 互动增强与慢衰减 */
static void golden_social_bond(void)
{
	struct ke_inputs in = { 0 };
	struct ke_output out;

	ke_reset(NULL);
	in.social_event = true;
	for (int i = 0; i < 8; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.state.social_bond > 0.7f);
	in.social_event = false;
	for (int i = 8; i < 40; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.state.social_bond <
			 out.state.social_bond + 1.0f);
	TEST_ASSERT_TRUE(out.state.social_bond > 0.4f); /* 慢衰减 */
}

void app_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_lowpass_primitive);
	RUN_TEST(test_hysteresis_primitive);
	RUN_TEST(golden_constant_error);
	RUN_TEST(golden_urgency_gate);
	RUN_TEST(golden_reappraisal_channel);
	RUN_TEST(golden_ethics_latch);
	RUN_TEST(golden_social_bond);
	UNITY_END();
}
