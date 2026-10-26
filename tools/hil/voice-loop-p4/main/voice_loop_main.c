/*
 * voice_loop_main.c — M1.5 语音全链 HIL：采集→ASR→LLM 流式→TTS→播放
 *
 * 目的：方案 v1.5 §6 M1.5 验收——P50<1.5s / P90<2.2s（采集结束→首音出声）。
 * 链路：串口 'g' 触发 → 30s 采音窗（边采边流式送 ASR）→ 定稿文本 →
 *       chat/completions SSE（ks_llm_chat，句读回调）→ 子句泵
 *       （常驻队列+feeder 任务逐句喂 TTS）→ PCM → StreamBuffer → I2S。
 * 所有时间戳 esp_timer；输出 "VL,RESULT,…" 单行供统计。
 *
 * 任务拓扑（谁拥有什么，改前先看这张图）：
 *   app_main（本文件）：轮编排、遥测，串口菜单严格同步（键无并发）
 *   ttsfeed（常驻）：子句队列 → ks_tts_feed（MiniMax 每句一 HTTPS）
 *   pb（常驻）：s_pb StreamBuffer → aio_play，扬声器唯一写者
 *   wn_feed/wn_det（'w' 后常驻）：AFE 喂音/检测，唤醒即 beep
 *   一批一次性探测任务：tone/prompt/tlisten（prio 6，可与 pb 并发，
 *   由 audio_io 内部互斥锁兜底）
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "clause_pump.h"
#include "convo_fsm.h"
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
#include "kaleido_secret.h"
#include "kaleido_speech.h"
#include "nvs_flash.h"
#include "provision.h"
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
/* 首子句时刻由子句泵记录：pump_first_clause_us() */
static volatile int64_t t_llm_first, t_tts_first_pcm, t_first_audio;
static volatile int s_round;

/* ---- 播放通路：WS 回调(WS 任务) → StreamBuffer → 播放任务 → I2S ---- */

static StreamBufferHandle_t s_pb;
/* TTS 突发下发远超实时：32KB 缓冲溢出丢块 => 语速忽快忽慢/后半段字挤在一起
 * （负责人听感报告）。扩到 PSRAM 768KB（≈24s 回复全容纳）+ 100ms 背压 */
#define PB_BYTES	786432

void wakenet_start(struct ks_tts_cfg *tts, bool selftest,
		   void (*on_wake)(void));
bool wakenet_selftest_busy(void);

/* convo_fsm 查询播放积压（轮隔离等待用）；单一写者 play_task */
size_t pb_pending(void)
{
	return s_pb ? xStreamBufferBytesAvailable(s_pb) : 0;
}

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

void on_tts_pcm(void *user, const uint8_t *pcm, size_t len)
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

/* ---- 子句泵与状态机（V-43 抽模块：见 clause_pump.c / convo_fsm.c）----
 * V-41 解嵌套史与幽灵消费者治理的完整记录见 docs/review-2026-10-26 */
static bool s_use_minimax;
static char s_mm_url[160];
static volatile bool s_busy;	/* 菜单轮/自检进行中：互斥 'g'/'a'/'W'/'c' */

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

/* TTS 播放期采音：每秒报 RMS，证明出声。截止时刻用静态存储——
 * 曾把栈变量指针交给本任务，acoustic_round 返回后任务仍在解引用
 * 已死栈帧（评审 2026-10-26 BUG#6） */
static int64_t s_listen_end;

static void tts_listen_task(void *arg)
{
	static int16_t win[16000];

	(void)arg;
	while (esp_timer_get_time() < s_listen_end) {
		int n = aio_capture(win, 16000, 1500);

		if (n <= 0)
			break;
		printf("VL,A,TTS_RMS,%d\r\n", rms_i16(win, n));
		fflush(stdout);
	}
	vTaskDelete(NULL);
}

static void acoustic_round(struct ks_asr_cfg *asr, struct ks_tts_cfg *tts,
			   struct ks_llm_cfg *llm)
{
	size_t total = (test_prompt_end - test_prompt_start) / 2;
	int16_t *cap = heap_caps_malloc(total * 2, MALLOC_CAP_SPIRAM);
	static int16_t chunk[CAP_CHUNK];
	int64_t ftok = 0;
	int rms_noise = 0;
	int rms_play = 0;
	size_t got = 0;
	int rc;

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

	if (aio_capture(noise, 16000, 2000) == 16000)
		rms_noise = rms_i16(noise, 16000);

	/* ② 外放 + 同步采回 */
	s_asr_text[0] = '\0';
	pump_reset();
	t_capture_end = t_asr_final = 0;
	t_llm_first = t_tts_first_pcm = t_first_audio = 0;

	xTaskCreatePinnedToCore(prompt_play_task, "pp", 4096, NULL, 6,
				NULL, 0);

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
	rms_play = got ? rms_i16(cap, got) : 0;

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
		goto out;
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
		goto out;
	}
	t_asr_final = esp_timer_get_time();
	ks_asr_close();
	printf("VL,A,ASR_ECHO,%s\r\n",
	       s_asr_text[0] ? s_asr_text : "(empty)");
	fflush(stdout);
	if (!s_asr_text[0])
		goto out;

	/* ④ LLM → 子句泵 → TTS 外放；播放期同步采音 */
	pump_tts_attach(tts, on_tts_pcm);
	s_listen_end = esp_timer_get_time() + 15000000;
	xTaskCreatePinnedToCore(tts_listen_task, "tlisten", 4096,
				NULL, 4, NULL, 0);
	llm->user_msg = s_asr_text;
	rc = ks_llm_chat(llm, pump_on_llm_sentence, NULL, &ftok);
	t_llm_first = ftok;

	if (!pump_stop(30000))
		ESP_LOGW(TAG, "pump not done in 30s");
	pump_tts_detach();
	if (rc != 0) {
		printf("VL,A,FAIL,llm=%d\r\n", rc);
		fflush(stdout);
		goto out;
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
out:
	free(cap);
}

/* ---- WiFi（M1 已验证路径，最简 station）----
 * 凭据走取值链 NVS → CONFIG（HIL 开发后备）；netif/事件环由 app_main
 * 先行初始化（OOBE 的 SoftAP 分支也依赖） */
static void wifi_start(const char *ssid, const char *pass)
{
	wifi_init_config_t w = WIFI_INIT_CONFIG_DEFAULT();
	wifi_config_t wc = { 0 };

	/* 返回 netif 指针（非 esp_err_t，勿包 ESP_ERROR_CHECK） */
	esp_netif_create_default_wifi_sta();
	ESP_ERROR_CHECK(esp_wifi_init(&w));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
	strlcpy((char *)wc.sta.password, pass,
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
		     struct ks_llm_cfg *llm, bool embedded)
{
	static int16_t chunk[CAP_CHUNK];
	int64_t t_trigger = esp_timer_get_time();
	int64_t cap_start;
	int64_t ftok = 0;
	size_t fed = 0;
	int rc;

	/* 轮隔离：等上一轮回复播完（≤20s），防残留音频污染本轮时间戳 */
	for (int i = 0; i < 100 && xStreamBufferBytesAvailable(s_pb); i++)
		vTaskDelay(pdMS_TO_TICKS(200));

	s_asr_text[0] = '\0';
	pump_reset();
	t_capture_end = t_asr_final = 0;
	t_llm_first = t_tts_first_pcm = t_first_audio = 0;

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
	/* V-41 解嵌套史见 clause_pump.c；此处同步开 TTS（MiniMax 的
	 * open 仅解析 JWT，µs 级） */
	if (pump_tts_attach(tts, on_tts_pcm) != 0)
		ESP_LOGE(TAG, "tts attach fail");

	/* 边采边喂（产品真实形态：流式）；embedded 模式以 1x 节奏
	 * 喂内置测试 PCM（无人值守回归） */
	cap_start = esp_timer_get_time();

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

	rc = 0;
	if (ks_asr_finish(15000) != 0) {
		ESP_LOGE(TAG, "asr finish fail");
		ks_asr_close();
		rc = -2;
		goto out;
	}
	t_asr_final = esp_timer_get_time();
	ks_asr_close();
	printf("VL,ASR,%s\r\n", s_asr_text[0] ? s_asr_text : "(empty)");
	fflush(stdout);
	if (!s_asr_text[0]) {
		rc = -3;
		goto out;
	}

	/* LLM 流式 → 子句入队；泵排空（含 TTS flush）后再收口会话。
	 * 所有路径（含成功）统一过 out：哨兵+close 保证无幽灵会话/任务，
	 * 泵收口幂等——feeder 空闲时哨兵即回即取，代价可忽略 */
	llm->user_msg = s_asr_text;
	rc = ks_llm_chat(llm, pump_on_llm_sentence, NULL, &ftok);
	t_llm_first = ftok;
	if (!pump_stop(30000))
		ESP_LOGW(TAG, "pump not done in 30s");
	if (rc != 0) {
		ESP_LOGE(TAG, "llm fail %d", rc);
		rc = -4;
		goto out;
	}

	/* 等播放队列清空（尾部余量） */
	vTaskDelay(pdMS_TO_TICKS(500));
	printf("VL,RESULT,round=%d,fed_ms=%u,asr_final=%lld,"
	       "llm_first=%lld,clause=%lld,tts_first_pcm=%lld,"
	       "first_audio=%lld,"
	       "total_trigger_to_audio=%lld,total_capture_to_audio=%lld\r\n",
	       s_round,
	       (unsigned)(fed * 100 / 16000),
	       (t_asr_final - t_capture_end) / 1000,
	       t_llm_first / 1000,
	       pump_first_clause_us() ?
		       (pump_first_clause_us() - t_capture_end) / 1000 : -1,
	       t_tts_first_pcm ? (t_tts_first_pcm -
				  t_capture_end) / 1000 : -1,
	       t_first_audio ? (t_first_audio - t_capture_end) / 1000 : -1,
	       t_first_audio ? (t_first_audio - t_trigger) / 1000 : -1,
	       t_first_audio ? (t_first_audio -
				t_capture_end) / 1000 : -1);
	fflush(stdout);
out:
	/* 早退路径泵可能尚未收口（预热会话还开着）：flush 哨兵再关 */
	if (!pump_stop(15000))
		ESP_LOGW(TAG, "pump cleanup timeout");
	pump_tts_detach();
	return rc;
}

/* ---- 凭据取值链（V-42 OOBE）：NVS → CONFIG（HIL 开发后备）→ 空 ---- */
static char s_wifi_ssid[33];
static char s_wifi_pass[64];
static char s_ds_key[256];	/* 百炼 ASR key */
static char s_mm_key_buf[768];	/* MiniMax JWT（~690B） */

static void creds_resolve(void)
{
	ksec_get_fallback(KSEC_KEY_WIFI_SSID, CONFIG_VL_WIFI_SSID,
			  s_wifi_ssid, sizeof(s_wifi_ssid));
	ksec_get_fallback(KSEC_KEY_WIFI_PASS, CONFIG_VL_WIFI_PASS,
			  s_wifi_pass, sizeof(s_wifi_pass));
	ksec_get_fallback(KSEC_KEY_DS, CONFIG_VL_DS_KEY, s_ds_key,
			  sizeof(s_ds_key));
	ksec_get_fallback(KSEC_KEY_MM, CONFIG_KS_MM_KEY, s_mm_key_buf,
			  sizeof(s_mm_key_buf));
	printf("VL,OOBE,nvs_mask=0x%x,wifi=%s,asr=%s,llm=%s\r\n",
	       ksec_state(),
	       s_wifi_ssid[0] ? "y" : "n",
	       s_ds_key[0] ? "y" : "n",
	       s_mm_key_buf[0] ? "y" : "n");
	fflush(stdout);
}

void app_main(void)
{
	static struct ks_asr_cfg asr = {
		.url = "wss://dashscope.aliyuncs.com",
		.model = "paraformer-realtime-v2",
		.sample_rate = 16000,
	};
	/* V-41：TTS/LLM 切 MiniMax（ASR 留百炼 paraformer-realtime-v2）。
	 * 模型选型实测见 docs/research/27：M3 关思考=旗舰质量+首子句 ~0.7s；
	 * speech-2.5-hd-preview=HD 质量档且首音最快（~0.25s） */
	static struct ks_tts_cfg tts = {
		.url = "",
		.model = CONFIG_KS_TTS_MM_MODEL,
		.voice = CONFIG_KS_TTS_MM_VOICE,
		.sample_rate = 16000,
	};
	static struct ks_llm_cfg llm = {
		.url = "https://dashscope.aliyuncs.com/"
		       "compatible-mode/v1/chat/completions",
		.model = "qwen-plus",
		.system = "你是桌搭小生物墨墨，口语化短句回答，每句不超过"
			  "30个字。",
		.user_msg = "",
		.cut_clauses = true, /* M1.5：子句级切送，首音不等句号 */
	};

	esp_log_level_set("*", ESP_LOG_WARN);
	nvs_flash_init();
	esp_netif_init();
	esp_event_loop_create_default();
	creds_resolve();
	asr.api_key = s_ds_key;
	tts.api_key = s_mm_key_buf;
	llm.api_key = s_mm_key_buf;

	/* 首电分流（V-42）：无 WiFi 凭据 → SoftAP + Web 配置页；
	 * 有则连网，密钥缺口可随时经 'k'（浏览器）或串口后备补 */
	bool oobe_ap = !s_wifi_ssid[0] || !s_wifi_pass[0];

	if (oobe_ap) {
		if (aio_init() != 0) {
			ESP_LOGE(TAG, "aio fail");
			return;
		}
		if (provision_ap_start() != 0) {
			ESP_LOGE(TAG, "oobe ap fail（串口后备见文档）");
			return;
		}
	} else {
		wifi_start(s_wifi_ssid, s_wifi_pass);
		if (aio_init() != 0) {
			ESP_LOGE(TAG, "aio fail");
			return;
		}
	}

	/* MiniMax 优先（URL 内嵌 GroupID，由组件解析 JWT 拼好）；
	 * 无 key/解析失败回落 DashScope compatible-mode */
	if (s_mm_key_buf[0] &&
	    ks_llm_minimax_url(s_mm_url, sizeof(s_mm_url),
			       s_mm_key_buf) == 0) {
		llm.url = s_mm_url;
		llm.model = "MiniMax-M3";
		llm.no_think = true;
		s_use_minimax = true;
		printf("VL,LLM,MINIMAX,ok\r\n");
	} else {
		llm.api_key = CONFIG_VL_LLM_KEY;
		printf("VL,LLM,MINIMAX,fallback_dashscope\r\n");
	}

	/* 未配齐云端 key：Web 配置页常驻本局域网（负责人指示：浏览器粘贴） */
	if (!s_ds_key[0] || !s_mm_key_buf[0])
		provision_server_start();
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

	/* 子句泵常驻（一次创建，轮内 reset/stop 控制生命周期） */
	if (pump_init() != 0)
		return;

	printf("VL,READY,keys: g=round t=embedded a=acoustic w=wakenet "
	       "W=wn-selftest c=hands-free n=manual-wake C=auto-soak "
	       "M=llm-toggle k=web-config o=ap-probe u=stats s=sd p=pa "
	       "l=loopback (%d menu rounds)\r\n", ROUND_MAX);
	fflush(stdout);
	while (s_round < ROUND_MAX) {
		int ch = getchar();
		bool round_key = (ch == 'g' || ch == 'G' || ch == 't' ||
				  ch == 'T');

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
		/* 互斥门：轮次/声学环回与唤醒自检都会独占 TTS 会话
		 * （ks_tts_open 会清 s_mm 单例），并发即互相拆台 */
		if ((round_key || ch == 'a' || ch == 'A' || ch == 'W') &&
		    (s_busy || wakenet_selftest_busy() || convo_active())) {
			printf("VL,BUSY\r\n");
			fflush(stdout);
			continue;
		}
		if (ch == 'a' || ch == 'A') {
			s_busy = true;
			acoustic_round(&asr, &tts, &llm);
			s_busy = false;
			continue;
		}
		if (ch == 'M') {
			if (s_use_minimax) {
				llm.url = "https://dashscope.aliyuncs.com/"
					  "compatible-mode/v1/chat/completions";
				llm.api_key = CONFIG_VL_LLM_KEY;
				llm.model = "qwen-plus";
				llm.no_think = false;
				s_use_minimax = false;
			} else {
				llm.url = s_mm_url;
				llm.api_key = s_mm_key_buf;
				llm.model = "MiniMax-M3";
				llm.no_think = true;
				s_use_minimax = true;
			}
			printf("VL,LLM,PROVIDER,%s\r\n",
			       s_use_minimax ? "minimax" : "dashscope");
			fflush(stdout);
			continue;
		}
		if (ch == 'k' || ch == 'K') {
			/* Web 配置页（浏览器粘贴长密钥；负责人指示的主通道） */
			if (provision_server_start() != 0)
				printf("VL,OOBE,WEB,fail\r\n");
			fflush(stdout);
			continue;
		}
		if (ch == 'o' || ch == 'O') {
			/* V-42a 运行时探针：强制切 SoftAP 热点态（esp_hosted
			 * AP 走 C5 RPC，bring-up 是否成功必须上板看）。
			 * 复位即恢复 STA */
			printf("VL,OOBE,AP,switching\r\n");
			fflush(stdout);
			esp_wifi_stop();
			if (provision_ap_start() != 0)
				printf("VL,OOBE,AP,fail\r\n");
			fflush(stdout);
			continue;
		}
		if (ch == 'c' || ch == 'C') {
			if (s_busy || wakenet_selftest_busy()) {
				printf("VL,BUSY\r\n");
				fflush(stdout);
				continue;
			}
			if (ch == 'C') {
				/* 自驱回归：TTS 过空气喊唤醒词+提问，
				 * N 轮无人值守（V-43 门禁判据） */
				wakenet_start(&tts, false, convo_notify_wake);
				convo_start(&asr, &tts, &llm);
				printf("VL,CONVO,SOAK,START,n=10\r\n");
				fflush(stdout);
				convo_soak_start(10);
				continue;
			}
			if (!convo_active()) {
				wakenet_start(&tts, false, convo_notify_wake);
				if (!convo_start(&asr, &tts, &llm)) {
					printf("VL,CONVO,START,fail\r\n");
					fflush(stdout);
					continue;
				}
				printf("VL,CONVO,ON,say=ni-hao-xiao-zhi\r\n");
			} else {
				convo_stop();
				printf("VL,CONVO,OFF\r\n");
			}
			fflush(stdout);
			continue;
		}
		if (ch == 'n' || ch == 'N') {
			/* 手动注入唤醒（无 wakenet 也能驱动 FSM 全链，
			 * 嘈杂环境/无声学时的调试入口） */
			if (convo_active()) {
				convo_notify_wake();
				printf("VL,CONVO,WAKE,manual\r\n");
			} else {
				printf("VL,CONVO,OFF,press-c-first\r\n");
			}
			fflush(stdout);
			continue;
		}
		if (ch == 'w') {
			wakenet_start(&tts, false, NULL);
			continue;
		}
		if (ch == 'u') {
			static char stats[4096];

			vTaskGetRunTimeStats(stats);
			printf("%s\r\n", stats);
			fflush(stdout);
			continue;
		}
		if (ch == 'W') {
			wakenet_start(&tts, true, NULL);
			continue;
		}
		if (!round_key)
			continue;
		bool embedded = (ch == 't' || ch == 'T');

		s_busy = true;
		s_round++;
		one_round(&asr, &tts, &llm, embedded);
		s_busy = false;
		printf("VL,READY,press 'g'+Enter (%d/%d used)\r\n",
		       s_round, ROUND_MAX);
		fflush(stdout);
	}
	printf("VL,DONE\r\n");
	fflush(stdout);
}
