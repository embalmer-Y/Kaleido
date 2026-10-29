/*
 * kaleido_speech.h — 语音/对话链路 provider 抽象
 *                    （ASR 上行 / TTS 下行 / LLM 流式对话）
 *
 * 设计依据：docs/design/灵魂架构方案v1.md §3（组合 C：云直连 + 自托管
 * 网关预留）；协议事实见 docs/gates/V-33 与 V-41 记录。
 *  - ASR：WS 双向流式（run/continue/finish-task + 二进制 PCM 上行）
 *  - TTS：MiniMax t2a_v2（HTTPS SSE，按子句无状态请求）或
 *         DashScope cosyvoice（WS continue-task 续喂），Kconfig 二选一
 *  - LLM ：OpenAI 兼容 chat/completions SSE，句读即回调
 *
 * 线程与生命周期契约（违反即未定义行为）：
 *  - 同一会话（asr/tts 各自）任一时刻只允许一个调用者线程；
 *    open/feed/finish/close 必须同线程顺序调用。
 *  - 回调在 provider 内部网络任务上下文执行——回调内禁止阻塞/重入
 *    本 API；PCM 消费方（如 I2S 写）须自带缓冲或投递。
 *  - cfg 结构体由调用方持有：指针在会话全周期存活（static 或堆），
 *    栈上 cfg 在 feed 期不可回收。
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
	const char *url;      /* wss://host（自动拼 /api-ws/v1/inference） */
	const char *api_key;  /* Bearer；运行时传入（NVS/BYOK）           */
	const char *model;    /* paraformer-realtime-v2                    */
	int sample_rate;      /* 固定 16000（16bit 单声道 PCM）             */
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

/* 返回 0 成功；-1 参数/已打开；-2 连接/协议失败 */
int ks_asr_open(const struct ks_asr_cfg *cfg,
		const struct ks_asr_events *ev);
/*
 * 喂 16kHz/16bit/mono PCM。内部按 100ms（3200B）分片上行；
 * 返回已消费样本数（<0 错误：-2 未打开，-3 发送失败）。
 */
int ks_asr_feed(const int16_t *pcm, size_t samples);
/* finish-task：flush 尾句；阻塞至 task-finished 或超时（ms）。
 * 返回 0 成功；-2 未打开；-3 发送失败；-4 超时；-5 task-failed */
int ks_asr_finish(uint32_t timeout_ms);
void ks_asr_close(void);

/* ---- TTS ---- */

struct ks_tts_cfg {
	const char *url;        /* DashScope 域名（MiniMax 忽略）     */
	const char *api_key;    /* MiniMax 为 JWT（内嵌 GroupID）      */
	const char *model;      /* speech-2.5-hd-preview / cosyvoice  */
	const char *voice;      /* 预置音色或复刻 voice_id             */
	int sample_rate;        /* 16000（pcm s16le 直推 I2S）         */
	float speech_rate;      /* 语速 [0.5,2]；0=不传（provider 缺省，
				 * W2 情感调制 tts_hint.rate 消费点）  */
	int pitch;              /* 半音 [-12,12]；0=不传（同上）       */
};

/* 合成 PCM 下行（s16le @cfg->sample_rate，单声道） */
typedef void (*ks_tts_pcm_cb)(void *user, const uint8_t *pcm,
			      size_t len);

/* 错误遥测（V-47：哑音可见化）。stage=KS_TTS_ERR_*，code=本次
 * ks_tts_* 返回码；KS_TTS_ERR_SILENT=哑音签名（HTTP 200 但零 PCM） */
enum ks_tts_err {
	KS_TTS_ERR_OPEN = 1,	/* open 参数/协议失败               */
	KS_TTS_ERR_LOCAL = 2,	/* feed 本地失败                    */
	KS_TTS_ERR_HTTP = 3,	/* feed HTTP 非 200                 */
	KS_TTS_ERR_CUT = 4,	/* 流中断（音频可能不完整）         */
	KS_TTS_ERR_SILENT = 5,	/* 哑音：成功返回但全程零 PCM       */
};
typedef void (*ks_tts_err_cb)(void *user, int stage, int code);

struct ks_tts_events {
	void *user;
	ks_tts_pcm_cb on_pcm;
	ks_tts_err_cb on_error;	/* 可 NULL（无遥测）                */
};

/* 返回 0 成功；-1 参数/缺 key；-2 协议失败（provider 实现各异）；
 * -6 会话忙（provider 互斥锁，V-47：think/round 并发拆台防护） */
int ks_tts_open(const struct ks_tts_cfg *cfg,
		const struct ks_tts_events *ev);
/*
 * 流式喂文本（MiniMax：一次 feed 一次 HTTPS 请求，天然按子句切分；
 * DashScope：continue-task 续喂，last=true 补 finish-task）。
 * 阻塞至整段合成完或超时。空文本非 last 返回 -1（无可合成）。
 * 返回 0 成功；-1 空文本；-2 未打开/本地失败；-3 HTTP 非 200；
 * -4 流中断（音频可能不完整，调用方应作失败处理）
 */
int ks_tts_feed(const char *text, bool last, uint32_t timeout_ms);
/* 首 PCM 包到达时刻（µs，自最近一次 feed 起；未到达为 -1）。
 * 注意：MiniMax 逐 feed 覆盖——统计口径是"本子句"，非全会话 */
int64_t ks_tts_first_pcm_us(void);
void ks_tts_close(void);
/* 连接预热（延迟优化 2026-10-30）：轮起跑前后台调用，把 TLS 握手从
 * 首子句关键路径挪走。MiniMax 实做（最小真实请求暖 keep-alive 连接，
 * 跨轮存活）；DashScope 空实现。忙（-6）/失败均无害——feed 自带
 * 冷路径回退 */
int ks_tts_prewarm(const struct ks_tts_cfg *cfg);

/* ---- LLM（流式对话） ---- */

struct ks_llm_cfg {
	const char *url;	/* chat/completions SSE 端点           */
	const char *api_key;	/* Bearer                               */
	const char *model;	/* MiniMax-M3 / qwen-plus ...           */
	const char *system;	/* 可为 NULL                            */
	const char *user_msg;
	bool cut_clauses;	/* 子句级（，、）切送，首音更快        */
	bool no_think;		/* body 附 thinking disabled（M3）      */
};

/* 返回 true 继续流；false 中止（消费方过载等） */
typedef bool (*ks_llm_sent_cb)(void *user, const char *sentence,
			       bool final);

/*
 * 阻塞执行一次流式对话；句读（。？！；\n，cut_clauses 再加 ，、）
 * 即回调。final=true 为收口：有尾句带尾句，流空则空串。
 * 回调与调用同线程（网络事件在内部读循环里同步分发）。
 * 返回 0 成功；-1 本地错误（参数/连接/写）；-2 HTTP 非 200
 */
int ks_llm_chat(const struct ks_llm_cfg *cfg, ks_llm_sent_cb cb,
		void *user, int64_t *first_token_us);

/* 便捷：由 MiniMax JWT 拼 chatcompletion_v2 URL（内含 GroupID 提取，
 * 免去调用方接触 JWT 细节）；解析失败返回 -1 */
int ks_llm_minimax_url(char *buf, size_t n, const char *jwt);

#ifdef __cplusplus
}
#endif
#endif /* KALEIDO_SPEECH_H */
