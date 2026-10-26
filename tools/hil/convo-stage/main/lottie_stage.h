/*
 * lottie_stage.h — 角色舞台对外最小接口
 */
#ifndef LOTTIE_STAGE_H
#define LOTTIE_STAGE_H

int lottie_stage_start(void);

/* 冻结/恢复渲染线程（保留最后画面）。V-45 第二阻断隔离用：
 * wakenet 加载期间冻结 thorvg 渲染，排除 PSRAM 并发变量 */
void lottie_stage_freeze(bool frozen);

#endif
