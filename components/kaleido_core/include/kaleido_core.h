/*
 * kaleido_core.h — KF 应用框架（V-45R 整改：crash 取证 + 调试模式 + 流量审计）
 *
 * 设计目标（负责人指示：crash 定位成本必须降下来）：
 *   1) 事件环形日志：所有关键 flow 过一遍 kf_ev()，常驻内存环；
 *      崩溃/看门狗复位后开机自动回放最近事件——"死前最后一屏"不再靠猜。
 *   2) 调试模式：串口 'v' 一键切换。关闭态=发布口径（WARN 级、无杂音）；
 *      打开态=DEBUG 级 + KF 控制台流 + 诊断仪表（FB 采样等）。
 *   3) 任务心跳：常驻任务 kf_beat() 打点，'u' 转储各任务心跳年龄——
 *      卡死类问题先看"谁没跳"。
 *   4) coredump 落 flash（分区 coredump@0x910000）：
 *      esp-coredump 离线解码（见 tools/hil/convo-stage/README）。
 *
 * 约束：环与心跳驻内部 RAM（panic 时 PSRAM 访问可能正是故障源）；
 * kf_ev 任何上下文可用（临界区极短，vsnprintf 在临界区外）。
 */
#ifndef KALEIDO_CORE_H
#define KALEIDO_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* 框架初始化（app_main 最先调用；幂等） */
void kf_init(void);

/* 调试模式开关（串口 'v'）；打开时全局日志级别→DEBUG 且 KF 流上屏 */
bool kf_dbg_get(void);
bool kf_dbg_toggle(void);

/* 事件入环（mod≤9 字符；printf 风格 fmt）。调试模式下同时上屏 */
void kf_ev(const char *mod, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* 常驻任务心跳打点（每主循环一次即可） */
void kf_beat(const char *mod);

/* 'u' 健康转储：heap 概览 + 心跳年龄表 + 事件环尾 16 条 */
void kf_health_dump(void);

/* 开机取证：若上次为 panic/WDT 复位，回放环内最近事件。
 * 在 kf_init 内自动调用，无需手动 */
void kf_boot_forensics(void);

#endif /* KALEIDO_CORE_H */
