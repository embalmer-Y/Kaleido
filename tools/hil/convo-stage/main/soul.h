/*
 * soul.h — 灵魂 limb（W0-3，plan §5R.1）
 *
 * 职责：九维感受态的唯一写者——1Hz tick 任务内先 kd_step 后 ke_step
 * （拍语义见组件头文件），并对外提供：
 *  - 快照拷贝服务（消费者拿副本不拿指针；ke/ki 状态为文件级 static，
 *    并发契约靠"单写者 + 锁内拷贝"成立）
 *  - 事件钩子（会话/声音任务调用，非阻塞；同拍多事件合并且
 *    BARGE > ROUND_* > VOICE/TOUCH 优先保强信号）
 *
 * 消费方现状（W0 仅遥测）：SOUL 遥测行 1Hz（debug 态）/15s（常态）；
 * W1 表达映射、W2 prompt 组装、W3 ki_think_gate 逐级接入。
 */
#ifndef SOUL_H
#define SOUL_H

#include <stdbool.h>
#include <stdint.h>

#include "kaleido_drive.h"
#include "kaleido_emotion.h"
#include "kaleido_ignition.h"

struct soul_snap {
	uint64_t tick;
	struct kd_output drive;
	struct ke_output emo;
	struct ki_decision ign;
};

/* 启动 1Hz tick 任务（幂等；false=资源失败）。KD/KE 复位到零误差初态 */
int soul_start(void);

/* 快照拷贝（任意任务；未启动时填零） */
void soul_snapshot(struct soul_snap *out);

/* ---- 事件钩子（任意任务，非阻塞；W1 接会话 FSM/wakenet） ---- */
void soul_on_round(bool ok);   /* 一轮结算（rc 落地拍；回执+驱力脉冲） */
void soul_on_barge(void);      /* 打断成功（强在场信号）               */
void soul_on_voice(void);      /* 人声活动拍（内部 ≥5s 限频防刷）      */
/* HIL 时间压缩档开关（'d' 键；仅 debug 会话使用，产品态不开） */
void soul_fast_drain(bool on);
uint32_t soul_tick_count(void);

#endif /* SOUL_H */
