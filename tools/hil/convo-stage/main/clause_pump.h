/*
 * clause_pump.h — 子句泵：LLM 句读回调 → 队列 → 常驻 feeder → TTS
 *
 * V-43 从 voice_loop_main 抽出成模块（会话状态机与菜单轮共用）。
 * 泵为常驻服务：boot 时 pump_init() 建一次，轮内 reset/stop 控制生命
 * 周期。V-41 解嵌套与幽灵消费者治理的历史见 voice_loop_main 头注。
 *
 * 轮生命周期：pump_reset() → pump_tts_attach() →（LLM 流式，回调
 * pump_on_llm_sentence）→ pump_stop() → pump_tts_detach()。
 */
#ifndef VOICE_LOOP_P4_CLAUSE_PUMP_H
#define VOICE_LOOP_P4_CLAUSE_PUMP_H

#include <stdint.h>

#include "kaleido_speech.h"

/* 队列项宽度：与 sp_llm 内部攒句缓冲同宽，两者须同步改 */
#define CLAUSE_ITEM_BYTES	512

/* boot 期一次：建队列/信号量/常驻 feeder。0 成功 */
int pump_init(void);

/* 轮开始：清上一轮残留 */
void pump_reset(void);

/* 开 TTS 会话（feeder 据此放行子句）。on_pcm 由调用方注入
 * （播放通路归属调用方）。0 成功 */
int pump_tts_attach(const struct ks_tts_cfg *cfg, ks_tts_pcm_cb on_pcm);

/* 轮收口：哨兵入队（队满重试至限期）+ 等 feeder 排空。
 * 返回 true 泵已干净收口（幂等，重复调用代价可忽略） */
bool pump_stop(uint32_t timeout_ms);

/* 关 TTS 会话（detach 前应先 pump_stop） */
void pump_tts_detach(void);

/* ks_llm_chat 的句读回调（final 语义由 stop 统一收口） */
bool pump_on_llm_sentence(void *user, const char *sentence, bool final);

/* 本轮首个子句到达时刻（µs；未有为 0）——遥测用 */
int64_t pump_first_clause_us(void);
/* 首子句文本（W4 记忆 facts[0]；pump_reset 后为空串） */
void pump_first_clause(char *out, size_t n);

#endif /* VOICE_LOOP_P4_CLAUSE_PUMP_H */
