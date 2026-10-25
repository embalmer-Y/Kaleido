/*
 * voice_loop_main.c — M1.5 语音全链 HIL：采集→ASR→LLM 流式→TTS→播放
 *
 * 目的：方案 v1.5 §6 M1.5 验收——P50<1.5s / P90<2.2s（采集结束→首音出声）。
 * 链路：串口 'g' 触发 → 6s 采音（喂入即流式送 ASR）→ 定稿文本 →
 *       compatible-mode SSE（句读回调）→ TTS（预开连接）→ I2S 播放。
 * 所有时间戳 esp_timer；输出 "VL,RESULT,…" 单行供统计。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "kaleido_speech.h"
#include "llm_stream.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#define CAP_SECONDS	30 /* 负责人要求：真人轮 30s 窗防错过（'t' 内置轮不受影响）*/
#define CAP_CHUNK	1600 /* 100ms @16k              */
#define ROUND_MAX	5

static const char *TAG = "vl";

extern const char test_prompt_start[] asm("_binary_test_prompt_pcm_start");
extern const char test_prompt_end[] asm("_binary_test_prompt_pcm_end");

static char s_asr_text[512];
static volatile int64_t t_capture_end, t_asr_final;
static volatile int64_t t_llm_first, t_llm_first_clause, t_tts_first_pcm,
    t_first_audio;
static volatile int s_round;

/* ---- 播放通路：WS 回调(WS 任务) → StreamBuffer → 播放任务 → I2S ---- */

static StreamBufferHandle_t s_pb;
/* TTS 突发下发远超实时：32KB 缓冲溢出丢块 => 语速忽快忽慢/后半段字挤在一起
 * （负责人听感报告）。扩到 PSRAM 768KB（≈24s 回复全容纳）+ 100ms 背压 */
#define PB_BYTES	786432

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
	size_t w = xStreamBufferSend(s_pb, pcm, len, pdMS_TO_TICKS(100));

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

/* ---- LLM 句读回调：首句即送 TTS（TTS 已在采音窗内预开） ---- */

static volatile bool s_tts_open;

/* M1.5 杠杆：轮开始即后台开 TTS 会话，~2.8s 冷建连藏进采音窗 */
static void tts_prewarm_task(void *arg)
{
	struct ks_tts_cfg *cfg = arg;

	if (ks_tts_open(cfg, &(struct ks_tts_events){
		    .on_pcm = on_tts_pcm }) == 0) {
		s_tts_open = true;
	} else {
		ESP_LOGE(TAG, "tts prewarm fail");
	}
	vTaskDelete(NULL);
}

static bool on_llm_sentence(void *user, const char *sentence,
			    bool final)
{
	(void)user; /* TTS 已在采音窗内预开（M1.5），此处只管投喂 */

	if (!t_llm_first_clause && sentence[0])
		t_llm_first_clause = esp_timer_get_time();
	if (final && !sentence[0]) {
		if (s_tts_open) {
			ks_tts_feed(NULL, true, 15000);
			s_tts_open = false;
		}
		return true;
	}
	if (!s_tts_open) {
		/* 预开通路径（M1.5）：asr_finish 后立即开好，此处无需再开 */
		ESP_LOGE(TAG, "tts not prewarmed");
		return false;
	}
	if (!sentence[0])
		return true; /* 空子句防御：喂了会 task-failed */
	int rc = ks_tts_feed(sentence, false, 15000);

	if (rc)
		return false;
	if (final) {
		ks_tts_feed(NULL, true, 15000);
		s_tts_open = false;
	}
	return true;
}

/* ---- 声学环回自检（负责人提议：板载喇叭发声、麦克风采回验证） ----
 *
 * 'l'：1kHz 正弦环回（噪声底/信噪比/谱峰量化声学链）
 * 'a'：内置语音喇叭外放 → 麦克风采回 → ASR（真实语音过空气的端到端证明）
 *      → LLM → TTS 外放（播放期同步采音证明出声）
 */

static int rms_i16(const int16_t *p, size_t n)
{
	int64_t acc = 0;

	for (size_t i = 0; i < n; i++)
		acc += (int64_t)p[i] * p[i];
	return (int)sqrt((double)acc / (n ? n : 1));
}

static float goertzel(const int16_t *x, size_t n, float freq, float sr)
{
	size_t k = (size_t)(n * freq / sr + 0.5f);
	float w = 2.0f * (float)M_PI * (float)k / (float)n;
	float coeff = 2.0f * cosf(w);
	float s1 = 0.0f, s2 = 0.0f;

	for (size_t i = 0; i < n; i++) {
		float s0 = (float)x[i] + coeff * s1 - s2;

		s2 = s1;
		s1 = s0;
	}
	return sqrtf(s1 * s1 + s2 * s2 - coeff * s1 * s2);
}

static void tone_play_task(void *arg)
{
	aio_play(arg, 16000, 3000);
	vTaskDelete(NULL);
}

/* 'p'：NS4150 使能脚（GPIO53）两种极性各放 1s 音，麦克风自动判别 */
static void pa_polarity_probe(void)
{
	gpio_config_t g = {
		.pin_bit_mask = 1ULL << 53,
		.mode = GPIO_MODE_OUTPUT,
	};
	static int16_t capbuf[16000];

	gpio_config(&g);
	for (int lvl = 1; lvl >= 0; lvl--) {
		float bin, bin_lo;
		static int16_t tone[16000];

		for (size_t i = 0; i < 16000; i++)
			tone[i] = (int16_t)(8000.0f *
					    sinf(2.0f * (float)M_PI *
						 1000.0f * (float)i /
						 16000.0f));
		gpio_set_level(53, lvl);
		vTaskDelay(pdMS_TO_TICKS(50));
		xTaskCreatePinnedToCore(tone_play_task, "tone", 4096, tone,
					6, NULL, 0);
		if (aio_capture(capbuf, 16000, 3000) != 16000) {
			printf("VL,P,lvl=%d,capture_fail\r\n", lvl);
			fflush(stdout);
			continue;
		}
		bin = goertzel(capbuf, 16000, 1000.0f, 16000.0f);
		bin_lo = goertzel(capbuf, 16000, 940.0f, 16000.0f);
		printf("VL,P,pa_lvl=%d,rms=%d,g1000=%.0f,g940=%.0f,"
		       "tone=%s\r\n",
		       lvl, rms_i16(capbuf, 16000), (double)bin,
		       (double)bin_lo,
		       (bin > bin_lo * 4.0f) ? "YES" : "no");
		fflush(stdout);
		vTaskDelay(pdMS_TO_TICKS(200));
	}
	gpio_set_level(53, 1); /* 恢复驱动默认极性 */
}

/* 's'：SD 卡自检（slot0 IOMUX 39-44，与 C5 slot1 并行）：
 * 挂载 → 1MB 写/读/校验 → 容量与速度报告 */
static void sd_test(void)
{
	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
	esp_vfs_fat_sdmmc_mount_config_t mc = {
		.format_if_mount_failed = false,
		.max_files = 4,
		.allocation_unit_size = 16 * 1024,
	};
	sdmmc_card_t *card = NULL;
	int64_t t0;
	uint8_t *buf = heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM);
	uint8_t *rb = buf ? heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM) :
		 NULL;

	host.slot = SDMMC_HOST_SLOT_0;
	slot.cd = SDMMC_SLOT_NO_CD; /* 先不卡检测，直挂 */
	slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
	if (!buf || !rb) {
		printf("VL,S,FAIL,oom\r\n");
		fflush(stdout);
		goto out;
	}
	/* 重试矩阵：首验 4bit@20M 在 SSR 读取超时（卡对速率/位宽敏感）；
	 * 失败清理会波及共享控制器上的 slot1(C5)，所以逐级降参一次挂成 */
	static const int try_width[3] = { 4, 1, 1 };
	static const int try_khz[3] = {
		SDMMC_FREQ_DEFAULT, SDMMC_FREQ_DEFAULT, SDMMC_FREQ_PROBING
	};
	esp_err_t err = ESP_FAIL;

	for (int i = 0; i < 3 && err != ESP_OK; i++) {
		slot.width = try_width[i];
		host.max_freq_khz = try_khz[i];
		printf("VL,S,TRY,width=%d,khz=%d\r\n", try_width[i],
		       try_khz[i]);
		fflush(stdout);
		err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mc,
					      &card);
	}

	if (err != ESP_OK) {
		printf("VL,S,FAIL,mount=%d\r\n", err);
		fflush(stdout);
		/* 不 unmount：失败态下清理会波及 slot1(C5)，复位恢复 */
		goto out;
	}
	sdmmc_card_print_info(stdout, card);

	for (size_t i = 0; i < 256 * 1024; i++)
		buf[i] = (uint8_t)i;

	FILE *f = fopen("/sd/kaleido_sdtest.bin", "wb");

	if (!f) {
		printf("VL,S,FAIL,open_w\r\n");
		fflush(stdout);
		goto unmount;
	}
	t0 = esp_timer_get_time();
	for (int i = 0; i < 4; i++) { /* 4×256KB = 1MB */
		if (fwrite(buf, 1, 256 * 1024, f) != 256 * 1024) {
			printf("VL,S,FAIL,write\r\n");
			fflush(stdout);
			fclose(f);
			goto unmount;
		}
	}
	fclose(f);
	int64_t t_w = esp_timer_get_time() - t0;

	f = fopen("/sd/kaleido_sdtest.bin", "rb");
	if (!f) {
		printf("VL,S,FAIL,open_r\r\n");
		fflush(stdout);
		goto unmount;
	}
	t0 = esp_timer_get_time();
	for (int i = 0; i < 4; i++) {
		if (fread(rb, 1, 256 * 1024, f) != 256 * 1024 ||
		    memcmp(buf, rb, 256 * 1024) != 0) {
			printf("VL,S,FAIL,read_verify\r\n");
			fflush(stdout);
			fclose(f);
			goto unmount;
		}
	}
	fclose(f);
	int64_t t_r = esp_timer_get_time() - t0;

	printf("VL,S,RESULT,write=%.2fMB/s,read=%.2fMB/s,verify=OK\r\n",
	       4.0 / ((double)t_w / 1e6), 4.0 / ((double)t_r / 1e6));
	fflush(stdout);
	remove("/sd/kaleido_sdtest.bin");
unmount:
	esp_vfs_fat_sdcard_unmount("/sd", card);
out:
	free(buf);
	free(rb);
}

static void loopback_tone(void)
{
	static int16_t silence[16000];
	static int16_t capbuf[16000];
	static int16_t tone[16000];

	for (size_t i = 0; i < 16000; i++)
		tone[i] = (int16_t)(8000.0f *
				    sinf(2.0f * (float)M_PI * 1000.0f *
					 (float)i / 16000.0f));

	int n_noise = aio_capture(silence, 16000, 2000);

	if (n_noise != 16000) {
		printf("VL,L,FAIL,capture_noise=%d\r\n", n_noise);
		fflush(stdout);
		return;
	}
	int rms_noise = rms_i16(silence, 16000);

	xTaskCreatePinnedToCore(tone_play_task, "tone", 4096, tone, 6,
				NULL, 0);
	int n_sig = aio_capture(capbuf, 16000, 3000);

	if (n_sig != 16000) {
		printf("VL,L,FAIL,capture_sig=%d\r\n", n_sig);
		fflush(stdout);
		return;
	}
	int rms_sig = rms_i16(capbuf, 16000);
	float bin = goertzel(capbuf, 16000, 1000.0f, 16000.0f);
	float bin_lo = goertzel(capbuf, 16000, 940.0f, 16000.0f);
	float bin_hi = goertzel(capbuf, 16000, 1060.0f, 16000.0f);
	float snr = 20.0f *
	    log10f(((float)rms_sig + 1.0f) / ((float)rms_noise + 1.0f));

	printf("VL,L,RESULT,noise_rms=%d,sig_rms=%d,snr_db=%.1f,"
	       "g1000=%.0f,g940=%.0f,g1060=%.0f,verdict=%s\r\n",
	       rms_noise, rms_sig, (double)snr, (double)bin,
	       (double)bin_lo, (double)bin_hi,
	       (bin > bin_lo * 2.5f &&
		bin > bin_hi * 2.5f) ? "PASS" : "FAIL");
	fflush(stdout);
}

/* 喇叭外放内置语音 + 同步采音 */
static void prompt_play_task(void *arg)
{
	const int16_t *p = (const void *)test_prompt_start;
	size_t total = (test_prompt_end - test_prompt_start) / 2;

	aio_play(p, total, 5000);
	vTaskDelete(NULL);
}

/* TTS 播放期采音：每秒报 RMS，证明出声 */
static void tts_listen_task(void *arg)
{
	static int16_t win[16000];
	int64_t *t_end = arg;

	while (esp_timer_get_time() < *t_end) {
		int n = aio_capture(win, 16000, 1500);

		if (n <= 0)
			break;
		printf("VL,A,TTS_RMS,%d\r\n", rms_i16(win, n));
		fflush(stdout);
	}
	vTaskDelete(NULL);
}

static void acoustic_round(struct ks_asr_cfg *asr, struct ks_tts_cfg *tts,
			   struct llm_cfg *llm)
{
	size_t total = (test_prompt_end - test_prompt_start) / 2;
	int16_t *cap = heap_caps_malloc(total * 2, MALLOC_CAP_SPIRAM);
	static int16_t chunk[CAP_CHUNK];
	int64_t t_listen_end;

	if (!cap) {
		printf("VL,A,FAIL,oom\r\n");
		fflush(stdout);
		return;
	}
	printf("VL,A,ROUND,speaker plays prompt %us, mic listens\r\n",
	       (unsigned)(total / 16000));
	fflush(stdout);

	/* ① 静默底噪（1s） */
	static int16_t noise[16000];
	int rms_noise = 0;

	if (aio_capture(noise, 16000, 2000) == 16000)
		rms_noise = rms_i16(noise, 16000);

	/* ② 外放 + 同步采回 */
	s_asr_text[0] = '\0';
	s_tts_open = false;
	t_capture_end = t_asr_final = 0;
	t_llm_first = t_llm_first_clause = t_tts_first_pcm = t_first_audio = 0;

	xTaskCreatePinnedToCore(prompt_play_task, "pp", 4096, NULL, 6,
				NULL, 0);
	size_t got = 0;

	while (got < total) {
		int n = aio_capture(chunk, CAP_CHUNK, 1000);

		if (n <= 0)
			break;
		memcpy(cap + got, chunk, n * 2);
		got += n;
	}
	/* 播放与采集同钟：采满 total 即放完；再等 DMA 尾部排空 */
	vTaskDelay(pdMS_TO_TICKS(300));
	t_capture_end = esp_timer_get_time();
	int rms_play = got ? rms_i16(cap, got) : 0;

	printf("VL,A,ACOUSTIC,noise_rms=%d,played_rms=%d,got=%u/%u,"
	       "air_snr_db=%.1f\r\n",
	       rms_noise, rms_play, (unsigned)got, (unsigned)total,
	       (double)(20.0f * log10f(((float)rms_play + 1.0f) /
				       ((float)rms_noise + 1.0f))));
	fflush(stdout);

	/* ③ 采回的音频送 ASR（喇叭→空气→麦→云端 的语音级证明） */
	if (ks_asr_open(asr, &(struct ks_asr_events){
			.on_text = on_asr_text }) != 0) {
		printf("VL,A,FAIL,asr_open\r\n");
		fflush(stdout);
		free(cap);
		return;
	}
	for (size_t off = 0; off < got; off += CAP_CHUNK) {
		size_t n = got - off;

		if (n > CAP_CHUNK)
			n = CAP_CHUNK;
		if (ks_asr_feed(cap + off, n) < 0)
			break;
		/* 快喂（采音频已完整，无需 1x 节奏） */
	}
	if (ks_asr_finish(15000) != 0) {
		printf("VL,A,FAIL,asr_finish\r\n");
		fflush(stdout);
		ks_asr_close();
		free(cap);
		return;
	}
	t_asr_final = esp_timer_get_time();
	ks_asr_close();
	printf("VL,A,ASR_ECHO,%s\r\n",
	       s_asr_text[0] ? s_asr_text : "(empty)");
	fflush(stdout);
	if (!s_asr_text[0]) {
		free(cap);
		return;
	}

	/* ④ LLM → TTS 外放（TTS 预开与 LLM 首句并行）；播放期同步采音 */
	if (ks_tts_open(tts, &(struct ks_tts_events){
			.on_pcm = on_tts_pcm }) == 0) {
		s_tts_open = true;
	} else {
		ESP_LOGE(TAG, "tts prewarm fail");
	}
	t_listen_end = esp_timer_get_time() + 15000000;
	xTaskCreatePinnedToCore(tts_listen_task, "tlisten", 4096,
				&t_listen_end, 4, NULL, 0);
	llm->user_msg = s_asr_text;
	int64_t ftok = 0;
	int rc = llm_stream_chat(llm, on_llm_sentence, tts, &ftok);

	t_llm_first = ftok;
	ks_tts_close();
	free(cap);
	if (rc != 0) {
		printf("VL,A,FAIL,llm=%d\r\n", rc);
		fflush(stdout);
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(500));
	printf("VL,A,RESULT,asr_final=%lld,llm_first=%lld,"
	       "tts_first_pcm=%lld,first_audio=%lld,"
	       "capture_to_audio=%lld\r\n",
	       (t_asr_final - t_capture_end) / 1000,
	       t_llm_first / 1000,
	       t_tts_first_pcm ? (t_tts_first_pcm - t_capture_end) / 1000 : -1,
	       t_first_audio ? (t_first_audio - t_capture_end) / 1000 : -1,
	       t_first_audio ? (t_first_audio - t_capture_end) / 1000 : -1);
	fflush(stdout);
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

	/* 轮隔离：等上一轮回复播完（≤20s），防残留音频污染本轮时间戳 */
	for (int i = 0; i < 100 && xStreamBufferBytesAvailable(s_pb); i++)
		vTaskDelay(pdMS_TO_TICKS(200));

	s_asr_text[0] = '\0';
	s_tts_open = false;                 /* G6：跨轮复位      */
	t_capture_end = t_asr_final = 0;
	t_llm_first = t_llm_first_clause = t_tts_first_pcm = t_first_audio = 0;

	printf("VL,ROUND,%d,speak now %ds\r\n", s_round, CAP_SECONDS);
	printf("VL,HEAP,int=%uk,psram=%uk\r\n",
	       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
	       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	fflush(stdout);

	if (ks_asr_open(asr, &(struct ks_asr_events){
			.on_text = on_asr_text }) != 0) {
		ESP_LOGE(TAG, "asr open fail");
		return -1;
	}
	xTaskCreatePinnedToCore(tts_prewarm_task, "ttspre", 6144, tts, 5,
				NULL, 0);
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
		/* 真人轮协议：哔一声（200ms 1kHz）= 开始说话 */
		static int16_t cue[3200];

		for (size_t i = 0; i < 3200; i++)
			cue[i] = (int16_t)(4000.0f *
				    sinf(2.0f * (float)M_PI * 1000.0f *
					 (float)i / 16000.0f));
		printf("VL,LIVE,beep=speak now %ds\r\n", CAP_SECONDS);
		fflush(stdout);
		aio_play(cue, 3200, 1000);
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
	       "llm_first=%lld,clause=%lld,tts_first_pcm=%lld,"
	       "first_audio=%lld,"
	       "total_trigger_to_audio=%lld,total_capture_to_audio=%lld\r\n",
	       s_round,
	       (unsigned)(fed * 100 / 16000),
	       (t_asr_final - t_capture_end) / 1000,
	       t_llm_first / 1000,
	       t_llm_first_clause ?
		       (t_llm_first_clause - t_capture_end) / 1000 : -1,
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
		.cut_clauses = true, /* M1.5：子句级切送，首音不等句号 */
	};

	esp_log_level_set("*", ESP_LOG_WARN);
	nvs_flash_init();
	wifi_start();
	if (aio_init() != 0) {
		ESP_LOGE(TAG, "aio fail");
		return;
	}
	/* pvPortMalloc 只走内部堆（实测 768KB 创建即 NULL→断言），缓冲数据
	 * 自取 PSRAM + Static 版本创建 */
	static StaticStreamBuffer_t s_pb_static;
	uint8_t *pb_mem = heap_caps_malloc(PB_BYTES, MALLOC_CAP_SPIRAM);

	if (!pb_mem) {
		ESP_LOGE(TAG, "pb mem fail");
		return;
	}
	s_pb = xStreamBufferCreateStatic(PB_BYTES, 1, pb_mem,
					 &s_pb_static);
	if (!s_pb) {
		ESP_LOGE(TAG, "pb create fail");
		return;
	}
	xTaskCreatePinnedToCore(play_task, "pb", 4096, NULL, 5, NULL, 0);

	printf("VL,READY,press 'g'+Enter to run a round\r\n");
	fflush(stdout);
	while (s_round < ROUND_MAX) {
		int ch = getchar();

		if (ch == 's' || ch == 'S') {
			sd_test();
			continue;
		}
		if (ch == 'p' || ch == 'P') {
			pa_polarity_probe();
			continue;
		}
		if (ch == 'l' || ch == 'L') {
			loopback_tone();
			continue;
		}
		if (ch == 'a' || ch == 'A') {
			acoustic_round(&asr, &tts, &llm);
			continue;
		}
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
