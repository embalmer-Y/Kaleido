/*
 * llm_stream.h — OpenAI 兼容 chat/completions 流式（SSE）客户端
 *
 * V-33 §1.6：compatible-mode stream=true 逐 delta 输出；
 * 句读回调（。？！；\n）供"首句即送 TTS"。
 */
#ifndef VOICE_LOOP_LLM_STREAM_H
#define VOICE_LOOP_LLM_STREAM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* 返回 true 继续流；false 中止 */
typedef bool (*llm_sent_cb)(void *user, const char *sentence,
			    bool final);

struct llm_cfg {
	const char *url;     /* https://…/compatible-mode/v1/chat/completions */
	const char *api_key;
	const char *model;   /* qwen-plus                            */
	const char *system;  /* 可为 NULL                            */
	const char *user_msg;
	bool cut_clauses;   /* true=子句级（，、）切送 TTS，首音更快 */
	bool no_think;     /* V-41 MiniMax-M3：body 附 thinking disabled */
};

/* 阻塞执行一次流式对话；输出 first_token_us（首 delta 时刻，可 NULL） */
int llm_stream_chat(const struct llm_cfg *cfg, llm_sent_cb cb,
		    void *user, int64_t *first_token_us);

#endif /* VOICE_LOOP_LLM_STREAM_H */
