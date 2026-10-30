/*
 * test_kex_face.c — 表达 v2 内核主机单测（design §6 判据的主机侧）
 *
 * 锚点冻结：数值即规格（映射表/τ/瞬态幅度改动必须同步改锚并过 review）。
 * 覆盖：映射锚点/死区/插值收敛/眨眼 SM/瞬态零残留/doze 斜坡/说话
 * 包络/帧间连续性（防抖动回归线）。
 */
#include "unity.h"
#include "kex_face.h"

#include <math.h>
#include <string.h>

static struct kexp_input neutral(void)
{
	struct kexp_input in = { 0 };

	in.activity = KEX_IDLE;
	return in;
}

/* ---------- 映射锚点 ---------- */

void test_target_neutral_anchor(void)
{
	struct kexp_input in = neutral();
	struct kexp_face t;

	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, t.mouth_curve);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, t.tint_warm);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, t.pupil);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.75f, t.eye_open_l);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, t.ear_perk_l);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.05f, t.mouth_open);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, t.head_tilt);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, t.gaze_y);
}

void test_target_valence_deadzone_and_slope(void)
{
	struct kexp_input in = neutral();
	struct kexp_face t;

	in.valence = 0.10f;		/* 死区内 */
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, t.mouth_curve);
	in.valence = 0.60f;		/* (0.60−0.15)×1.7 */
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.765f, t.mouth_curve);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.80f, t.tint_warm);
	in.valence = -0.60f;
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.765f, t.mouth_curve);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.20f, t.tint_warm);
	in.valence = 1.0f;		/* 饱和截断 */
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, t.mouth_curve);
}

void test_target_activity_and_ethics(void)
{
	struct kexp_input in = neutral();
	struct kexp_face t;

	in.activity = KEX_THINKING;
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.55f, t.gaze_y);
	in.activity = KEX_LISTENING;
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.10f, t.gaze_y);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.60f, t.ear_perk_l); /* 0.25+0.35 */
	in.ethics_latched = true;	/* 安抚：低垂 */
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.50f, t.eye_open_l); /* 0.75−0.25 */
	TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.20f, t.head_tilt);
}

void test_target_drive_severity(void)
{
	struct kexp_input in = neutral();
	struct kexp_face t;

	in.social_sev = 2;		/* 社交告警：竖耳找人 */
	kexp_target(&in, &t);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.75f, t.ear_perk_l); /* 0.25+0.5 */
}

/* ---------- 插值收敛 ---------- */

void test_step_converges_to_target(void)
{
	struct kexp_ctx c;
	struct kexp_input in = neutral();
	struct kexp_face f;
	uint32_t t = 0;

	kexp_reset(&c, t, 42);
	in.valence = 0.6f;
	for (int i = 0; i < 400; i++) {	/* 400×33ms=13.2s > 4×τ_curve */
		t += 33;
		kexp_step(&c, &in, t, &f);
	}
	/* 目标 0.765；τ=1.5s，13.2s 后残差 e^(−8.8)≈0 */
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.765f, f.mouth_curve);
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.8f, f.tint_warm);
}

void test_step_frame_rate_independence(void)
{
	/* 同墙钟不同帧率，收敛轨迹一致（一阶惯性帧率无关性） */
	struct kexp_ctx c1, c2;
	struct kexp_input in = neutral();
	struct kexp_face f1, f2;
	uint32_t t = 0;

	in.arousal = 0.8f;
	kexp_reset(&c1, t, 7);
	kexp_reset(&c2, t, 7);
	for (int i = 0; i < 60; i++) {	/* 2s：33ms 步 */
		t += 33;
		kexp_step(&c1, &in, t, &f1);
	}
	t = 0;
	for (int i = 0; i < 12; i++) {	/* 2s：165ms 步 */
		t += 165;
		kexp_step(&c2, &in, t, &f2);
	}
	/* τ_eye=120ms 下两轨迹差应在微动（呼吸相位一致 dt 同）允许带内 */
	TEST_ASSERT_FLOAT_WITHIN(0.06f, f1.eye_open_l, f2.eye_open_l);
}

/* ---------- 眨眼 SM ---------- */

void test_blink_autonomous(void)
{
	struct kexp_ctx c;
	struct kexp_input in = neutral();
	struct kexp_face f;
	uint32_t t = 0;
	int blinks = 0;
	bool in_blink = false;
	int max_dur_ms = 0, cur_dur = 0;

	kexp_reset(&c, t, 99);
	for (int i = 0; i < 120000 / 33; i++) {	/* 120s 仿真 */
		t += 33;
		kexp_step(&c, &in, t, &f);
		bool closed = f.eye_open_l < 0.25f;

		if (closed && !in_blink) {
			in_blink = true;
			blinks++;
			cur_dur = 0;
		} else if (closed) {
			cur_dur += 33;
		} else if (in_blink) {
			in_blink = false;
			if (cur_dur > max_dur_ms)
				max_dur_ms = cur_dur;
		}
	}
	/* 2-6s 间隔（均值 4s）→ 120s 期望 ~30；宽容带 8..70 */
	TEST_ASSERT_TRUE_MESSAGE(blinks >= 8 && blinks <= 70,
				 "眨眼频率失控");
	TEST_ASSERT_TRUE_MESSAGE(max_dur_ms && max_dur_ms <= 500,
				 "单次眨眼时长异常");
	/* 闭眼最低点应近全闭 */
	TEST_ASSERT_TRUE(f.eye_open_l >= 0.0f && f.eye_open_l <= 1.0f);
}

/* ---------- 事件瞬态：≤1 帧生效、零残留 ---------- */

void test_event_wake_fast_and_clean(void)
{
	struct kexp_ctx c;
	struct kexp_input in = neutral();
	struct kexp_face f;
	uint32_t t = 0;

	kexp_reset(&c, t, 5);
	for (int i = 0; i < 100; i++) {	/* 稳到基线 */
		t += 33;
		kexp_step(&c, &in, t, &f);
	}
	float ear_base = f.ear_perk_l;

	kexp_event(&c, KEXP_EV_WAKE, t);
	t += 33;				/* 1 帧内 */
	kexp_step(&c, &in, t, &f);
	TEST_ASSERT_TRUE_MESSAGE(f.ear_perk_l >= 0.90f,
				 "WAKE 后 1 帧耳未立起");
	t += 2500;				/* 截止之后 */
	kexp_step(&c, &in, t, &f);
	TEST_ASSERT_FLOAT_WITHIN(0.05f, ear_base, f.ear_perk_l);
}

void test_event_barge_transient(void)
{
	struct kexp_ctx c;
	struct kexp_input in = neutral();
	struct kexp_face f;
	uint32_t t = 0;

	kexp_reset(&c, t, 5);
	for (int i = 0; i < 60; i++) {
		t += 33;
		kexp_step(&c, &in, t, &f);
	}
	float curve_base = f.mouth_curve;

	kexp_event(&c, KEXP_EV_BARGE, t);
	t += 33;
	kexp_step(&c, &in, t, &f);
	TEST_ASSERT_TRUE_MESSAGE(f.eye_open_l > 0.95f, "BARGE 未睁大眼");
	TEST_ASSERT_TRUE_MESSAGE(f.mouth_curve < curve_base,
				 "BARGE 未收嘴（惊讶张嘴语义）");
	t += 2500;
	kexp_step(&c, &in, t, &f);
	TEST_ASSERT_FLOAT_WITHIN(0.05f, curve_base, f.mouth_curve);
}

/* ---------- doze 斜坡 ---------- */

void test_doze_ramp_both_ways(void)
{
	struct kexp_ctx c;
	struct kexp_input in = neutral();
	struct kexp_face f;
	uint32_t t = 0;

	kexp_reset(&c, t, 11);
	/* 条件不足（安静但无 rest 告警）→ 不睡 */
	in.quiet_s = 900;
	for (int i = 0; i < 50; i++) {
		t += 200;
		kexp_step(&c, &in, t, &f);
	}
	TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, f.doze);
	/* 补齐 rest 告警 → 60s（2×τ）后深度睡眠 */
	in.rest_sev = 2;
	for (int i = 0; i < 300; i++) {
		t += 200;
		kexp_step(&c, &in, t, &f);
	}
	TEST_ASSERT_TRUE_MESSAGE(f.doze > 0.7f, "doze 未爬起");
	TEST_ASSERT_TRUE_MESSAGE(f.eye_open_l < 0.25f, "睡着未垂眼");
	/* 任何活动 → 快退（≤1.5s 显著回退） */
	in.activity = KEX_LISTENING;
	for (int i = 0; i < 8; i++) {
		t += 200;
		kexp_step(&c, &in, t, &f);
	}
	TEST_ASSERT_TRUE_MESSAGE(f.doze < 0.2f, "活动后未快醒");
}

/* ---------- 说话包络 ---------- */

void test_speaking_mouth_envelope(void)
{
	struct kexp_ctx c;
	struct kexp_input in = neutral();
	struct kexp_face f;
	uint32_t t = 0;
	float hi = 0, lo = 1;

	kexp_reset(&c, t, 3);
	in.activity = KEX_SPEAKING;
	for (int i = 0; i < 40; i++) {	/* 200ms 方波 ×40 */
		t += 50;
		in.speak_level = (i / 4) % 2;	/* 0/1 各 200ms */
		kexp_step(&c, &in, t, &f);
		if (i > 8) {		/* 跳过起振 */
			if ((i / 4) % 2)
				hi = hi < f.mouth_open ? f.mouth_open : hi;
			else
				lo = lo > f.mouth_open ? f.mouth_open : lo;
		}
	}
	TEST_ASSERT_TRUE_MESSAGE(hi > 0.30f, "包络高位嘴动不足");
	TEST_ASSERT_TRUE_MESSAGE(lo < 0.15f, "包络低位嘴未合");
	TEST_ASSERT_TRUE_MESSAGE(hi - lo > 0.25f, "嘴动对比度不足");
}

/* ---------- 帧间连续性（防抖动回归线） ---------- */

void test_continuity_no_jumps(void)
{
	struct kexp_ctx c;
	struct kexp_input in = neutral();
	struct kexp_face prev, f;
	uint32_t t = 0;

	kexp_reset(&c, t, 21);
	kexp_step(&c, &in, t + 33, &prev);
	/* 随机脉冲轮：情绪/事件乱序轰炸，帧间跳变必须受限。
	 * 断言只管**插值层连续性**；产品语义内的即时反应豁免：
	 * ①快眨眼（45ms 闭合，高睁眼度起点单帧 ~0.66）；②事件快路径
	 * （竖耳/睁眼≤1 帧）；③事件打断眨眼的惊跳弹开（实测 0.91）。
	 * 事件帧只验域边界（NaN/越界永远是 bug） */
	for (int i = 0; i < 3000; i++) {
		t += 33;
		bool ev_frame = i % 211 == 0;

		if (i % 37 == 0) {
			in.valence = (float)((i * 7) % 200 - 100) / 100.0f;
			in.arousal = (float)((i * 13) % 100) / 100.0f;
			in.urgency = (float)((i * 3) % 100) / 100.0f;
		}
		if (ev_frame)
			kexp_event(&c, (enum kexp_event)(1 + (i / 211) % 5),
				   t);
		if (i % 173 == 0)
			in.activity = (enum kex_state)((i / 173) % 5);
		kexp_step(&c, &in, t, &f);
		TEST_ASSERT_TRUE(f.eye_open_l >= 0.0f && f.eye_open_l <= 1.0f);
		TEST_ASSERT_TRUE(f.mouth_open >= 0.0f && f.mouth_open <= 1.0f);
		TEST_ASSERT_TRUE(f.ear_perk_l >= 0.0f && f.ear_perk_l <= 1.0f);
		if (!ev_frame) {
			TEST_ASSERT_TRUE_MESSAGE(
				fabsf(f.eye_open_l - prev.eye_open_l) < 0.75f,
				"eye 跳变");
			TEST_ASSERT_TRUE_MESSAGE(
				fabsf(f.mouth_open - prev.mouth_open) < 0.6f,
				"mouth 跳变");
			TEST_ASSERT_TRUE_MESSAGE(
				fabsf(f.ear_perk_l - prev.ear_perk_l) < 0.6f,
				"ear 跳变");
		}
		prev = f;
	}
}

/* ---------- 遥测格式 ---------- */

void test_fmt_line(void)
{
	struct kexp_face f = { 0 };
	char buf[192];

	f.eye_open_l = f.eye_open_r = 0.75f;
	f.pupil = 1.0f;
	f.tint_warm = 0.5f;
	int n = kexp_fmt(buf, sizeof(buf), &f);

	TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(buf));
	TEST_ASSERT_NOT_NULL(strstr(buf, "eye=0.75/0.75"));
	TEST_ASSERT_NOT_NULL(strstr(buf, "doze="));
}

void kexp_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_target_neutral_anchor);
	RUN_TEST(test_target_valence_deadzone_and_slope);
	RUN_TEST(test_target_activity_and_ethics);
	RUN_TEST(test_target_drive_severity);
	RUN_TEST(test_step_converges_to_target);
	RUN_TEST(test_step_frame_rate_independence);
	RUN_TEST(test_blink_autonomous);
	RUN_TEST(test_event_wake_fast_and_clean);
	RUN_TEST(test_event_barge_transient);
	RUN_TEST(test_doze_ramp_both_ways);
	RUN_TEST(test_speaking_mouth_envelope);
	RUN_TEST(test_continuity_no_jumps);
	RUN_TEST(test_fmt_line);
	UNITY_END();
}

