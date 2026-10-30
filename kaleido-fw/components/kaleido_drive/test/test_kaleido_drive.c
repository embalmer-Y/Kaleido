/*
 * test_kaleido_drive.c — 三驱力动力学黄金回归（Unity）
 *
 * 期望值全部手算；自定义 params 精确步进（metabolic 取 0.5/0.1 等
 * 二进制精确值，避免浮点累积噪声）。锚：28 号 §2.1/§2.3/§9.1。
 */
#include <math.h>
#include <string.h>

#include "kaleido_drive.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 精确步进表：每拍水平 −0.5（rest 恢复 +0.25 供 quiet 用例） */
static const struct kd_params s_fast = {
	.metabolic = { 0.5f, 0.5f, 0.5f },
	.rest_recover = 0.25f,
	.band = 0.1f,
	/* pulse/stim_event 缺省=0：用例单独给（0 覆盖语义清晰） */
};

/* 脉冲表（C1 整改用：非零 stim_event，验证 stim 通路） */

static void test_reset_full_levels(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;

	kd_init(NULL);
	kd_reset();
	kd_step(0, &in, &out);
	/* 默认表代谢首拍即消耗：0.9998/0.9999/0.9997（误差=代谢） */
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.9998f));
	TEST_ASSERT_TRUE(feq(out.level[KD_REST], 0.9997f));
	TEST_ASSERT_TRUE(feq(out.error_sum, 6e-4f));
	TEST_ASSERT_TRUE(feq(out.error_max, 3e-4f));
	TEST_ASSERT_EQUAL_INT(KD_SEV_NONE, out.sev[KD_SOCIAL]);
	TEST_ASSERT_TRUE(feq(out.trend[KD_REST], 3e-4f));
}

/* 代谢精确步进：0.5/拍；sum/max 聚合；HIGH/CRITICAL 边界 */
static void test_metabolic_decay_precise(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;

	kd_init(&s_fast);
	kd_reset();
	kd_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.5f));
	TEST_ASSERT_TRUE(feq(out.error[KD_SOCIAL], 0.5f));
	TEST_ASSERT_TRUE(feq(out.error_sum, 1.5f));
	TEST_ASSERT_TRUE(feq(out.error_max, 0.5f));
	TEST_ASSERT_EQUAL_INT(KD_SEV_HIGH, out.sev[KD_REST]); /* ≥0.5 含边界 */
	TEST_ASSERT_TRUE(feq(out.trend[KD_SOCIAL], 0.5f));
	kd_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.0f));
	TEST_ASSERT_TRUE(feq(out.error_sum, 3.0f));
	TEST_ASSERT_EQUAL_INT(KD_SEV_CRITICAL, out.sev[KD_STIM]);
}

/* severity_of 四档边界（源 drives.py:28-40）。
 * 参数纪律：metabolic 取二进制精确值（0.25/0.05）——0.1 类非精确值
 * 会使 1−0.9f=0.100000024 越过 >band 边界（float 精度陷阱） */
static void test_severity_boundaries(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;
	const struct kd_params band_and_step = {
		.metabolic = { 0.25f, 0.25f, 0.25f },
		.band = 0.15f,
	};
	int i;

	kd_init(&band_and_step);
	kd_reset();
	for (i = 0; i < 4; i++) {
		kd_step(i, &in, &out);
		switch (i) {
		case 0:	/* e=0.25 > band → LOW */
			TEST_ASSERT_EQUAL_INT(KD_SEV_LOW, out.sev[KD_SOCIAL]);
			break;
		case 1:	/* e=0.5 → HIGH（边界含） */
			TEST_ASSERT_EQUAL_INT(KD_SEV_HIGH, out.sev[KD_SOCIAL]);
			break;
		case 2:	/* e=0.75 仍 HIGH */
			TEST_ASSERT_EQUAL_INT(KD_SEV_HIGH, out.sev[KD_SOCIAL]);
			break;
		case 3:	/* e=1.0 → CRITICAL */
			TEST_ASSERT_EQUAL_INT(KD_SEV_CRITICAL,
					      out.sev[KD_SOCIAL]);
			break;
		}
	}
	/* NONE 档：慢代谢 0.05，e=0.05 ≤ band → NONE */
	{
		const struct kd_params gentle = {
			.metabolic = { 0.05f, 0.05f, 0.05f },
			.band = 0.15f,
		};

		kd_init(&gentle);
		kd_reset();
		kd_step(0, &in, &out);
		TEST_ASSERT_EQUAL_INT(KD_SEV_NONE, out.sev[KD_SOCIAL]);
	}
}

/* 脉冲恢复（social 通路）：ROUND_OK +0.15；trend 翻负（好转） */
static void test_pulse_recovery(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;
	const struct kd_params p = {
		.metabolic = { 0.1f, 0.0f, 0.0f },
		.pulse = {
			[KD_EVT_ROUND_OK] = 0.15f,
			[KD_EVT_ROUND_FAIL] = 0.03f,
			[KD_EVT_BARGE] = 0.05f,
			[KD_EVT_VOICE] = 0.005f,
			[KD_EVT_TOUCH] = 0.05f,
		},
		.stim_event = 0.10f,
		.band = 0.1f,
	};

	kd_init(&p);
	kd_reset();
	kd_step(0, &in, &out);              /* social 0.9, e 0.1 */
	in.event = KD_EVT_ROUND_OK;
	kd_step(1, &in, &out);              /* 0.9−0.1+0.15=0.95 */
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.95f));
	TEST_ASSERT_TRUE(feq(out.trend[KD_SOCIAL], -0.05f));
}

/* stim 通路专测（评审 C1：原断言被满值钳位掩蔽，stim 分支曾可整段
 * 删掉仍全绿）。参数全二进制精确：0.875−0.125+0.25=1.0 无浮点噪声 */
static void test_stim_path(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;
	const struct kd_params p = {
		.metabolic = { 0.0f, 0.125f, 0.0f },
		.pulse = { [KD_EVT_ROUND_OK] = 0.15f },
		.stim_event = 0.25f,        /* BARGE 同走此参数 */
		.band = 0.1f,
	};

	kd_init(&p);
	kd_reset();
	kd_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.level[KD_STIM], 0.875f));
	in.event = KD_EVT_ROUND_OK;
	kd_step(1, &in, &out);              /* 0.875−0.125+0.25 */
	TEST_ASSERT_TRUE(feq(out.level[KD_STIM], 1.0f));
	in.event = KD_EVT_NONE;
	kd_step(2, &in, &out);
	in.event = KD_EVT_BARGE;
	kd_step(3, &in, &out);              /* 0.875−0.125+0.25 再证 */
	TEST_ASSERT_TRUE(feq(out.level[KD_STIM], 1.0f));
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 1.0f)); /* BARGE 喂
					    * social +0.05，满值钳位 1.0 */
}

/* 小脉冲仅 social（评审 C1 整改：stim 预降位后断言 FAIL 不喂 stim） */
static void test_small_pulses_social_only(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;
	const struct kd_params p = {
		.metabolic = { 0.1f, 0.125f, 0.0f },
		.pulse = {
			[KD_EVT_ROUND_FAIL] = 0.03f,
			[KD_EVT_VOICE] = 0.005f,
			[KD_EVT_TOUCH] = 0.05f,
		},
		.band = 0.1f,               /* stim_event=0：FAIL 期无刺激源 */
	};

	kd_init(&p);
	kd_reset();
	kd_step(0, &in, &out);              /* social 0.9 / stim 0.875 */
	in.event = KD_EVT_ROUND_FAIL;
	kd_step(1, &in, &out);              /* social 0.9−0.1+0.03 */
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.83f));
	TEST_ASSERT_TRUE(feq(out.level[KD_STIM], 0.75f)); /* 仅代谢：
					    * 若误喂 FAIL 脉冲则 ≠0.75 */
	in.event = KD_EVT_VOICE;
	kd_step(2, &in, &out);              /* 0.83−0.1+0.005 */
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.735f));
	in.event = KD_EVT_TOUCH;
	kd_step(3, &in, &out);              /* 0.735−0.1+0.05 */
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.685f));
}

/* rest 双恢复源（quiet/charging）与钳位 */
static void test_rest_recover(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;

	kd_init(&s_fast);
	kd_reset();
	in.quiet = true;
	kd_step(0, &in, &out);              /* 1−0.5+0.25 */
	TEST_ASSERT_TRUE(feq(out.level[KD_REST], 0.75f));
	kd_step(1, &in, &out);              /* 0.75−0.5+0.25 */
	TEST_ASSERT_TRUE(feq(out.level[KD_REST], 0.5f));
	in.quiet = false;
	in.charging = true;                 /* charging 同路 */
	kd_step(2, &in, &out);
	TEST_ASSERT_TRUE(feq(out.level[KD_REST], 0.25f));
}

/* 越界事件值安全归 NONE；空参安全 */
static void test_input_robustness(void)
{
	struct kd_inputs in = { .event = (enum kd_event)99 };
	struct kd_output out;

	kd_init(&s_fast);
	kd_reset();
	kd_step(0, &in, &out);              /* 无脉冲、无恢复 */
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.5f));
	kd_step(1, NULL, &out);             /* 空 in 忽略 */
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.5f));
	memset(&out, 0xaa, sizeof(out));
	kd_step(2, &in, NULL);              /* 空 out 忽略 */
}

/* 内置冻结表：起步参数量级对（10 拍代谢后仍在恢复带内） */
static void test_default_params_scale(void)
{
	struct kd_inputs in = { 0 };
	struct kd_output out;
	int i;

	kd_init(NULL);
	kd_reset();
	for (i = 0; i < 10; i++)
		kd_step(i, &in, &out);
	TEST_ASSERT_TRUE(feq(out.level[KD_SOCIAL], 0.998f));
	TEST_ASSERT_TRUE(feq(out.level[KD_STIM], 0.999f));
	TEST_ASSERT_TRUE(feq(out.level[KD_REST], 0.997f));
	TEST_ASSERT_TRUE(feq(out.error_sum, 0.006f));
	TEST_ASSERT_EQUAL_INT(KD_SEV_NONE, out.sev[KD_REST]);
}

void drive_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_reset_full_levels);
	RUN_TEST(test_metabolic_decay_precise);
	RUN_TEST(test_severity_boundaries);
	RUN_TEST(test_pulse_recovery);
	RUN_TEST(test_stim_path);
	RUN_TEST(test_small_pulses_social_only);
	RUN_TEST(test_rest_recover);
	RUN_TEST(test_input_robustness);
	RUN_TEST(test_default_params_scale);
	UNITY_END();
}
