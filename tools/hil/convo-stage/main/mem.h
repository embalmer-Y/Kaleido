/* mem.h — W4 记忆接线：storage 挂载 / 每轮情节写回 / 检索注入块
 *
 * 板级纪律适配（board discipline，缺陷 I/H 时代两度立规）：
 *  - km 写路径（littlefs append/overlay 落盘）只在 mem_recorder 任务
 *    上执行——静态内部栈（CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y
 *    下动态栈可能落 PSRAM，flash 写的 cache 关断窗口会断言）；
 *  - 检索 km_query 是纯读（cache 映射读，无 cache 关断），允许在
 *    轮任务/main 任务就地调；
 *  - mem_boot/mem_net_up 在 main 任务（内部栈）执行。
 */
#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>

#include "kaleido_memory.h"

/* 挂载 /storage（littlefs）+ km_init + recorder 任务。返回 0=ok，
 * <0=挂载失败（记忆功能整体降级为不可用，主流程不阻断） */
int mem_boot(void);

/* WiFi 就绪后调：SNTP 对时（时间相对化/时间戳的真实时钟源） */
void mem_net_up(void);

/* 每轮情节写回（轮任务可调——只入队，写盘由 recorder 做）：
 * asr=ASR 定稿文本（rc!=0 整轮不写，30 号 §8.2 写入门槛）；
 * origin=KM_ORIGIN_USER（对话轮）/SELF（点火 SPEAK，W3 接） */
void mem_round_record(const char *asr_text, int rc, int origin);

/* 检索注入块（30 号 §8.3 格式；静态缓冲，当轮有效）。
 * 返回空串=无库可注入（prompt 不加段） */
const char *mem_prompt_block(void);

/* W4→W3 记忆钩子（soul_mem_hook 注册用）：top1 分数 30s 节流 */
float mem_hook_probe(void);

/* 健康巡检（'u'）：水位/seq/覆盖表脏数，KF 单行落痕 */
void mem_stats(void);

/* V-48e 板端证据注入：写一条 canned 情节（'K' 键），返回 seq */
int64_t mem_test_inject(void);

#endif /* MEM_H */
