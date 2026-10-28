/*
 * kaleido_emotion.h — 九维感受态情感引擎（Galatea L05 affect 的 C 提取）
 *
 * 设计依据：docs/research/18-Galatea情感模型提取评估.md §2/§4 +
 * 28 号深挖（W0-2 对源偏差修正 D1-D7 落地）。
 *  - 确定性动力学：一阶低通（τ_int=4/τ_dec=16 拍）、迟滞门、饱和、
 *    一次性重评价——零 RNG、零墙钟（tick 由调用方供给，1Hz 拍）
 *  - 安全范式整体继承：LLM 禁直写情感数值（D-13）；重评价唯一例外通道
 *    = 有界 delta + 回执（28 号 D4：改一次性精确施加，applied 可审计）
 *  - Kaleido 特有：social_bond 维、ethics_latched 伦理闩锁（极端持续
 *    负面 → 只允许安抚模板；ke_ethics_clear 解锁=owner 批准语义）
 *
 * 拍语义：1 拍 = 1Hz 调用一次 ke_step()；τ 以拍计。
 */
#ifndef KALEIDO_EMOTION_H
#define KALEIDO_EMOTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 感受态（九维；18 号 §2.1 的 Kaleido 剪裁版）---- */

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

/* ---- 每拍输入 ---- */

/* 执行回执三态（G1：无事件拍保持 controllability；一轮结算拍喂一次） */
enum ke_receipt {
	KE_RECEIPT_NONE = 0,
	KE_RECEIPT_SUCCESS,
	KE_RECEIPT_FAIL,
};

struct ke_inputs {
	bool have_drive;         /* 驱力误差输入是否有效（无效拍
				    arousal/urgency 保持——D5 源缺席语义） */
	float drive_error_sum;   /* E = Σw·e（**sum 非 mean**，28 号 D1：
				    kd error_sum 直供；raw valence 域随
				    权重与驱力数，发布 clip [−1,1]）   */
	float drive_error_max;   /* max e_d（arousal/urgency 源）      */
	enum ke_receipt receipt; /* 执行回执（→controllability）       */
	bool have_residual;      /* novelty 输入有效性位（D7：无效拍保持）*/
	float sensory_residual_mean; /* 感觉预测误差 → novelty         */
	bool have_uncertainty;   /* uncertainty 输入有效性位（D7）      */
	float belief_uncertainty_mean; /* 1−p_max → uncertainty        */
	bool social_event;       /* 本拍发生真实互动（→social_bond）   */
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
	bool ignition_candidate;         /* urgency 过激活门（>0.1；
					    Kaleido 附加迟滞，非源语义） */
	uint8_t hyst_saturated : 1;      /* 饱和命中（本拍）             */
};

/*
 * 重评价请求（LLM → 情感的唯一合法通道；有界+回执）。
 * 28 号 D4：一次性精确施加——下一拍把钳位后的 delta 直接加进积分器
 * （禁历史改写；TTL 斜坡已废，实际施加量=请求值）。回执经
 * ke_reappraisal_receipt 查询、ke_reappraisal_applied 审计。
 */
struct ke_reappraisal {
	float delta;    /* ∈ [-0.2,+0.2] 有界（越界整单拒绝）  */
	uint64_t receipt_id; /* 回执编号（调用方生成，>0）     */
};

/* 重评价三件套（评审 C3 契约）：与 ke_step **同任务**调用（组件
 * 文件级 static 无锁——soul tick 单写者持有；异任务请求须经调用方
 * 侧的 pending 漏斗，如 soul_on_mood）。返回 0 成功；-1 参数错
 * （req 空/receipt_id 0）；-2 delta 越界。 */
int ke_reappraisal_request(const struct ke_reappraisal *req);
bool ke_reappraisal_receipt(uint64_t receipt_id); /* 是否已生效 */
float ke_reappraisal_applied(void);   /* 最近一次实际施加量（审计用） */

/* 引擎：ke_step 每拍调用（tick 预留未消费；台账键=调用方 receipt_id，
 * 评审 C7）。ke_state_snapshot 返回内部指针——仅当拍借用，ke_reset
 * 后失效；跨任务取状态用拷贝（soul_snapshot 模式）。 */
void ke_step(uint64_t tick, const struct ke_inputs *in,
	     struct ke_output *out);
const struct ke_state *ke_state_snapshot(void);
void ke_reset(const struct ke_state *init); /* NULL=全零 */
/* 伦理闩锁解锁（owner 批准语义：云端确认/长正交互后调用；28 号 D3） */
void ke_ethics_clear(void);

/* 动力学原语（测试与 ignition 复用；18 号 §2.2 六算子）
 * ke_hysteresis：band 为**半带宽**（源 dynamics.py:34-40 语义，
 * band=0.1 ⇒ ON ≥0.55 / OFF ≤0.45，边界含）——28 号 D6 修正 */
float ke_lowpass(float s, float x, float tau);
bool ke_hysteresis(bool on, float x, float band);

#ifdef __cplusplus
}
#endif
#endif /* KALEIDO_EMOTION_H */
