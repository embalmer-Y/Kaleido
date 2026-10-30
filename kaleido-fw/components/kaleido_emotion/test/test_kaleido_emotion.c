/*
 * test_kaleido_emotion.c — 九维动力学黄金轨迹回归（Unity）
 *
 * 期望值全部公式手算。W0-2 重构（28 号 D1-D7 修正同步）：
 *  - drive_error_sum 语义（D1）；半带宽迟滞（D6）；一次性重评价
 *    （D4）；缺席保持（D5/D7）；伦理闩锁 −0.35/3 + ke_ethics_clear
 *  - 新增 Galatea 手算锚：s3/s4 阶跃、(15/16)^16 衰减、恒定误差
 *    40 拍 fixed-point、kd→ke 全链
 * 主机回归入口见 tools/host-tests/run_all.sh；板端 soul-test 复核。
 */
#include <math.h>
#include <string.h>

#include "kaleido_drive.h"
#include "kaleido_emotion.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 低通原语：Galatea 阶跃/衰减黄金锚（test_dynamics_identifiability） */
static void test_lowpass_primitive(void)
{
	float s = 0.0f;
	int i;

	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.25f));
	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.4375f));
	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.578125f));     /* 源锚 s3 */
	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.68359375f));   /* 源锚 s4（t63=4 拍） */
	s = 1.0f;
	for (i = 0; i < 16; i++)
		s = ke_lowpass(s, 0.0f, 16.0f);
	TEST_ASSERT_TRUE(fabsf(s - 0.35607413f) < 1e-4f); /* (15/16)^16 */
}

/* 半带宽迟滞（D6）：band=0.1 ⇒ 0.55/0.45，边界含 */
static void test_hysteresis_primitive(void)
{
	bool on = false;

	TEST_ASSERT_TRUE(ke_hysteresis(on, 0.55f, 0.1f));  /* 恰 0.55 → ON */
	on = true;
	TEST_ASSERT_TRUE(ke_hysteresis(on, 0.50f, 0.1f));  /* 带内保持 */
	TEST_ASSERT_FALSE(ke_hysteresis(on, 0.45f, 0.1f)); /* 恰 0.45 → OFF */
	on = false;
	TEST_ASSERT_FALSE(ke_hysteresis(on, 0.54f, 0.1f)); /* 差 0.01 不动 */
}

/*
 * 黄金 1：恒定驱力误差 e=0.5
 *  拍0：raw_v = -(0.5-0) = -0.5 → val = -0.5/4 = -0.125
 *  拍1起：raw_v = 0 → val 向 0 衰减 ×0.75/拍
 */
static void golden_constant_error(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_sum = 0.5f,
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

/* 源锚（test_valence_direction）：恒定误差 40 拍 → valence≈0、
 * arousal≈0.5——"持续饥渴不难过"（valence 只表达变化率） */
static void golden_constant_error_long(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_sum = 0.5f,
		.drive_error_max = 0.5f,
	};
	struct ke_output out;
	int i;

	ke_reset(NULL);
	for (i = 0; i <= 40; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(fabsf(out.state.valence) < 0.01f);
	TEST_ASSERT_TRUE(feq(out.state.arousal, 0.5f));
}

/* 误差跳变（0→1）：valence 单拍 dip = -0.25（sum 语义单驱力全崩） */
static void golden_error_step_jump(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	in.drive_error_sum = 1.0f;
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.25f));
}

/* urgency 迟滞：0.10 激活 / 0.05 释放（边界；Kaleido 附加迟滞） */
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

/* 重评价（D4 一次性）：越界拒、即时精确施加、applied 审计、
 * 未生效前新请求覆盖旧请求 */
static void golden_reappraisal_channel(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;
	struct ke_reappraisal r = { .delta = 0.5f, .receipt_id = 42 };
	struct ke_reappraisal r2 = { .delta = -0.1f, .receipt_id = 7 };

	ke_reset(NULL);
	TEST_ASSERT_EQUAL(-2, ke_reappraisal_request(&r)); /* 越界拒 */
	r.delta = 0.2f;
	TEST_ASSERT_EQUAL(0, ke_reappraisal_request(&r));
	ke_step(0, &in, &out);
	/* s_val=0.2 施加后随 raw=0 低通一拍 → 0.2·3/4 */
	TEST_ASSERT_TRUE(feq(out.state.valence, 0.15f));
	TEST_ASSERT_TRUE(feq(ke_reappraisal_applied(), 0.2f));
	TEST_ASSERT_TRUE(ke_reappraisal_receipt(42));
	TEST_ASSERT_EQUAL(0, ke_reappraisal_request(&r2)); /* 覆盖 */
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, (0.15f - 0.1f) * 0.75f));
	TEST_ASSERT_TRUE(ke_reappraisal_receipt(7));
	TEST_ASSERT_FALSE(ke_reappraisal_receipt(42));
	TEST_ASSERT_TRUE(feq(ke_reappraisal_applied(), -0.1f));
}

/*
 * 伦理闩锁（V=−0.35/N=3 仿真定档，见源文件头注）：
 * 路径1 seed−0.95 无驱力衰减：−0.7125/−0.5344/−0.4008 → 第 3 拍闩锁
 * 粘滞；ke_ethics_clear 解锁（owner 批准语义）。
 * 路径2（负向）双驱力崩 raw=−2：−0.5/−0.375 仅 2 拍 → 不闩锁。
 */
static void golden_ethics_latch(void)
{
	struct ke_inputs in = { .have_drive = false };
	struct ke_output out;
	struct ke_state seed = { .valence = -0.95f };
	int i;

	ke_reset(&seed);
	for (i = 0; i < 2; i++) {
		ke_step(i, &in, &out);
		TEST_ASSERT_FALSE(out.state.ethics_latched);
	}
	ke_step(2, &in, &out);
	TEST_ASSERT_TRUE(out.state.ethics_latched);
	ke_step(3, &in, &out);
	TEST_ASSERT_TRUE(out.state.ethics_latched);   /* 粘滞 */
	ke_ethics_clear();
	ke_step(4, &in, &out);
	TEST_ASSERT_FALSE(out.state.ethics_latched);

	ke_reset(NULL);
	in.have_drive = true;
	in.drive_error_sum = 2.0f;
	ke_step(0, &in, &out);                        /* −0.5：cnt1 */
	TEST_ASSERT_FALSE(out.state.ethics_latched);
	ke_step(1, &in, &out);                        /* −0.375：cnt2 */
	TEST_ASSERT_FALSE(out.state.ethics_latched);
	ke_step(2, &in, &out);                        /* 回升：清零 */
	TEST_ASSERT_FALSE(out.state.ethics_latched);
}

/* ke_reset 全清（S6）：残值不得泄漏到下一局 */
static void golden_reset_clears(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_sum = 1.0f,
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

/*
 * D5 缺席保持：!have_drive 拍 valence 仍按 raw=0 衰减（变化率语义），
 * 但 arousal/urgency 保持误差源继续向 held 值收敛（禁向 0 塌缩）
 */
static void golden_no_drive_hold(void)
{
	struct ke_inputs in = { .have_drive = true,
				.drive_error_sum = 1.0f,
				.drive_error_max = 0.9f };
	struct ke_output out;
	float v, a, u;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	ke_step(1, &in, &out);
	v = out.state.valence;
	a = out.state.arousal;
	u = out.state.urgency;
	in.have_drive = false;
	ke_step(2, &in, &out);
	TEST_ASSERT_TRUE(out.state.valence > v);      /* 向 0 衰减非跳变 */
	TEST_ASSERT_TRUE(out.state.arousal > a);      /* 向 0.9 续收敛   */
	TEST_ASSERT_TRUE(out.state.arousal > 0.45f);  /* 而非塌向 0      */
	TEST_ASSERT_TRUE(out.state.urgency > u);
}

/* D7 有效性位：无位喂值 = 保持（禁主动衰减）；置位才更新 */
static void golden_residual_hold(void)
{
	struct ke_inputs in = { 0 };
	struct ke_output out;

	ke_reset(NULL);
	in.sensory_residual_mean = 0.8f;
	ke_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.novelty, 0.0f));  /* 无位保持 */
	in.have_residual = true;
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.novelty, 0.2f));  /* 0.8/4 */
}

/* kd→ke 全链锚：代谢 0.5/拍 → E 0→1.5→3 → valence −0.375/−0.5625
 * 后回升（误差持稳即回暖——变化率语义的链级验证） */
static void golden_kd_ke_chain(void)
{
	const struct kd_params p = {
		.metabolic = { 0.5f, 0.5f, 0.5f },
		.band = 0.1f,
	};
	struct kd_inputs din = { 0 };
	struct kd_output dout;
	struct ke_inputs kin = { .have_drive = true };
	struct ke_output kout;

	kd_init(&p);
	kd_reset();
	ke_reset(NULL);
	kd_step(0, &din, &dout);
	kin.drive_error_sum = dout.error_sum;
	kin.drive_error_max = dout.error_max;
	ke_step(0, &kin, &kout);
	TEST_ASSERT_TRUE(feq(kout.state.valence, -0.375f)); /* −1.5/4 */
	kd_step(1, &din, &dout);
	kin.drive_error_sum = dout.error_sum;    /* E 1.5→3.0 */
	kin.drive_error_max = dout.error_max;
	ke_step(1, &kin, &kout);
	/* 低通第二拍：−0.375+(−1.5+0.375)/4 = −0.65625 */
	TEST_ASSERT_TRUE(feq(kout.state.valence, -0.65625f));
	kd_step(2, &din, &dout);          /* E 持稳 3.0 → raw 0 */
	kin.drive_error_sum = dout.error_sum;
	kin.drive_error_max = dout.error_max;
	ke_step(2, &kin, &kout);
	TEST_ASSERT_TRUE(feq(kout.state.valence, -0.4921875f)); /* 回升 */
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
	RUN_TEST(golden_constant_error_long);
	RUN_TEST(golden_error_step_jump);
	RUN_TEST(golden_urgency_gate);
	RUN_TEST(golden_reappraisal_channel);
	RUN_TEST(golden_ethics_latch);
	RUN_TEST(golden_reset_clears);
	RUN_TEST(golden_no_drive_hold);
	RUN_TEST(golden_residual_hold);
	RUN_TEST(golden_kd_ke_chain);
	RUN_TEST(golden_social_bond);
	UNITY_END();
}
