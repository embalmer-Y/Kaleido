/*
 * kaleido_ignition.h — 点火机制方案 A：显著性打分 + 防骚扰门 + 滞回
 *
 * 设计依据：docs/research/22-点火机制与决策模型调研.md §4.1（方案 A）
 *  - 五因子打分（时间节律/情感动机/记忆钩子/事件钩子/在场置信）
 *  - AND 硬门：勿扰外 ∧ 冷静期过 ∧ 配额未用 ∧ 用户未说话
 *  - 双阈值滞回（T_high 触发 / T_low 复位）防抖
 *  - 结算反馈：被忽略 → 冷静期指数翻倍；被回应 → 复位（22号 §5.5）
 *  - 日志透明：decision 携带全因子与门状态（供 /ignite 云裁决 prompt）
 * 方案 B（Pareto 仲裁/慢环）演进时本组件降级为因子供给器。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KI_FACTOR_CNT	5

struct ki_factors {
	float time_rhythm;   /* 时间节律（作息匹配度）        */
	float affect_motive; /* 情感动机（ke urgency 门输出） */
	float memory_hook;   /* 记忆钩子（到期承诺/三因子Top1） */
	float event_hook;    /* 事件钩子（充电完成/长时间无互动） */
	float presence;      /* 在场置信（IMU 拿起/VAD 人声） */
};

struct ki_config {
	float w[KI_FACTOR_CNT]; /* 权重（默认等权 0.2）        */
	float t_high;           /* 触发阈值（默认 0.70）       */
	float t_low;            /* 复位阈值（默认 0.50）       */
	uint16_t cooldown_min;  /* 基础冷静期（默认 30 分钟）  */
	uint16_t cooldown_max;  /* 上限（默认 240）            */
	uint8_t daily_quota;    /* 日配额（默认 4）            */
	uint8_t proactivity;    /* 主动度 0-10（persona 注入） */
};

struct ki_context {
	bool user_speaking;    /* VAD：用户说话中 → 禁火      */
	bool quiet_hours;      /* 勿扰时段（本地 RTC 裁决）   */
	uint32_t now_min;      /* 当日分钟数（0-1439）        */
	uint32_t last_fire_min;/* 上次点火（当日分钟）        */
	uint8_t fired_today;   /* 今日已用配额                */
	bool holding;          /* 滞回态（上次过 T_high）     */
};

struct ki_decision {
	bool candidate;        /* 过滞回 + 过硬门             */
	float score;
	struct ki_factors f;
	struct {
		uint8_t gate_mask; /* bit0..3 = quiet/cooldown/
				      quota/speaking，1=被拦 */
	} gates;
};

/* 默认参数（static const 冻结；persona 可覆写副本后传入） */
const struct ki_config *ki_config_default(void);

/*
 * 打分 + 滞回 + 硬门。proactivity<2 时 t_high 上浮
 * （"太黏了"降档语义：22号 §5.3 旋钮统一缩放）。
 */
void ki_step(const struct ki_context *ctx, const struct ki_config *cfg,
	     const struct ki_factors *f, struct ki_decision *out);

/*
 * 结算（点火执行后必调）：responded=false（被忽略）→ 冷静期 ×2 封顶；
 * responded=true → 冷静期复位。返回下一次冷静期（分钟）。
 */
uint16_t ki_settle(bool responded);

/* 当前生效冷静期（分钟） */
uint16_t ki_cooldown_current(void);

#ifdef __cplusplus
}
#endif
