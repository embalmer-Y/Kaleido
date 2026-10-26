/*
 * wakenet_test.c — WakeNet 单麦唤醒（M2 前置，esp-sr 2.6.0）
 *
 * 'w'：监听模式——AFE 双任务常驻，唤醒即双短鸣 + 日志
 * 'W'：TTS 回环自验——喇叭每 7s 说一次"你好小智"（wn9_nihaoxiaozhi_tts
 *      模型对 TTS 合成语音鲁棒），统计命中数与检测延迟
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"

#include "audio_io.h"
#include "kaleido_speech.h"

/* voice_loop_main.c 的 TTS PCM → 扬声器缓冲（s_pb）转发 */
extern void on_tts_pcm(void *user, const uint8_t *pcm, size_t len);
void wakenet_start(struct ks_tts_cfg *tts, bool selftest);

static const char *TAG = "wn";

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static volatile int s_wake_cnt;
static volatile int64_t s_speak_us;	/* TTS 开口时刻（延迟基准） */
static int16_t s_beep[4800];		/* 300ms 提示音 */

static void beep_play_task(void *arg)
{
	(void)arg;
	aio_play(s_beep, 4800, 3000);
	vTaskDelay(pdMS_TO_TICKS(80));
	aio_play(s_beep, 4800, 3000);	/* 双短鸣 = 已唤醒 */
	vTaskDelete(NULL);
}

static void feed_task(void *arg)
{
	int chunk = s_afe->get_feed_chunksize(s_afe_data);
	int16_t *buf = malloc(chunk * sizeof(int16_t));

	(void)arg;
	if (!buf) {
		printf("VL,WN,ERROR=feed_alloc\r\n");
		fflush(stdout);
		vTaskDelete(NULL);
		return;
	}
	printf("VL,WN,FEED,chunk=%d\r\n", chunk);
	fflush(stdout);
	for (;;) {
		int n = aio_capture(buf, chunk, 2000);

		if (n < chunk)
			memset(buf + n, 0, (chunk - n) * sizeof(int16_t));
		s_afe->feed(s_afe_data, buf);
	}
}

static void detect_task(void *arg)
{
	(void)arg;
	for (;;) {
		afe_fetch_result_t *r = s_afe->fetch(s_afe_data);

		if (r && r->wakeup_state == WAKENET_DETECTED) {
			int64_t lat = s_speak_us ?
				(esp_timer_get_time() - s_speak_us) / 1000 : -1;

			s_wake_cnt++;
			printf("VL,WN,WAKE,cnt=%d,lat_ms=%lld,vol=%.1f\r\n",
			       s_wake_cnt, lat, r->data_volume);
			fflush(stdout);
			xTaskCreatePinnedToCore(beep_play_task, "beep", 4096,
						NULL, 6, NULL, 0);
		}
	}
}

/* TTS 喊话任务：周期说唤醒词，喂给回环自验 */
static void announce_task(void *arg)
{
	struct ks_tts_cfg *cfg = arg;
	const int cycles = 10;
	int hits0 = s_wake_cnt;

	for (int i = 0; i < cycles; i++) {
		/* dashscope 限制：finish 后同会话续喂新文本报缺 task_group，
		 * 每轮独立会话（自验场景多花 ~0.5s 开销可接受） */
		if (ks_tts_open(cfg, &(struct ks_tts_events){
				.on_pcm = on_tts_pcm,
			}) != 0) {
			printf("VL,WN,ERROR=tts_open,i=%d\r\n", i);
			fflush(stdout);
			vTaskDelay(pdMS_TO_TICKS(3000));
			continue;
		}
		printf("VL,WN,SPEAK,i=%d\r\n", i);
		fflush(stdout);
		s_speak_us = esp_timer_get_time();
		ks_tts_feed("你好小智", false, 15000);
		ks_tts_feed(NULL, true, 15000);
		ks_tts_close();
		vTaskDelay(pdMS_TO_TICKS(7000));
	}
	vTaskDelay(pdMS_TO_TICKS(1500));	/* 等尾音抵达检测 */
	printf("VL,WN,SELFTEST,done,hits=%d/%d\r\n",
	       s_wake_cnt - hits0, cycles);
	fflush(stdout);
	vTaskDelete(NULL);
}

void wakenet_start(struct ks_tts_cfg *tts, bool selftest)
{
	uint32_t i0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	uint32_t p0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	srmodel_list_t *models = esp_srmodel_init("model");

	if (!models || models->num == 0) {
		printf("VL,WN,ERROR=no_model,check CONFIG_SR_WN_*\r\n");
		fflush(stdout);
		return;
	}
	afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR,
					    AFE_MODE_HIGH_PERF);

	/* P4 内部 RAM 紧张，AFE 尽量放 PSRAM；单麦无 AEC（TX 参考后续接） */
	cfg->aec_init = false;
	cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
	cfg = afe_config_check(cfg);

	s_afe = esp_afe_handle_from_config(cfg);
	if (!s_afe) {
		printf("VL,WN,ERROR=no_afe_handle\r\n");
		fflush(stdout);
		return;
	}
	s_afe_data = s_afe->create_from_config(cfg);
	if (!s_afe_data) {
		printf("VL,WN,ERROR=afe_create\r\n");
		fflush(stdout);
		return;
	}
	printf("VL,WN,INIT,ok,model=%s,int=-%uKB,ps=-%uKB\r\n",
	       models->model_name[0] ? models->model_name[0] : "?",
	       (unsigned)((i0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) >> 10),
	       (unsigned)((p0 - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) >> 10));
	fflush(stdout);

	for (size_t i = 0; i < sizeof(s_beep) / sizeof(s_beep[0]); i++)
		s_beep[i] = (int16_t)(6000.0f *
				sinf(2.0f * (float)M_PI * 880.0f * (float)i /
				     16000.0f));

	xTaskCreatePinnedToCore(feed_task, "wn_feed", 4096, NULL, 5, NULL, 0);
	xTaskCreatePinnedToCore(detect_task, "wn_det", 8192, NULL, 5, NULL, 1);
	if (selftest)
		xTaskCreatePinnedToCore(announce_task, "wn_say", 8192, tts,
					4, NULL, 0);
	printf("VL,WN,LISTENING%s\r\n", selftest ? ",selftest" : "");
	fflush(stdout);
}
