/*
 * convo_fsm.c — 免按键会话状态机实现（V-43）
 *
 * 任务拓扑：
 *   convo_task（常驻）：等唤醒事件 → LISTENING 采音（VAD-lite）→
 *     THINKING（ASR 定稿→LLM 流式，子句进泵）→ SPEAKING（等泵排空 +
 *     播放缓冲清空）→ 回 IDLE。每轮一条 VL,CONVO,ROUND 遥测。
 *   soak_task（按需，'C'）：TTS 过空气扮演用户——喊唤醒词、在
 *     LISTENING 窗内提问，真实麦克风收音走完整链路，N 轮无人值守
 *     回归（V-40 已证 TTS 语音可稳定触发 wn9 模型）。
 *
 * 并发注释：soak_task 只在 IDLE/LISTENING 期用 TTS（此时 convo_task
 * 尚未 pump_tts_attach）；attach 发生在 ASR 定稿之后，与 soak 的
 * close 之间隔着 VAD 静音窗（~1.2s）+ asr_finish（~0.5s），无单例
 * 争用窗口。改轮流程时保持这个顺序。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "audio_io.h"
#include "clause_pump.h"
#include "convo_fsm.h"

/* voice_loop_main 的播放通路（单一写者原则下的跨模块挂点） */
extern void on_tts_pcm(void *user, const uint8_t *pcm, size_t len);
extern size_t pb_pending(void);
/* 内置语音（'t'/'a' 已证过空气可被 ASR 转写）：soak 用它扮演"用户
 * 提问"——避免 TTS 与 ASR WS 双 TLS 并发（实测该并发把内部 RAM 打穿：
 * esp-aes 对齐缓冲分配失败 → WS 写崩，8/10 轮 rc=-2 的真因） */
extern const char test_prompt_start[] asm("_binary_test_prompt_pcm_start");
extern const char test_prompt_end[] asm("_binary_test_prompt_pcm_end");

static const char *TAG = "convo";

#define LISTEN_MIN_MS	3000	/* 最短采音窗（防抢话截断） */
#define LISTEN_MAX_MS	12000	/* 最长采音窗 */
#define VAD_SILENCE_MS	1200	/* 说话后连续静音即提前截断 */
#define VAD_CHUNK	1600	/* 100ms @16k */

static volatile enum convo_state s_state = CONVO_OFF;
static volatile bool s_on;
static volatile int s_round_cnt;
static QueueHandle_t s_wake_q;
static struct ks_asr_cfg *s_asr;
static struct ks_tts_cfg *s_tts;
static struct ks_llm_cfg *s_llm;
static char s_text[512];

enum convo_state convo_state_get(void)
{
	return s_state;
}

bool convo_active(void)
{
	return s_on;
}

int convo_round_count(void)
{
	return s_round_cnt;
}

void convo_notify_wake(void)
{
	int dummy = 1;

	if (s_wake_q && s_on)
		xQueueSend(s_wake_q, &dummy, 0);
}

/* ---- ASR 定稿累积（本模块自有，不与菜单轮共用缓冲） ---- */

static void on_asr_text(void *user, const char *sentence, bool final)
{
	(void)user;
	if (!final || !sentence[0])
		return;
	strlcat(s_text, sentence, sizeof(s_text));
}

static int rms_i16(const int16_t *p, size_t n)
{
	int64_t acc = 0;

	for (size_t i = 0; i < n; i++)
		acc += (int64_t)p[i] * p[i];
	return (int)sqrt((double)acc / (n ? n : 1));
}

/* LISTENING 采音：边采边喂 ASR；VAD-lite（前 300ms 估底噪，说话后
 * 连续静音 VAD_SILENCE_MS 即收口）。返回喂入样本数 */
static size_t listen_capture(int64_t *speech_ms_out)
{
	static int16_t chunk[VAD_CHUNK];
	int64_t t0 = esp_timer_get_time();
	int64_t speech_at = 0;
	int64_t silence_at = 0;
	int noise = 0;
	int noise_n = 0;
	int thresh = 1500;
	size_t fed = 0;

	for (;;) {
		int64_t now = esp_timer_get_time();
		int64_t el = now - t0;
		int n;
		int r;

		if (el >= LISTEN_MAX_MS * 1000)
			break;
		n = aio_capture(chunk, VAD_CHUNK, 500);
		if (n <= 0)
			break;
		r = rms_i16(chunk, n);
		if (noise_n < 3) {
			noise += r;
			noise_n++;
			/* 底噪×4 自适应 + 800 底座，但封顶 3600：嘈杂房间
			 * （实测底噪 rms≈2646）不封顶会算出 >11k 的阈值，
			 * 比语音电平（≈4.5k）还高——VAD 永远不触发 */
			if (noise_n == 3)
				thresh = (noise / 3) * 4 + 800;
			if (thresh > 3600)
				thresh = 3600;
			continue;	/* 底噪估计段不喂（≈300ms） */
		}
		if (ks_asr_feed(chunk, n) < 0)
			break;
		fed += n;
		if (r > thresh) {
			if (!speech_at)
				speech_at = now;
			silence_at = 0;
		} else if (speech_at) {
			if (!silence_at)
				silence_at = now;
			if (el >= LISTEN_MIN_MS * 1000 &&
			    now - silence_at >= VAD_SILENCE_MS * 1000)
				break;
		}
	}
	if (speech_ms_out)
		*speech_ms_out = speech_at ?
				 (esp_timer_get_time() - speech_at) / 1000 : 0;
	return fed;
}

/* 等状态（含超时）；返回是否等到 */
static bool wait_state(enum convo_state st, uint32_t timeout_ms)
{
	int64_t deadline = esp_timer_get_time() +
			   (int64_t)timeout_ms * 1000;

	while (esp_timer_get_time() < deadline) {
		if (s_state == st)
			return true;
		vTaskDelay(pdMS_TO_TICKS(50));
	}
	return s_state == st;
}

/* 一轮：LISTENING→THINKING→SPEAKING，返回 0 或负错码（对齐菜单轮）。
 * speech_ms_out（可 NULL）带走 VAD 测得的实际说话时长 */
static int convo_round(int64_t *speech_ms_out)
{
	int64_t t_cap_end;
	int64_t speech_ms = 0;
	int rc = 0;

	s_text[0] = '\0';
	pump_reset();

	if (ks_asr_open(s_asr, &(struct ks_asr_events){
			.on_text = on_asr_text }) != 0) {
		vTaskDelay(pdMS_TO_TICKS(1000));	/* WS 偶发失联重试一次 */
		if (ks_asr_open(s_asr, &(struct ks_asr_events){
				.on_text = on_asr_text }) != 0)
			return -1;
	}
	listen_capture(&speech_ms);
	if (speech_ms_out)
		*speech_ms_out = speech_ms;
	t_cap_end = esp_timer_get_time();
	s_state = CONVO_THINKING;

	rc = ks_asr_finish(15000) == 0 ? 0 : -2;
	ks_asr_close();
	if (rc)
		return rc;
	if (!s_text[0])
		return -3;	/* 空 ASR：安静地回 IDLE */

	if (pump_tts_attach(s_tts, on_tts_pcm) != 0)
		return -5;
	s_llm->user_msg = s_text;
	rc = ks_llm_chat(s_llm, pump_on_llm_sentence, NULL, NULL);
	if (!pump_stop(30000))
		ESP_LOGW(TAG, "pump not done in 30s");

	s_state = CONVO_SPEAKING;
	/* 等播放缓冲排空（轮隔离，防下轮自采到上轮尾音） */
	for (int i = 0; i < 100 && pb_pending(); i++)
		vTaskDelay(pdMS_TO_TICKS(200));
	pump_tts_detach();
	if (rc != 0)
		return -4;
	(void)t_cap_end;
	return 0;
}

static void convo_task(void *arg)
{
	int ev;
	static int64_t t_last;

	(void)arg;
	for (;;) {
		xQueueReceive(s_wake_q, &ev, portMAX_DELAY);
		if (!s_on)
			continue;
		s_state = CONVO_LISTENING;
		{
			int64_t speech_ms = 0;
			int rc = convo_round(&speech_ms);
			int64_t el = t_last ? esp_timer_get_time() - t_last : 0;

			t_last = esp_timer_get_time();
			s_round_cnt++;
			printf("VL,CONVO,ROUND,n=%d,rc=%d,speech_ms=%lld,"
			       "asr_len=%d,el_ms=%lld,int=%uk,ps=%uk\r\n",
			       s_round_cnt, rc, speech_ms,
			       (int)strlen(s_text), el / 1000,
			       (unsigned)(heap_caps_get_free_size(
					  MALLOC_CAP_INTERNAL) / 1024),
			       (unsigned)(heap_caps_get_free_size(
					  MALLOC_CAP_SPIRAM) / 1024));
			fflush(stdout);
		}
		s_state = CONVO_IDLE;
	}
}

bool convo_start(struct ks_asr_cfg *asr, struct ks_tts_cfg *tts,
		 struct ks_llm_cfg *llm)
{
	static bool s_task_up;

	if (s_on)
		return true;
	if (!asr || !tts || !llm)
		return false;
	s_asr = asr;
	s_tts = tts;
	s_llm = llm;
	if (!s_wake_q) {
		s_wake_q = xQueueCreate(4, sizeof(int));
		if (!s_wake_q)
			return false;
	}
	if (!s_task_up) {
		/* 16384：与主任务同栈（convo_round 全程跑 ASR WS+TLS 与
		 * LLM SSE+TLS，V-43 首版给 8192 实测即栈保护故障） */
		if (xTaskCreatePinnedToCore(convo_task, "convo", 16384,
					    NULL, 5, NULL, 0) != pdPASS)
			return false;
		s_task_up = true;
	}
	s_on = true;
	s_state = CONVO_IDLE;
	return true;
}

void convo_stop(void)
{
	s_on = false;
	s_state = CONVO_OFF;
}

/* ---- 自驱回归（'C'）---- */

/* TTS 过空气说一句话（喊话/提问共用；IDLE 期使用，TTS 单例此刻无争用） */
static void speak_over_air(const char *text)
{
	if (ks_tts_open(s_tts, &(struct ks_tts_events){
			.on_pcm = on_tts_pcm }) != 0) {
		ESP_LOGE(TAG, "soak tts open fail");
		return;
	}
	ks_tts_feed(text, false, 15000);
	ks_tts_feed(NULL, true, 15000);
	ks_tts_close();
}

/* 内置提问：播 ~4.3s（首句"今天天气真不错……"），零网络依赖，
 * play_task 实时节奏出声 */
static void play_prompt_question(void)
{
	const uint8_t *p = (const void *)test_prompt_start;
	size_t total = 68800 * 2;	/* 4.3s @16k s16 */

	if (total > (size_t)(test_prompt_end - test_prompt_start))
		total = test_prompt_end - test_prompt_start;
	for (size_t off = 0; off < total; off += 3200) {
		size_t n = total - off;

		if (n > 3200)
			n = 3200;
		on_tts_pcm(NULL, p + off, n);
	}
}

static void soak_task(void *arg)
{
	int rounds = (int)(intptr_t)arg;
	int hits = 0;
	int synth = 0;
	int questions = 0;

	for (int k = 0; k < rounds && s_on; k++) {
		if (!wait_state(CONVO_IDLE, 15000)) {
			printf("VL,CONVO,SOAK,k=%d,st=timeout\r\n", k);
			fflush(stdout);
			continue;
		}
		vTaskDelay(pdMS_TO_TICKS(300));
		speak_over_air("你好小智");		/* 喊唤醒词 */
		if (!wait_state(CONVO_LISTENING, 8000)) {
			/* 声学 miss（环境噪声/音量所致）→ 合成唤醒兜底：
			 * 轮机制与声学检出解耦验证；命中数如实分列 */
			printf("VL,CONVO,SOAK,k=%d,wake=synth\r\n", k);
			fflush(stdout);
			convo_notify_wake();
			if (!wait_state(CONVO_LISTENING, 3000))
				continue;
			synth++;
		} else {
			hits++;
		}
		vTaskDelay(pdMS_TO_TICKS(400));		/* 让 beep/尾音过掉 */
		play_prompt_question();		/* 提问=内置语音（见上注） */
		questions++;
		wait_state(CONVO_IDLE, 60000);		/* 等本轮完成 */
		printf("VL,CONVO,SOAK,k=%d,done\r\n", k);
		fflush(stdout);
	}
	printf("VL,CONVO,SOAK,DONE,wake_hits=%d,synth=%d/%d,asked=%d\r\n",
	       hits, synth, rounds, questions);
	fflush(stdout);
	vTaskDelete(NULL);
}

int convo_soak_start(int rounds)
{
	if (!s_on || rounds <= 0)
		return 0;
	if (xTaskCreatePinnedToCore(soak_task, "soak", 10240,
				    (void *)(intptr_t)rounds, 4, NULL,
				    0) != pdPASS)
		return 0;
	return rounds;
}
