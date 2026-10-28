/*
 * kaleido_drive.h — 三驱力内稳态层（social/stim/rest）
 *
 * 模型 = Galatea MW0 世界侧的时间化翻译（28 号 §2.1/§9.1）：
 *   水平只被两类项改变——每拍常数代谢（"活着 = look −0.005"的时间化）
 *   与事件脉冲恢复（"eat +0.15 / rest +0.10"的交互化）。
 *   无被动回归 setpoint、无泄漏项——与源一致（源 energy 只在动作结算
 *   时变化，mw0.py:199-217）。
 * 误差 e_d = 1 − level；单侧 ≥0 由 [0,1] 值域构造保证，公式不加钳位
 *   （源 dynamics.py:93-111 同构）。
 * 聚合输出 error_sum/error_max——sum 非 mean（28 号 D1：源 E=Σw·e 无
 *   归一，等权 Q-L09-08；喂 ke valence/arousal/urgency）。
 * 拍语义：1Hz，与 ke_step 同拍（soul limb 内先 kd_step 后 ke_step）。
 * 纪律：零 RNG 零墙钟零分配；纯函数 + 文件级 static 状态；单写者
 *   （W0-3 soul tick 任务持有）。
 * 免扰动原则（源不变量 2 的产品化）：任何云侧/LLM 数值不得绕过 kd
 *   直写 ke 的 drive_error——驱力输入只进 kd（kd→ke）。
 */
#ifndef KALEIDO_DRIVE_H
#define KALEIDO_DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum kd_drive {
	KD_SOCIAL = 0,   /* 社交联结：互动恢复 / 无人时代谢消耗        */
	KD_STIM,         /* 刺激：新事件恢复 / 单调环境代谢            */
	KD_REST,         /* 休憩：静息·充电恢复 / 表达活跃代谢         */
	KD_N
};

/* 事件脉冲（幅度锚 m0 action_energy_delta，28 号 §8.3/§9.2） */
enum kd_event {
	KD_EVT_NONE = 0,
	KD_EVT_ROUND_OK,     /* 一轮对话圆满（rc=0）→ social 大恢复  */
	KD_EVT_ROUND_FAIL,   /* 轮失败 → 小恢复（互动发生但体验差）  */
	KD_EVT_BARGE,        /* 打断：强社交在场信号                 */
	KD_EVT_VOICE,        /* 人声活动拍：小恢复（须限频 ≥5s 防刷）*/
	KD_EVT_TOUCH,        /* 触摸：小恢复                         */
	KD_EVT_CNT
};

/* 告警级（源 severity_of 同构，drives.py:28-40；边界：≥HIGH/CRIT，
 * > band 才 LOW） */
enum kd_sev {
	KD_SEV_NONE = 0,  /* e ≤ band(0.1)          */
	KD_SEV_LOW,       /* e > 0.1                */
	KD_SEV_HIGH,      /* e ≥ 0.5                */
	KD_SEV_CRITICAL,  /* e ≥ 0.8                */
};

struct kd_params {           /* 冻结表（内置 static const；可 kd_init 覆写） */
	float metabolic[KD_N];   /* 每拍水平消耗 ≥0（1Hz 拍）             */
	float pulse[KD_EVT_CNT]; /* 按 kd_event 的 social 恢复幅度        */
	float stim_event;        /* ROUND_OK/BARGE 对 stim 的恢复         */
	float rest_recover;      /* quiet/charging 拍 rest 恢复幅度       */
	float band;              /* 0.1（恢复正常带，锚 Q-L05-03③）      */
};

struct kd_inputs {
	uint64_t tick;           /* 预留（当前未消费，调用方单调供给）    */
	bool active;             /* 预留：当前恒代谢口径（plan §5R.1）下
				  * 不消费——有意偏离 28 号 §9.2 表，W3/W6
				  * 接线人勿按"表达中才代谢"误读（评审 C2）*/
	bool quiet;              /* 本拍静息（idle 且无播放）→ rest 恢复  */
	bool charging;           /* 充电中 → rest 恢复（与 quiet 同路
				  * 同份，不叠加——28 号 §9.2 表口径）    */
	enum kd_event event;     /* 本拍交互事件（至多一个；多事件分拍喂）*/
};

struct kd_output {
	float level[KD_N];       /* [0,1] 水平                            */
	float error[KD_N];       /* e_d = 1 − level                       */
	float error_sum;         /* E = Σ e_d → ke valence（sum 非 mean） */
	float error_max;         /* max e_d → ke arousal/urgency          */
	enum kd_sev sev[KD_N];   /* 逐驱力告警级（→W3 VIABILITY 因子）    */
	float trend[KD_N];       /* e_t − e_{t−1}（正=恶化，随告警上遥测）*/
};

void kd_init(const struct kd_params *p);   /* NULL = 用内置冻结表；
					    * p 须静态生存期（组件只存
					    * 指针，评审 C4） */
void kd_reset(void);                      /* level 全 1.0（初始零误差） */
void kd_step(uint64_t tick, const struct kd_inputs *in,
	     struct kd_output *out);

#ifdef __cplusplus
}
#endif
#endif /* KALEIDO_DRIVE_H */
