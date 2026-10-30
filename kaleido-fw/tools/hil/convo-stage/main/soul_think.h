/*
 * soul_think.h — W3 思维轮编排（点火=思维触发闸的执行侧）
 *
 * 职责：ki_think_gate 的时机字段供给（时钟/预算/新意态）＋ think 任务
 * （深思 prompt → LLM → ki_intent_parse → 出口门 → TTS 直说 → 结算
 * 六态）。任务栈用内部 RAM（TLS 深度；PSRAM 栈禁碰 flash 纪律）。
 * 单飞：同一时刻至多一个 think 任务。
 */
#ifndef SOUL_THINK_H
#define SOUL_THINK_H

#include <stdbool.h>
#include <stdint.h>

struct ks_tts_cfg;
struct ks_llm_cfg;
struct ki_think_ctx;

/* 主固件启动时接线（app_main 上下文；指针须静态生存期） */
void soul_think_init(struct ks_tts_cfg *tts, struct ks_llm_cfg *llm);

/* soul tick 每拍调用：补全 tc 的时机字段（now_min 压缩态 ×6、
 * last_think_min、thought_today、last_silent 态、new_event 供给） */
void soul_think_gate_ctx(struct ki_think_ctx *tc);

/* 触发后由 soul tick 调用（reason_mask/forced 入队；忙则丢弃并返回
 * false——思维不排队，下拍闸会再判） */
bool soul_think_start(uint8_t reason_mask, bool forced, float score);

/* 'd' 压缩态同步（思维时钟 ×6；soul_fast_drain 调用） */
void soul_think_set_compress(bool on);

/* 事件侧记（soul 钩子转发） */
void soul_think_on_event(void); /* 任何交互事件 → 新意门/结算窗证据 */
void soul_think_note_voice(void);       /* 结算窗人声（不限频） */
void soul_think_note_barge(void);       /* speak 期打断 */
bool soul_think_busy(void);
struct ki_intent;
/* HIL 假意图直注（'h' 键）：跳深思 LLM，出口/执行/结算同一套 */
bool soul_think_inject(const struct ki_intent *intent);
/* HIL 坏 JSON 直注（'H'）：跳深思 LLM，parse 拒绝原路径（判据②板端） */
bool soul_think_inject_json(const char *js);

#endif /* SOUL_THINK_H */
