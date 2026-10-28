/*
 * emote_stage.h — 表情舞台对外接口（V-45R：猫伴表情包）
 */
#ifndef EMOTE_STAGE_H
#define EMOTE_STAGE_H

#include "kaleido_express.h"

int emote_stage_start(void);

/* KEX 状态 → 表情片段（异步：内部入队，LVGL 回调上下文安全调用） */
void emote_stage_set_state(enum kex_state st);

/* 情感片段（W1：soul 表达策略驱动，IDLE 底色；活动四态仍走
 * set_state 直配——策略侧保证驻留与迟回，本接口只管切） */
void emote_stage_set_emo(const char *clip);

/* 'e' 键：循环播放下一个片段（人工目验用） */
void emote_stage_cycle(void);

/* 暂停/恢复动画渲染。返回：暂停是否已确认（ack≤1s；false=渲染可能
 * 仍在跑，调用方须记日志并自决继续/中止——不得静默）。
 * 用途：wakenet 加载隔离（V-45 死锁教训）——先暂停 gfx 渲染再持 LVGL 锁 */
bool emote_stage_pause(bool paused);

#endif
