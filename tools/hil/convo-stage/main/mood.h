/*
 * mood.h — LLM mood 标签剥离（W2；纯函数零依赖，主机可测）
 *
 * 协议：LLM 回复最末尾至多一个 <mood d="+0.1" t="6"/>（d∈[-0.2,0.2]
 * 一位小数，t 为 4-8 整数；t 仅协议兼容字段，固件不消费——重评价已
 * 改一次性施加语义）。本模块只管"剥干净不入 TTS + 转发首个 delta"，
 * 有界性与回执由 kaleido_emotion 引擎端钳位保证（D-13：LLM 禁直写
 * 情感数值，唯一合法通道=有界 delta）。
 *
 * 跨子句缓冲：标签可能被子句边界（，、。）切开——未闭合的 "<mood"
 * 前缀尾巴扣下并入下一子句；round_end 时仍未闭合则丢弃（绝不入 TTS，
 * 也不泄漏进下一轮）。长度上限 31 字符（评审 M-05）：协议标签
 * "<mood d=\"+0.1\" t=\"8\"/>" 最长 24 字符，超 31 的 "<mood…" 前缀
 * 必非协议串（放行入 TTS 无害），防无限扣留。
 */
#ifndef MOOD_H
#define MOOD_H

#include <stddef.h>

/* 每轮起点复位（pump_tts_attach 调用） */
void mood_round_reset(void);

/* 每轮收尾（pump_tts_detach 调用）：返回未闭合尾巴是否被丢弃 */
bool mood_round_end(void);

/*
 * 处理一个 LLM 子句（work 为调用方缓冲，[0,len) 为子句内容；
 * 原地重排）。on_mood：本会话首个完整标签的 delta 转发点（可为
 * NULL）。返回剥离后可入 TTS 的长度（可能 0）。
 */
size_t mood_process(char *work, size_t cap, size_t len,
		    void (*on_mood)(float delta));

#endif /* MOOD_H */
