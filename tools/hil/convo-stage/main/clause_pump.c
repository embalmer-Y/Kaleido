/*
 * clause_pump.c — 子句泵实现（V-43 抽模块，逻辑自 voice_loop_main 平移）
 *
 * 历史教训（V-41 + 评审 2026-10-26）：回调里嵌套 TTS 的 TLS 会在
 * LLM 网络事件栈上再压一套 TLS（实测跳进 JSON 数据）；按轮创建
 * feeder 则在早退路径漏哨兵成幽灵消费者。故：回调只入队，feeder
 * 常驻，哨兵只由 pump_stop 下发。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "clause_pump.h"
#include "mood.h"
#include "soul.h"

static const char *TAG = "pump";

static QueueHandle_t s_q;
static SemaphoreHandle_t s_done;
static volatile bool s_tts_open;
static volatile int64_t s_first_clause_us;

static void feeder_task(void *arg)
{
	char item[CLAUSE_ITEM_BYTES];

	(void)arg;
	for (;;) {
		if (xQueueReceive(s_q, item, portMAX_DELAY) != pdTRUE)
			continue;
		if (!item[0]) {		/* 全零项=本轮收口哨兵 */
			if (s_tts_open) {
				ks_tts_feed(NULL, true, 15000);
				s_tts_open = false;
			}
			xSemaphoreGive(s_done);
			continue;	/* 不退出：常驻等下一轮 */
		}
		if (!s_tts_open) {
			ESP_LOGE(TAG, "tts not open, drop clause");
			continue;
		}
		if (ks_tts_feed(item, false, 15000) != 0)
			ESP_LOGW(TAG, "clause feed fail: %.24s", item);
	}
}

int pump_init(void)
{
	if (s_q)
		return 0;
	s_q = xQueueCreate(8, CLAUSE_ITEM_BYTES);
	s_done = xSemaphoreCreateBinary();
	if (!s_q || !s_done) {
		ESP_LOGE(TAG, "pump create fail");
		return -1;
	}
	if (xTaskCreatePinnedToCore(feeder_task, "ttsfeed", 10240, NULL,
				    5, NULL, 0) != pdPASS) {
		ESP_LOGE(TAG, "feeder create fail");
		return -1;
	}
	return 0;
}

void pump_reset(void)
{
	if (!s_q)
		return;
	xQueueReset(s_q);
	xSemaphoreTake(s_done, 0);
	s_first_clause_us = 0;
}

int pump_tts_attach(const struct ks_tts_cfg *cfg, ks_tts_pcm_cb on_pcm)
{
	mood_round_reset();	/* W2：每轮只取首个标签 */
	struct ks_tts_events ev = { .on_pcm = on_pcm };

	/* MiniMax 的 open 只是解析 JWT（µs 级）；DashScope 构建下此处
	 * 同步付出 ~2.8s 冷建连（原 prewarm 任务曾把它藏进采音窗，
	 * 抽模块后从简——DashScope 回归时再议） */
	if (ks_tts_open(cfg, &ev) != 0) {
		ESP_LOGE(TAG, "tts attach fail");
		return -1;
	}
	s_tts_open = true;
	return 0;
}

bool pump_stop(uint32_t timeout_ms)
{
	static const char eos[CLAUSE_ITEM_BYTES];	/* 全零=哨兵 */
	int64_t deadline;

	if (!s_q)
		return true;
	deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
	while (xQueueSend(s_q, eos, pdMS_TO_TICKS(200)) != pdTRUE) {
		if (esp_timer_get_time() >= deadline) {
			ESP_LOGE(TAG, "pump sentinel lost (feeder stuck)");
			return false;
		}
	}
	{
		int64_t left_us = deadline - esp_timer_get_time();
		/* 上限：最坏 8 子句 × 每句 15s 超时的排水余量 */
		return xSemaphoreTake(
			s_done, pdMS_TO_TICKS((uint32_t)(left_us / 1000)));
	}
}

void pump_tts_detach(void)
{
	if (mood_round_end())
		ESP_LOGW(TAG, "mood tail dropped (unclosed)");
	ks_tts_close();
	s_tts_open = false;
}

bool pump_on_llm_sentence(void *user, const char *sentence, bool final)
{
	(void)user;	/* TTS 由常驻 feeder 负责，回调只管入队 */
	(void)final;	/* 流结束哨兵由 pump_stop 统一下发 */

	char work[CLAUSE_ITEM_BYTES + 32];	/* +mood 尾巴余量 */
	size_t n;

	if (!s_q)
		return true;
	if (!s_first_clause_us && sentence[0])
		s_first_clause_us = esp_timer_get_time();
	if (!sentence[0])
		return true;	/* 空子句防御 */

	/* W2：mood 剥离（跨子句缓冲在 mood 模块内） */
	strlcpy(work, sentence, sizeof(work));
	if (!mood_process(work, sizeof(work), strlen(work), soul_on_mood))
		return true;	/* 整句只是标签：不喂 TTS */
	if (xQueueSend(s_q, work, pdMS_TO_TICKS(200)) != pdTRUE) {
		ESP_LOGW(TAG, "clause q full, drop");
		return false;	/* 消费不动：让 LLM 流尽早停 */
	}
	return true;
}

int64_t pump_first_clause_us(void)
{
	return s_first_clause_us;
}
