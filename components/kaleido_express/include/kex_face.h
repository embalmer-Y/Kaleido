/*
 * kex_face.h — 表达 v2 参数化猫脸内核（design-表达层重做v2 §2）
 *
 * 纯函数组件：无平台依赖、显式时间入参、无全局态——host 单测与
 * 板端同一二进制语义。板上 expr 任务 20-30fps 调 kexp_step，soul
 * tick 1Hz 调 kexp_target 对账（遥测口径）。
 *
 * 分层契约（单向，情绪数值禁直写纪律不变）：
 *   调用方（soul/FSM/事件钩子）──kexp_input──→ 本核 ──kexp_face──→
 *   渲染器（图元→像素）。本核永不回调、永不主动出声/改灵魂态。
 *
 * 线程契约：单写者（expr 任务独占 ctx）；kexp_target 无状态可任意
 * 线程调（遥测对账用）。
 */
#ifndef KEX_FACE_H
#define KEX_FACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kaleido_express.h"	/* enum kex_state（会话活动四态） */

/* ---- 输入（调用方从 soul 快照/会话态填充；1Hz 或更快均可） ---- */
struct kexp_input {
	float valence;		/* -1..1 */
	float arousal;		/* 0..1 */
	float urgency;		/* 0..1 */
	bool ethics_latched;	/* 安抚闩锁 */
	uint8_t social_sev;	/* 驱力告警 0正常 1低 2高 3危急 */
	uint8_t rest_sev;	/* 同上（doze 触发条件之一） */
	uint16_t quiet_s;	/* 安静秒数（doze 触发：≥600） */
	uint8_t novelty;	/* 0..100 惊讶积分（W6b 接入前填 0） */
	enum kex_state activity;/* UNCONFIGURED/IDLE/LISTENING/… */
	float speak_level;	/* 0..1 说话能量包络（SPEAKING 期） */
};

/* ---- 事件（快路径瞬态，绕过 1Hz；衰减后自动回底色不残留） ---- */
enum kexp_event {
	KEXP_EV_NONE = 0,
	KEXP_EV_WAKE,		/* 唤醒词命中：双耳立+瞳孔放大 */
	KEXP_EV_BARGE,		/* 打断：惊讶瞬态 */
	KEXP_EV_ROUND_OK,	/* 轮成功：微笑脉冲 */
	KEXP_EV_ROUND_FAIL,	/* 轮失败：轻垂 */
	KEXP_EV_IGNORED,	/* 主动说话被忽略：失落轻垂（长尾） */
};

/* ---- 输出：帧描述（渲染器只认这套参数，§2.1 参数空间） ---- */
struct kexp_face {
	float eye_open_l, eye_open_r;/* 0..1 */
	float gaze_x, gaze_y;	/* -1..1 */
	float pupil;		/* 0.6..1.4 */
	float mouth_curve;	/* -1..1（正=微笑） */
	float mouth_open;	/* 0..1 */
	float ear_perk_l, ear_perk_r;/* 0..1 */
	float head_tilt;	/* -1..1 */
	float breath;		/* 0..1 呼吸相位 */
	float tint_warm;	/* 0..1（0.5=中性） */
	float doze;		/* 0..1 睡眠深度 */
};

/* ---- 内核状态（expr 任务独占；结构可见便于 host 断言内部量） ---- */
struct kexp_ctx {
	struct kexp_face cur;	/* 当前参数值（插值域） */
	uint32_t t_last_ms;	/* 上次 step 时刻 */
	/* 眨眼状态机 */
	uint32_t blink_next_ms;	/* 下次眨眼起始 */
	uint16_t blink_phase_ms;/* 眨眼进行相位（0=未眨） */
	bool blink_double;	/* 本轮是否双眨 */
	/* 微动相位 */
	float breath_phase;	/* rad */
	float drift_phase;	/* rad（IDLE gaze 游走 LFO） */
	/* 事件瞬态（只保最新一条，自然合并） */
	enum kexp_event ev;
	uint32_t ev_t0_ms;
	/* 轻量确定性 RNG（host/board 同序列；种子可注入测试） */
	uint32_t rng;
};

/* 重置到中性脸（now 为调用方时钟基准；seed 0=固定序列便于测试） */
void kexp_reset(struct kexp_ctx *c, uint32_t now_ms, uint32_t seed);

/* 纯映射：输入→参数目标（无状态；遥测对账与 step 内部共用同一表）。
 * 不含瞬态/微动/doze 叠加——那是 step 的职责 */
void kexp_target(const struct kexp_input *in, struct kexp_face *tgt);

/* 事件快路径：压入瞬态（≤1 帧生效，指数衰减，自动回落） */
void kexp_event(struct kexp_ctx *c, enum kexp_event ev, uint32_t now_ms);

/* 帧推进：目标插值（每参数独立 τ）+ 微动 + 瞬态 + doze 斜坡。
 * now_ms 单调增；dt>500ms 按 500ms 截断（防挂起后跳变）。
 * out 可为 NULL（只推进状态） */
void kexp_step(struct kexp_ctx *c, const struct kexp_input *in,
	       uint32_t now_ms, struct kexp_face *out);

/* 遥测格式化：VL,EXPR 对账行（buf ≥160B） */
int kexp_fmt(char *buf, size_t n, const struct kexp_face *f);

#endif /* KEX_FACE_H */
