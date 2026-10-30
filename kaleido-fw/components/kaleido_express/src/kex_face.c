/*
 * kex_face.c — 表达 v2 参数化猫脸内核实现
 *
 * 设计依据 docs/design-表达层重做v2-2026-10-29.md §2：
 *  - 映射纯函数（kexp_target）：九维→参数目标，死区在 valence 通道
 *   （±0.15 平静微正语义）；
 *  - 插值：每参数一阶惯性 p += (tgt−p)·(1−e^(−dt/τ))，帧率无关；
 *    τ 分档=语义速度（眼动快/情绪底色慢），天然无片段抖动；
 *  - 微动：自主眨眼 SM/呼吸相位/IDLE gaze 游走——"活"感的来源；
 *  - 瞬态：事件叠加层（指数衰减），不改 1Hz 目标，零残留；
 *  - doze：安静≥600s 且 rest 告警→睡眠斜坡（30s τ），任何活动
 *    即快退（800ms τ）。
 *
 * 数值域纪律：所有参数 clamp 在声明域内；无除零（τ>0 常量）；
 * 时间 uint32_t 毫秒回绕由 dt 截断兜底（回绕差经无符号减法仍得
 * 小值，>500ms 截断路径吸收）。
 */
#include "kex_face.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- τ 表（ms）：参数语义速度 ---- */
#define TAU_EYE		120	/* 眼开合跟随 */
#define TAU_GAZE	150	/* 注视移动 */
#define TAU_PUPIL	400
#define TAU_CURVE	1500	/* 情绪底色嘴形 */
#define TAU_MOUTH_SPK	60	/* 说话包络要跟得上音节 */
#define TAU_EAR		300
#define TAU_TILT	3000
#define TAU_TINT	4000
#define TAU_DOZE_UP	30000	/* 入睡慢 */
#define TAU_DOZE_DN	800	/* 醒得快 */

#define TAU_EV_MS	500	/* 事件瞬态衰减常数 */
#define EV_CUTOFF_MS	2500	/* 事件彻底消失 */
#define TAU_EV_IGNORED	1500	/* 被忽略的失落感长尾 */

#define SPEAK_GAIN	0.6f	/* 说话嘴动夸张度（保守档，负责人可调） */
#define QUIET_DOZE_S	600	/* 10min 安静入睡门槛（旧 asleep 判据迁移） */

#define BLINK_CLOSE_MS	45
#define BLINK_HOLD_MS	20
#define BLINK_OPEN_MS	75

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float dzf(float v, float dz)	/* 死区（保持斜率 1，域内平移） */
{
	if (v > dz)
		return v - dz;
	if (v < -dz)
		return v + dz;
	return 0.0f;
}

static uint32_t kexp_rand(struct kexp_ctx *c)	/* LCG：host/board 同序列 */
{
	c->rng = c->rng * 1664525u + 1013904223u;
	return c->rng >> 8;
}

void kexp_reset(struct kexp_ctx *c, uint32_t now_ms, uint32_t seed)
{
	memset(c, 0, sizeof(*c));
	c->rng = seed ? seed : 1;
	c->t_last_ms = now_ms;
	c->cur.eye_open_l = c->cur.eye_open_r = 0.75f;
	c->cur.pupil = 1.0f;
	c->cur.mouth_open = 0.05f;
	c->cur.ear_perk_l = c->cur.ear_perk_r = 0.25f;
	c->cur.tint_warm = 0.5f;
	c->blink_next_ms = now_ms + 1500 + kexp_rand(c) % 3000;
	c->ev = KEXP_EV_NONE;
}

/* ---- 纯映射：九维→参数目标（锚点冻结于单测） ---- */
void kexp_target(const struct kexp_input *in, struct kexp_face *t)
{
	float v = clampf(in->valence, -1.0f, 1.0f);
	float a = clampf(in->arousal, 0.0f, 1.0f);
	float u = clampf(in->urgency, 0.0f, 1.0f);
	bool busy = in->activity == KEX_LISTENING ||
		    in->activity == KEX_THINKING ||
		    in->activity == KEX_SPEAKING;

	memset(t, 0, sizeof(*t));
	/* 嘴形：valence 主通道（±0.15 死区），×1.7 拉满表情幅度 */
	t->mouth_curve = clampf(dzf(v, 0.15f) * 1.7f, -1.0f, 1.0f);
	t->tint_warm = clampf(0.5f + 0.5f * v, 0.0f, 1.0f);
	/* 眼：唤醒抬基线，安抚闩锁低垂；urgency 是吃惊的近亲 */
	t->eye_open_l = t->eye_open_r =
		clampf(0.75f + 0.20f * a + (u >= 0.5f ? 0.10f : 0.0f) -
		       (in->ethics_latched ? 0.25f : 0.0f), 0.1f, 1.0f);
	/* 注视：唤醒微微上抬；THINKING 上看（人抬眼思考的刻板印象） */
	t->gaze_y = 0.15f * a;
	if (in->activity == KEX_THINKING)
		t->gaze_y += 0.55f;
	else if (in->activity == KEX_LISTENING)
		t->gaze_y -= 0.10f;
	t->gaze_y = clampf(t->gaze_y, -1.0f, 1.0f);
	/* 瞳孔：正情绪放大+惊讶积分脉冲 */
	t->pupil = clampf(1.0f + 0.25f * v + in->novelty * 0.003f,
			  0.6f, 1.4f);
	/* 耳：社交驱力告警竖耳，倾听附耳 */
	t->ear_perk_l = t->ear_perk_r =
		clampf(0.25f + (in->social_sev >= 2 ? 0.50f : 0.0f) +
		       0.20f * a +
		       (in->activity == KEX_LISTENING ? 0.35f : 0.0f),
		       0.0f, 1.0f);
	/* 头姿：愉悦轻歪头，安抚低垂 */
	t->head_tilt = clampf(0.15f * v -
			      (in->ethics_latched ? 0.20f : 0.0f),
			      -1.0f, 1.0f);
	/* 嘴开度：说话由包络主导（step 里叠加），静息=唤醒微开 */
	t->mouth_open = clampf(0.05f + 0.10f * a, 0.0f, 0.6f);
	/* doze 由 step 斜坡管理；目标层只给条件初值 0（醒着） */
	t->doze = 0.0f;
	(void)busy;
}

void kexp_event(struct kexp_ctx *c, enum kexp_event ev, uint32_t now_ms)
{
	if (ev == KEXP_EV_NONE)
		return;
	c->ev = ev;
	c->ev_t0_ms = now_ms;
	/* 事件本身带唤醒语义：眨眼立刻复位（睁大眼） */
	c->blink_phase_ms = 0;
	c->blink_next_ms = now_ms + 2500 + kexp_rand(c) % 2500;
}

/* 瞬态叠加：在**输出层**（slew 之后）生效——事件是即时反应，必须
 * ≤1 帧可见，不被 τ 拖慢；衰减 e^(−dt/τ)，截止归零；cur 插值域
 * 保持纯净（遥测对账=底色轨迹） */
static void ev_apply(const struct kexp_ctx *c, uint32_t now_ms,
		     struct kexp_face *f)
{
	float e;

	if (c->ev == KEXP_EV_NONE)
		return;
	uint32_t age = now_ms - c->ev_t0_ms;
	uint32_t cutoff = c->ev == KEXP_EV_IGNORED ? 5000 : EV_CUTOFF_MS;
	uint32_t tau = c->ev == KEXP_EV_IGNORED ? TAU_EV_IGNORED : TAU_EV_MS;

	if (age >= cutoff)
		return;
	e = expf(-(float)age / (float)tau);
	switch (c->ev) {
	case KEXP_EV_WAKE:
		f->ear_perk_l = f->ear_perk_r =
			clampf(f->ear_perk_l + 0.80f * e, 0, 1);
		f->pupil = clampf(f->pupil * (1.0f + 0.30f * e), 0.6f, 1.4f);
		f->eye_open_l = f->eye_open_r =
			clampf(f->eye_open_l + 0.20f * e, 0, 1);
		break;
	case KEXP_EV_BARGE:
		f->eye_open_l = f->eye_open_r =
			clampf(f->eye_open_l + 0.30f * e, 0, 1);
		f->mouth_curve = clampf(f->mouth_curve - 0.30f * e, -1, 1);
		f->pupil = clampf(f->pupil * (1.0f + 0.40f * e), 0.6f, 1.4f);
		f->ear_perk_l = f->ear_perk_r =
			clampf(f->ear_perk_l + 0.90f * e, 0, 1);
		break;
	case KEXP_EV_ROUND_OK:
		f->mouth_curve = clampf(f->mouth_curve + 0.40f * e, -1, 1);
		break;
	case KEXP_EV_ROUND_FAIL:
		f->mouth_curve = clampf(f->mouth_curve - 0.25f * e, -1, 1);
		f->eye_open_l = f->eye_open_r =
			clampf(f->eye_open_l - 0.15f * e, 0, 1);
		break;
	case KEXP_EV_IGNORED:
		f->head_tilt = clampf(f->head_tilt - 0.20f * e, -1, 1);
		f->mouth_curve = clampf(f->mouth_curve - 0.10f * e, -1, 1);
		break;
	default:
		break;
	}
}

/* 一阶惯性推进（帧率无关） */
static float slew(float cur, float tgt, float dt, float tau)
{
	return cur + (tgt - cur) * (1.0f - expf(-dt / tau));
}

void kexp_step(struct kexp_ctx *c, const struct kexp_input *in,
	       uint32_t now_ms, struct kexp_face *out)
{
	struct kexp_face tgt;
	uint32_t dt = now_ms - c->t_last_ms;
	float a = clampf(in->arousal, 0.0f, 1.0f);
	bool doze_cond = in->quiet_s >= QUIET_DOZE_S && in->rest_sev >= 2 &&
			 in->activity == KEX_IDLE;
	float dt_f;

	if (dt > 500)			/* 挂起/回绕兜底 */
		dt = 500;
	if (dt == 0)
		dt = 1;
	dt_f = (float)dt;
	c->t_last_ms = now_ms;

	kexp_target(in, &tgt);

	/* doze 斜坡：入睡眠慢/任何活动快退 */
	c->cur.doze = slew(c->cur.doze, doze_cond ? 1.0f : 0.0f, dt_f,
			   doze_cond ? (float)TAU_DOZE_UP :
				       (float)TAU_DOZE_DN);

	/* 说话包络主导嘴开度（TAU 快档） */
	if (in->activity == KEX_SPEAKING) {
		float want = clampf(SPEAK_GAIN *
				    clampf(in->speak_level, 0, 1), 0, 1);

		if (want < tgt.mouth_open)
			want = tgt.mouth_open;
		c->cur.mouth_open = slew(c->cur.mouth_open, want, dt_f,
					 (float)TAU_MOUTH_SPK);
	} else {
		c->cur.mouth_open = slew(c->cur.mouth_open, tgt.mouth_open,
					 dt_f, (float)TAU_CURVE);
	}

	c->cur.eye_open_l = slew(c->cur.eye_open_l, tgt.eye_open_l, dt_f,
				 (float)TAU_EYE);
	c->cur.eye_open_r = slew(c->cur.eye_open_r, tgt.eye_open_r, dt_f,
				 (float)TAU_EYE);
	c->cur.gaze_x = slew(c->cur.gaze_x, tgt.gaze_x, dt_f,
			     (float)TAU_GAZE);
	c->cur.gaze_y = slew(c->cur.gaze_y, tgt.gaze_y, dt_f,
			     (float)TAU_GAZE);
	c->cur.pupil = slew(c->cur.pupil, tgt.pupil, dt_f, (float)TAU_PUPIL);
	c->cur.mouth_curve = slew(c->cur.mouth_curve, tgt.mouth_curve, dt_f,
				  (float)TAU_CURVE);
	c->cur.ear_perk_l = slew(c->cur.ear_perk_l, tgt.ear_perk_l, dt_f,
				 (float)TAU_EAR);
	c->cur.ear_perk_r = slew(c->cur.ear_perk_r, tgt.ear_perk_r, dt_f,
				 (float)TAU_EAR);
	c->cur.head_tilt = slew(c->cur.head_tilt, tgt.head_tilt, dt_f,
				(float)TAU_TILT);
	c->cur.tint_warm = slew(c->cur.tint_warm, tgt.tint_warm, dt_f,
				(float)TAU_TINT);

	/* ---- 微动层 ---- */
	/* 呼吸：周期 4s，唤醒加快，睡眠减半速 */
	{
		float period = 4000.0f * (1.0f - 0.4f * a) *
			       (1.0f + 1.0f * c->cur.doze);

		c->breath_phase += 2.0f * (float)M_PI * dt_f / period;
		if (c->breath_phase >= 2.0f * (float)M_PI)
			c->breath_phase -= 2.0f * (float)M_PI;
		c->cur.breath = 0.5f - 0.5f * cosf(c->breath_phase);
	}
	/* IDLE gaze 游走（非会话期才像在"自己待着"）——输出层偏移，
	 * 不污染插值域（cur 累加 sin 值=随机游走，首版实现曾踩） */
	if (in->activity == KEX_IDLE || in->activity == KEX_UNCONFIGURED)
		c->drift_phase += 2.0f * (float)M_PI * dt_f / 9000.0f;
	/* 眨眼 SM：间隔 2-6s，唤醒缩短，睡眠稀疏；18% 双眨 */
	{
		float interval_scale = (1.0f - 0.45f * a) *
				       (c->cur.doze > 0.5f ? 3.0f : 1.0f);

		if (c->blink_phase_ms) {
			uint32_t dur = BLINK_CLOSE_MS + BLINK_HOLD_MS +
				       BLINK_OPEN_MS;

			c->blink_phase_ms += dt;
			if (c->blink_phase_ms >=
			    (c->blink_double ? 2 * dur : dur)) {
				c->blink_phase_ms = 0;	/* 完成 */
				c->blink_next_ms = now_ms +
					(uint32_t)((2000 + kexp_rand(c) %
						    4000) * interval_scale);
			}
		} else if ((int32_t)(now_ms - c->blink_next_ms) >= 0) {
			c->blink_phase_ms = 1;
			c->blink_double = (kexp_rand(c) % 100) < 18;
		}
	}
	/* 输出：眨眼/睡眠对眼开度的乘性效果（不动插值域 cur） */
	if (out) {
		float blink_mul = 1.0f;

		if (c->blink_phase_ms) {
			uint32_t p = c->blink_phase_ms;
			uint32_t dur = BLINK_CLOSE_MS + BLINK_HOLD_MS +
				       BLINK_OPEN_MS;

			if (p >= dur && c->blink_double)
				p -= dur;	/* 双眨第二程 */
			if (p < BLINK_CLOSE_MS)
				blink_mul = 1.0f - (float)p / BLINK_CLOSE_MS;
			else if (p < BLINK_CLOSE_MS + BLINK_HOLD_MS)
				blink_mul = 0.0f;
			else
				blink_mul = (float)(p - BLINK_CLOSE_MS -
						    BLINK_HOLD_MS) /
					    BLINK_OPEN_MS;
		}
		*out = c->cur;
		/* 瞬态（输出层即时生效） */
		ev_apply(c, now_ms, out);
		/* IDLE 游走偏移（同层） */
		if (in->activity == KEX_IDLE || in->activity == KEX_UNCONFIGURED)
			out->gaze_x = clampf(out->gaze_x +
					     0.06f * sinf(c->drift_phase),
					     -1, 1);
		out->eye_open_l = clampf(out->eye_open_l * blink_mul *
					 (1.0f - 0.92f * out->doze), 0, 1);
		out->eye_open_r = clampf(out->eye_open_r * blink_mul *
					 (1.0f - 0.92f * out->doze), 0, 1);
	}
}

int kexp_fmt(char *buf, size_t n, const struct kexp_face *f)
{
	return snprintf(buf, n,
			"eye=%.2f/%.2f,gz=%.2f/%.2f,pu=%.2f,mo=%.2f/%.2f,"
			"ear=%.2f,tilt=%.2f,br=%.2f,tint=%.2f,doze=%.2f",
			f->eye_open_l, f->eye_open_r, f->gaze_x, f->gaze_y,
			f->pupil, f->mouth_curve, f->mouth_open,
			(f->ear_perk_l + f->ear_perk_r) / 2, f->head_tilt,
			f->breath, f->tint_warm, f->doze);
}
