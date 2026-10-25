/*
 * kaleido_emotion.h — 九维感受态情感引擎（Galatea L05 affect 的 C 提取）
 *
 * 设计依据：docs/research/18-Galatea情感模型提取评估.md §2/§4
 *  - 确定性动力学：一阶低通（τ_int=4/τ_dec=16 拍）、迟滞门、饱和、
 *    two_step 重评价——零 RNG、零墙钟（tick 由调用方供给，1Hz 拍）
 *  - 安全范式整体继承：LLM 禁直写情感数值（D-13）；重评价唯一例外通道
 *    = 有界 delta + TTL + 回执（18号 §2.2/§4.4）
 *  - Kaleido 特有：social_bond 维（替换 Galatea MW0 单主体的恒 0 维）、
 *    ethics_latched 伦理闩锁（极端持续负面 → 只允许安抚模板）
 *
 * 拍语义：1 拍 = 1Hz 调用一次 ke_step()；TTL/τ 均以拍计。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 感受态（九维；18号 §2.1 的 Kaleido 剪裁版）---- */

struct ke_state {
	float valence;         /* [-1,1] 唯一带符号维（改善正/恶化负） */
	float arousal;         /* [0,1]                            */
	float urgency;         /* [0,1]                            */
	float persistence;     /* [0,1] 慢谱（|valence| 达伦理阈=1） */
	float controllability; /* [0,1]                            */
	float novelty;         /* [0,1] 感觉预测误差供给           */
	float uncertainty;     /* [0,1] 信念不确定性供给            */
	float social_bond;     /* [0,1] 互动强度积分（Kaleido 特有） */
	bool ethics_latched;   /* 极端持续负面闩锁（只允许安抚模板） */
};

/* ---- 每拍输入（全部 [0,1]，驱力误差类单侧 ≥0；缺省语义=保持）---- */

struct ke_inputs {
	bool have_drive;         /* 驱力误差输入是否有效            */
	float drive_error_mean;  /* Σw·e_d / n（L05 E 项）          */
	float drive_error_max;   /* max e_d（arousal/urgency 源）    */
	bool action_receipt_success; /* 执行回执（→controllability） */
	float sensory_residual_mean; /* 感觉预测误差 → novelty       */
	float belief_uncertainty_mean; /* 1−p_max → uncertainty     */
	bool social_event;       /* 本拍发生真实互动（→social_bond） */
};

/* ---- 输出（消费方：express/动画、speech/TTS hint、LLM prompt 注入）---- */

#define KE_ANIM_TAG_MAX 16

struct ke_output {
	struct ke_state state;
	char anim_tag[KE_ANIM_TAG_MAX];  /* valence×arousal 平面查表     */
	struct {
		float rate;                  /* TTS 语速调制 [0.8,1.2]      */
		float pitch;                 /* 音高调制 [-2,+2] 半音        */
	} tts_hint;
	bool ignition_candidate;         /* urgency 过激活门（>0.1）     */
	uint8_t hyst_saturated : 1;      /* 饱和命中（本拍）             */
};

/*
 * 重评价请求（LLM → 情感的唯一合法通道；有界+TTL+回执）。
 * delta 经 two_step 低通重算生效，禁历史改写（18号 §2.2）。
 */
struct ke_reappraisal {
	float delta;    /* ∈ [-0.2,+0.2] 有界               */
	uint8_t ttl;    /* 生效窗口（拍），>0                */
	uint64_t receipt_id; /* 回执编号（调用方生成，>0）   */
};

int ke_reappraisal_request(const struct ke_reappraisal *req);
bool ke_reappraisal_receipt(uint64_t receipt_id); /* 是否已生效 */

/* 引擎：ke_step 每拍调用（tick 单调递增，仅用于回执台账） */
void ke_step(uint64_t tick, const struct ke_inputs *in,
	     struct ke_output *out);
const struct ke_state *ke_state_snapshot(void);
void ke_reset(const struct ke_state *init); /* NULL=全零 */

/* 动力学原语（测试与 ignition 复用；18号 §2.2 六算子） */
float ke_lowpass(float s, float x, float tau);
bool ke_hysteresis(bool on, float x, float band);

#ifdef __cplusplus
}
#endif
