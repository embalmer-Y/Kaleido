/*
 * stage_express.h — 表达桥：会话/OOBE 状态 → kaleido_express → 舞台视觉
 */
#ifndef VOICE_LOOP_P4_STAGE_EXPRESS_H
#define VOICE_LOOP_P4_STAGE_EXPRESS_H

#include "lvgl.h"

/* 建 UI（状态环）+ 起 100ms 轮询定时器。须在 LVGL 锁内调用 */
int stage_express_init(void);

#endif
