/*
 * kaleido_speech.h — 语音链路 provider 抽象（ASR 上行 / TTS 下行）
 *
 * 设计依据：docs/design/灵魂架构方案v1.md §3（组合 C：云直连 + 自托管网关预留）
 * 协议事实：docs/gates/V-33-百炼语音链路实测.md §6
 *  - ASR：WS 双向流式（run/continue/finish-task + 二进制 PCM 上行）
 *  - TTS：WS 双向流式（文本分句 continue-task，二进制 PCM 下行直推 I2S）
 *
 * 线程模型：回调在 provider 内部 WS 任务上下文执行——回调内禁止阻塞/重入
 * 本 API；PCM 消费方（如 I2S 写）须自带缓冲或投递。
 */
#ifndef KALEIDO_SPEECH_H
#define KALEIDO_SPEECH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ASR ---- */

struct ks_asr_cfg {
	const char *url;      /* wss://host/api-ws/v1/inference   */
	const char *api_key;  /* Bearer；运行时传入（NVS/BYOK）   */
	const char *model;    /* paraformer-realtime-v2           */
	int sample_rate;      /* 固定 16000（16bit 单声道 PCM）    */
};

/*
 * sentence 回调：final=false 为 partial 覆盖（同 sentence_id 增量），
 * final=true 为该句定稿（含标点）。
 */
typedef void (*ks_asr_text_cb)(void *user, const char *sentence,
			       bool final);

struct ks_asr_events {
	void *user;
	ks_asr_text_cb on_text;
};

int ks_asr_open(const struct ks_asr_cfg *cfg,
		const struct ks_asr_events *ev);
/*
 * 喂 16kHz/16bit/mono PCM。内部按 100ms（3200B）分片上行；
 * 返回已消费样本数（<0 错误：-2 未打开，-3 发送失败）。
 */
int ks_asr_feed(const int16_t *pcm, size_t samples);
/* finish-task：flush 尾句；阻塞至 task-finished 或超时（ms） */
int ks_asr_finish(uint32_t timeout_ms);
void ks_asr_close(void);

/* ---- TTS ---- */

struct ks_tts_cfg {
	const char *url;        /* 同 ASR 域名                        */
	const char *api_key;
	const char *model;      /* cosyvoice-v3-flash                */
	const char *voice;      /* 预置音色或复刻 voice_id           */
	int sample_rate;        /* 16000（pcm s16le 直推 I2S）       */
};

/* 合成 PCM 下行（s16le @cfg->sample_rate，单声道） */
typedef void (*ks_tts_pcm_cb)(void *user, const uint8_t *pcm,
			      size_t len);

struct ks_tts_events {
	void *user;
	ks_tts_pcm_cb on_pcm;
};

int ks_tts_open(const struct ks_tts_cfg *cfg,
		const struct ks_tts_events *ev);
/*
 * 流式喂文本（continue-task）。last=true 时补 finish-task；
 * 阻塞至 task-finished 或超时。调用方负责断句节奏
 * （服务端自动分句，句读即出音频——V-33 §2）。
 */
int ks_tts_feed(const char *text, bool last, uint32_t timeout_ms);
/* 首 PCM 包到达时刻（feed 后 ms；未到达为 -1）——延迟统计用 */
int64_t ks_tts_first_pcm_us(void);
void ks_tts_close(void);

#ifdef __cplusplus
}
#endif
#endif /* KALEIDO_SPEECH_H */
