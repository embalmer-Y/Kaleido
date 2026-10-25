/*
 * voice_loop_main.c — M1.5 语音全链 HIL：采集→ASR→LLM 流式→TTS→播放
 *
 * 目的：方案 v1.5 §6 M1.5 验收——P50<1.5s / P90<2.2s（采集结束→首音出声）。
 * 链路：串口 'g' 触发 → 6s 采音（喂入即流式送 ASR）→ 定稿文本 →
 *       compatible-mode SSE（句读回调）→ TTS（预开连接）→ I2S 播放。
 * 所有时间戳 esp_timer；输出 "VL,RESULT,…" 单行供统计。
 */
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "kaleido_speech.h"
#include "llm_stream.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define CAP_SECONDS	6
#define CAP_CHUNK	1600 /* 100ms @16k              */
#define ROUND_MAX	5

static const char *TAG = "vl";

extern const char test_prompt_start[] asm("_binary_test_prompt_pcm_start");
extern const char test_prompt_end[] asm("_binary_test_prompt_pcm_end");

static char s_asr_text[512];
static volatile int64_t t_capture_end, t_asr_final;
static volatile int64_t t_llm_first, t_tts_first_pcm, t_first_audio;
static volatile int s_round;

/* ---- 播放通路：WS 回调(WS 任务) → StreamBuffer → 播放任务 → I2S ---- */

static StreamBufferHandle_t s_pb;
#define PB_BYTES	32768

static void play_task(void *arg)
{
	static int16_t mono[1600]; /* 100ms 单块 */

	for (;;) {
		size_t n = xStreamBufferReceive(s_pb, mono,
						sizeof(mono),
						pdMS_TO_TICKS(10000));

		if (n == 0)
			continue;
		if (!t_first_audio)
			t_first_audio = esp_timer_get_time();
		aio_play(mono, n / 2, 1000);
	}
}

static void on_tts_pcm(void *user, const uint8_t *pcm, size_t len)
{
	if (!t_tts_first_pcm)
		t_tts_first_pcm = esp_timer_get_time();
	size_t w = xStreamBufferSend(s_pb, pcm, len, 0);

	if (w != len)
		ESP_LOGW(TAG, "pb drop %u/%u", (unsigned)(len - w),
			 (unsigned)len);
}

/* ---- ASR 回调：拼定稿句 ---- */

static void on_asr_text(void *user, const char *sentence, bool final)
{
	if (!final || !sentence[0])
		return;
	strlcat(s_asr_text, sentence, sizeof(s_asr_text));
}

/* ---- LLM 句读回调：首句即送 TTS ---- */

static bool s_tts_open;

static bool on_llm_sentence(void *user, const char *sentence,
			    bool final)
{
	struct ks_tts_cfg *cfg = user;

	if (final && !sentence[0]) {
		if (s_tts_open) {
			ks_tts_feed(NULL, true, 15000);
			s_tts_open = false;
		}
		return true;
	}
	if (!s_tts_open) {
		if (ks_tts_open(cfg,
				&(struct ks_tts_events){
					.on_pcm = on_tts_pcm }) != 0) {
			ESP_LOGE(TAG, "tts open fail");
			return false;
		}
		s_tts_open = true;
	}
	int rc = ks_tts_feed(sentence, false, 15000);

	if (rc)
		return false;
	if (final) {
		ks_tts_feed(NULL, true, 15000);
		s_tts_open = false;
	}
	return true;
}

/* ---- WiFi（M1 已验证路径，最简 station） ---- */

static void wifi_start(void)
{
	esp_netif_init();
	esp_event_loop_create_default();
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t w = WIFI_INIT_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_wifi_init(&w));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	wifi_config_t wc = { 0 };

	strlcpy((char *)wc.sta.ssid, CONFIG_VL_WIFI_SSID,
		sizeof(wc.sta.ssid));
	strlcpy((char *)wc.sta.password, CONFIG_VL_WIFI_PASS,
		sizeof(wc.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_connect());

	/* 简化：轮询 IP（HIL 语境，不建事件组） */
	for (int i = 0; i < 100; i++) {
		esp_netif_ip_info_t ipi;

		if (esp_netif_get_ip_info(
			    esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ipi) == ESP_OK &&
		    ipi.ip.addr) {
			ESP_LOGI(TAG, "wifi up");
			return;
		}
		vTaskDelay(pdMS_TO_TICKS(200));
		esp_wifi_connect();
	}
	ESP_LOGE(TAG, "wifi fail");
}

static int one_round(struct ks_asr_cfg *asr, struct ks_tts_cfg *tts,
		     struct llm_cfg *llm, bool embedded)
{
	static int16_t chunk[CAP_CHUNK];
	int64_t t_trigger = esp_timer_get_time();

	s_asr_text[0] = '\0';
	s_tts_open = false;                 /* G6：跨轮复位      */
	t_capture_end = t_asr_final = 0;
	t_llm_first = t_tts_first_pcm = t_first_audio = 0;

	printf("VL,ROUND,%d,speak now %ds\r\n", s_round, CAP_SECONDS);
	fflush(stdout);

	if (ks_asr_open(asr, &(struct ks_asr_events){
			.on_text = on_asr_text }) != 0) {
		ESP_LOGE(TAG, "asr open fail");
		return -1;
	}
	/* 边采边喂（产品真实形态：流式）；embedded 模式以 1x 节奏
	 * 喂内置测试 PCM（无人值守回归） */
	int64_t cap_start = esp_timer_get_time();
	size_t fed = 0;

	if (embedded) {
		const int16_t *p = (const void *)test_prompt_start;
		size_t total = (test_prompt_end - test_prompt_start) / 2;

		while (fed < total) {
			size_t n = CAP_CHUNK;

			if (n > total - fed)
				n = total - fed;
			if (ks_asr_feed(p + fed, n) < 0)
				break;
			fed += n;
			vTaskDelay(pdMS_TO_TICKS(100));
		}
	} else {
		while (esp_timer_get_time() - cap_start <
		       CAP_SECONDS * 1000000LL) {
			int n = aio_capture(chunk, CAP_CHUNK, 500);

			if (n <= 0)
				break;
			if (ks_asr_feed(chunk, n) < 0)
				break;
			fed += n;
		}
	}
	t_capture_end = esp_timer_get_time();
	if (ks_asr_finish(15000) != 0) {
		ESP_LOGE(TAG, "asr finish fail");
		ks_asr_close();
		return -2;
	}
	t_asr_final = esp_timer_get_time();
	ks_asr_close();
	printf("VL,ASR,%s\r\n", s_asr_text[0] ? s_asr_text : "(empty)");
	fflush(stdout);
	if (!s_asr_text[0])
		return -3;

	llm->user_msg = s_asr_text;
	int64_t ftok = 0;

	int rc = llm_stream_chat(llm, on_llm_sentence, tts, &ftok);
	t_llm_first = ftok;
	ks_tts_close(); /* 消费者异常中断时兜底 */
	if (rc != 0) {
		ESP_LOGE(TAG, "llm fail %d", rc);
		return -4;
	}
	/* 等播放队列清空（尾部余量） */
	vTaskDelay(pdMS_TO_TICKS(500));
	int64_t now = esp_timer_get_time();

	printf("VL,RESULT,round=%d,fed_ms=%u,asr_final=%lld,"
	       "llm_first=%lld,tts_first_pcm=%lld,first_audio=%lld,"
	       "total_trigger_to_audio=%lld,total_capture_to_audio=%lld\r\n",
	       s_round,
	       (unsigned)(fed * 100 / 16000),
	       (t_asr_final - t_capture_end) / 1000,
	       t_llm_first / 1000,
	       t_tts_first_pcm ? (t_tts_first_pcm -
				  t_capture_end) / 1000 : -1,
	       t_first_audio ? (t_first_audio - t_capture_end) / 1000 : -1,
	       t_first_audio ? (t_first_audio - t_trigger) / 1000 : -1,
	       t_first_audio ? (t_first_audio -
				t_capture_end) / 1000 : -1);
	fflush(stdout);
	(void)now;
	return 0;
}

void app_main(void)
{
	struct ks_asr_cfg asr = {
		.url = "wss://dashscope.aliyuncs.com",
		.api_key = CONFIG_VL_DS_KEY,
		.model = "paraformer-realtime-v2",
		.sample_rate = 16000,
	};
	static struct ks_tts_cfg tts = {
		.url = "wss://dashscope.aliyuncs.com",
		.api_key = CONFIG_VL_DS_KEY,
		.model = "cosyvoice-v3-flash",
		.voice = "longanyang",
		.sample_rate = 16000,
	};
	struct llm_cfg llm = {
		.url = "https://dashscope.aliyuncs.com/"
		       "compatible-mode/v1/chat/completions",
		.api_key = CONFIG_VL_LLM_KEY,
		.model = "qwen-plus",
		.system = "你是桌搭小生物墨墨，口语化短句回答，每句不超过"
			  "30个字。",
		.user_msg = "",
	};

	esp_log_level_set("*", ESP_LOG_WARN);
	nvs_flash_init();
	wifi_start();
	if (aio_init() != 0) {
		ESP_LOGE(TAG, "aio fail");
		return;
	}
	s_pb = xStreamBufferCreate(PB_BYTES, 1);
	xTaskCreatePinnedToCore(play_task, "pb", 4096, NULL, 5, NULL, 0);

	printf("VL,READY,press 'g'+Enter to run a round\r\n");
	fflush(stdout);
	while (s_round < ROUND_MAX) {
		int ch = getchar();

		if (ch != 'g' && ch != 'G' && ch != 't' && ch != 'T')
			continue;
		bool embedded = (ch == 't' || ch == 'T');

		s_round++;
		one_round(&asr, &tts, &llm, embedded);
		printf("VL,READY,press 'g'+Enter (%d/%d used)\r\n",
		       s_round, ROUND_MAX);
		fflush(stdout);
	}
	printf("VL,DONE\r\n");
	fflush(stdout);
}
