/*
 * sp_dashscope.c — DashScope（百炼）WS 语音客户端：ks_asr/ks_tts 的云直连实现
 *
 * 协议依据：docs/gates/V-33-百炼语音链路实测.md（§2 TTS/§3 ASR/§4 复刻）
 *  - 同一 wss 端点，run-task 的 task/function 区分 asr/tts
 *  - 文本帧 JSON（task-started/result-generated/task-finished/task-failed）
 *  - 二进制帧：ASR 上行 PCM / TTS 下行 PCM（s16le）
 *  - ASR 定稿标志 = output.sentence.sentence_end（V-33 §3，非 output.type）
 */
#include "kaleido_speech.h"

#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define KS_URI_PATH "/api-ws/v1/inference"
#define KS_TX_TIMEOUT_MS 8000
#define KS_PCM_CHUNK 3200 /* 100ms @16k s16le mono */

static const char *TAG = "kaleido_speech";

/* ---- 公共小件 ---- */

static void task_id_gen(char out[33])
{
	uint8_t rnd[16];

	esp_fill_random(rnd, sizeof(rnd));
	for (int i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02x", rnd[i]);
	out[32] = '\0';
}

/*
 * 组 run-task 帧。kind: "asr"→recognition（format/sample_rate）
 * 或 "tts"→SpeechSynthesizer（voice/format/sample_rate）。
 */
static char *run_task_build(const char *task_id, const char *model,
			    const char *voice, int sample_rate)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *hdr = cJSON_CreateObject();
	cJSON *pl = cJSON_CreateObject();
	cJSON *par = cJSON_CreateObject();
	cJSON *in = cJSON_CreateObject();
	char *s;
	bool tts = (voice != NULL);

	cJSON_AddStringToObject(hdr, "action", "run-task");
	cJSON_AddStringToObject(hdr, "task_id", task_id);
	cJSON_AddStringToObject(hdr, "streaming", "duplex");

	cJSON_AddStringToObject(pl, "task_group", "audio");
	cJSON_AddStringToObject(pl, "task", tts ? "tts" : "asr");
	cJSON_AddStringToObject(pl, "function",
				tts ? "SpeechSynthesizer" : "recognition");
	cJSON_AddStringToObject(pl, "model", model);

	cJSON_AddStringToObject(par, "format", "pcm");
	cJSON_AddNumberToObject(par, "sample_rate", sample_rate);
	if (tts) {
		cJSON_AddStringToObject(par, "text_type", "PlainText");
		cJSON_AddStringToObject(par, "voice", voice);
		cJSON_AddNumberToObject(par, "volume", 50);
		cJSON_AddNumberToObject(par, "rate", 1);
		cJSON_AddNumberToObject(par, "pitch", 1);
	}
	cJSON_AddItemToObject(pl, "parameters", par);
	/* run-task 必带空 input 对象（V-33 asr_probe 实证；缺此字段
	 * 服务端报 InvalidParameter: Missing required parameter 'payload.input'） */
	cJSON_AddItemToObject(pl, "input", in);
	cJSON_AddItemToObject(root, "header", hdr);
	cJSON_AddItemToObject(root, "payload", pl);

	s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return s;
}

static char *cont_task_build(const char *task_id, const char *text)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *hdr = cJSON_CreateObject();
	cJSON *pl = cJSON_CreateObject();
	cJSON *in = cJSON_CreateObject();
	char *s;

	cJSON_AddStringToObject(hdr, "action", "continue-task");
	cJSON_AddStringToObject(hdr, "task_id", task_id);
	cJSON_AddStringToObject(hdr, "streaming", "duplex");
	if (text)
		cJSON_AddStringToObject(in, "text", text);
	cJSON_AddItemToObject(pl, "input", in);
	cJSON_AddItemToObject(root, "header", hdr);
	cJSON_AddItemToObject(root, "payload", pl);

	s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return s;
}

static char *fin_task_build(const char *task_id)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *hdr = cJSON_CreateObject();
	cJSON *pl = cJSON_CreateObject();
	char *s;

	cJSON_AddStringToObject(hdr, "action", "finish-task");
	cJSON_AddStringToObject(hdr, "task_id", task_id);
	cJSON_AddStringToObject(hdr, "streaming", "duplex");
	cJSON_AddItemToObject(pl, "input", cJSON_CreateObject());
	cJSON_AddItemToObject(root, "header", hdr);
	cJSON_AddItemToObject(root, "payload", pl);

	s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return s;
}

static int json_event_parse(const char *data, size_t len,
			    char *event, size_t event_sz,
			    char *sentence, size_t sent_sz,
			    bool *sentence_end,
			    char *err, size_t err_sz)
{
	cJSON *root = cJSON_ParseWithLength(data, len);
	const cJSON *h, *e, *o, *sen;
	int rc = -1;

	if (!root)
		return -1;
	h = cJSON_GetObjectItem(root, "header");
	if (!h)
		goto out;
	e = cJSON_GetObjectItem(h, "event");
	if (!e || !cJSON_IsString(e))
		goto out;
	strlcpy(event, e->valuestring, event_sz);

	o = cJSON_GetObjectItem(root, "payload");
	if (o)
		o = cJSON_GetObjectItem(o, "output");
	if (o) {
		sen = cJSON_GetObjectItem(o, "sentence");
		if (sen) {
			const cJSON *t, *se;

			t = cJSON_GetObjectItem(sen, "text");
			if (t && cJSON_IsString(t))
				strlcpy(sentence, t->valuestring,
					sent_sz);
			se = cJSON_GetObjectItem(sen, "sentence_end");
			*sentence_end = cJSON_IsTrue(se);
		}
	}
	if (strcmp(event, "task-failed") == 0) {
		const cJSON *c = cJSON_GetObjectItem(h, "error_code");
		const cJSON *m = cJSON_GetObjectItem(h, "error_message");

		snprintf(err, err_sz, "%s: %s",
			 c && cJSON_IsString(c) ? c->valuestring : "?",
			 m && cJSON_IsString(m) ? m->valuestring : "?");
	}
	rc = 0;
out:
	cJSON_Delete(root);
	return rc;
}

/* ---- ASR 会话 ---- */

struct ks_asr_sess {
	char task_id[33];
	esp_websocket_client_handle_t ws;
	SemaphoreHandle_t connected;
	SemaphoreHandle_t started;
	SemaphoreHandle_t finished;
	struct ks_asr_events ev;
	uint8_t pend[KS_PCM_CHUNK];
	size_t pend_len;
	bool failed;
	bool done;
	bool closing;	/* V-41：关闭期事件直接丢弃（UAF 竞态防御） */
};

static struct ks_asr_sess *s_asr;

static void asr_ws_event(void *arg, esp_event_base_t base, int32_t id,
			 void *data)
{
	esp_websocket_event_data_t *e = data;
	char event[24] = "", sentence[512] = "", err[160] = "";
	bool send_end = false;

	/* 实测（V-41）：恶劣网况下 close 超时后 destroy，客户端任务仍在
	 * 派发残余事件 → 对已 free 会话取字段 → PSRAM 指针当代码跳 */
	if (!s_asr || s_asr->closing)
		return;
	if (id == WEBSOCKET_EVENT_CONNECTED) {
		xSemaphoreGive(s_asr->connected);
		return;
	}
	if (id == WEBSOCKET_EVENT_DISCONNECTED) { /* G3：快失败 */
		s_asr->failed = true;
		xSemaphoreGive(s_asr->started);
		xSemaphoreGive(s_asr->finished);
		return;
	}
	if (id != WEBSOCKET_EVENT_DATA || !e->data_ptr || !e->data_len)
		return;
	if (e->op_code != WS_TRANSPORT_OPCODES_TEXT)
		return;
	if (json_event_parse(e->data_ptr, e->data_len, event,
			     sizeof(event), sentence, sizeof(sentence),
			     &send_end, err, sizeof(err)) < 0)
		return;

	if (strcmp(event, "task-started") == 0) {
		xSemaphoreGive(s_asr->started);
	} else if (strcmp(event, "result-generated") == 0) {
		if (s_asr->ev.on_text && sentence[0])
			s_asr->ev.on_text(s_asr->ev.user, sentence,
					  send_end);
	} else if (strcmp(event, "task-finished") == 0) {
		s_asr->done = true;
		xSemaphoreGive(s_asr->finished);
	} else if (strcmp(event, "task-failed") == 0) {
		ESP_LOGE(TAG, "asr task-failed: %s", err);
		s_asr->failed = true;
		xSemaphoreGive(s_asr->finished);
		xSemaphoreGive(s_asr->started);
	}
}

int ks_asr_open(const struct ks_asr_cfg *cfg,
		const struct ks_asr_events *ev)
{
	char auth[192];
	char *run;
	esp_websocket_client_config_t wc = {
		.buffer_size = 4096,
		.network_timeout_ms = 10000,
		.crt_bundle_attach = esp_crt_bundle_attach,
	};

	if (s_asr)
		return -1;
	s_asr = calloc(1, sizeof(*s_asr));
	if (!s_asr)
		return -1;
	s_asr->ev = ev ? *ev :
		    (struct ks_asr_events){ 0 };
	task_id_gen(s_asr->task_id);
	s_asr->connected = xSemaphoreCreateBinary();
	s_asr->started = xSemaphoreCreateBinary();
	s_asr->finished = xSemaphoreCreateBinary();

	/* uri 去尾部路径，拼标准 inference 端点（容错首尾 '/'） */
	{
		char host[192];
		size_t n = strlen(cfg->url);

		while (n && cfg->url[n - 1] == '/')
			n--;
		strlcpy(host, cfg->url, n + 1 > sizeof(host) ?
			sizeof(host) : n + 1);
		strlcat(host, KS_URI_PATH, sizeof(host));
		wc.uri = host;
	}
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
		 cfg->api_key);
	wc.headers = auth;

	s_asr->ws = esp_websocket_client_init(&wc);
	if (!s_asr->ws)
		goto fail;
	ESP_ERROR_CHECK(esp_websocket_register_events(
		s_asr->ws, WEBSOCKET_EVENT_ANY, asr_ws_event, NULL));
	if (esp_websocket_client_start(s_asr->ws) != ESP_OK)
		goto fail;
	if (!xSemaphoreTake(s_asr->connected, pdMS_TO_TICKS(10000))) {
		ESP_LOGE(TAG, "asr ws connect timeout");
		goto fail;
	}

	run = run_task_build(s_asr->task_id, cfg->model, NULL,
			     cfg->sample_rate);
	int rc = esp_websocket_client_send_text(s_asr->ws, run,
						strlen(run),
						KS_TX_TIMEOUT_MS);
	free(run);
	if (rc < 0)
		goto fail;

	/* 等 task-started（服务端就绪）；失败路径由 started 兜底 give */
	if (!xSemaphoreTake(s_asr->started, pdMS_TO_TICKS(10000))) {
		ESP_LOGE(TAG, "asr task-started timeout");
		goto fail;
	}
	if (s_asr->failed)
		goto fail;
	return 0;
fail:
	ks_asr_close();
	return -2;
}

int ks_asr_feed(const int16_t *pcm, size_t samples)
{
	const uint8_t *p = (const uint8_t *)pcm;
	size_t len = samples * 2;

	if (!s_asr)
		return -2;
	while (len) {
		size_t n = KS_PCM_CHUNK - s_asr->pend_len;

		if (n > len)
			n = len;
		memcpy(s_asr->pend + s_asr->pend_len, p, n);
		s_asr->pend_len += n;
		p += n;
		len -= n;
		if (s_asr->pend_len == KS_PCM_CHUNK) {
			if (esp_websocket_client_send_bin(
				    s_asr->ws, (const char *)s_asr->pend,
				    KS_PCM_CHUNK,
				    KS_TX_TIMEOUT_MS) < 0)
				return -3;
			s_asr->pend_len = 0;
		}
	}
	return samples;
}

int ks_asr_finish(uint32_t timeout_ms)
{
	char *f;
	int rc = 0;

	if (!s_asr)
		return -2;
	/* 尾片不足 100ms 也补发 */
	if (s_asr->pend_len &&
	    esp_websocket_client_send_bin(s_asr->ws,
					  (const char *)s_asr->pend,
					  s_asr->pend_len,
					  KS_TX_TIMEOUT_MS) < 0)
		return -3;
	s_asr->pend_len = 0;
	f = fin_task_build(s_asr->task_id);
	rc = esp_websocket_client_send_text(s_asr->ws, f, strlen(f),
					    KS_TX_TIMEOUT_MS);
	free(f);
	if (rc < 0)
		return -3;
	if (!xSemaphoreTake(s_asr->finished,
			    pdMS_TO_TICKS(timeout_ms))) {
		ESP_LOGE(TAG, "asr finish timeout");
		return -4;
	}
	return s_asr->failed ? -5 : 0;
}

void ks_asr_close(void)
{
	if (!s_asr)
		return;
	s_asr->closing = true;
	if (s_asr->ws) {
		esp_websocket_client_close(s_asr->ws,
					   pdMS_TO_TICKS(2000));
		vTaskDelay(pdMS_TO_TICKS(50));
		esp_websocket_client_destroy(s_asr->ws);
	}
	vSemaphoreDelete(s_asr->connected);
	vSemaphoreDelete(s_asr->started);
	vSemaphoreDelete(s_asr->finished);
	free(s_asr);
	s_asr = NULL;
}

/* ---- TTS 会话 ----
 * V-41：TTS 可切 MiniMax（t2a_v2，见 sp_minimax.c）；DashScope 实现整段编译隔离 */
#if !CONFIG_KS_TTS_PROVIDER_MINIMAX

/* ---- TTS 会话 ---- */

struct ks_tts_sess {
	char task_id[33];
	esp_websocket_client_handle_t ws;
	SemaphoreHandle_t connected;
	SemaphoreHandle_t started;
	SemaphoreHandle_t finished;
	struct ks_tts_events ev;
	int64_t first_pcm_us;
	int64_t feed_us;
	bool failed;
	bool done;
};

static struct ks_tts_sess *s_tts;

static void tts_ws_event(void *arg, esp_event_base_t base, int32_t id,
			 void *data)
{
	esp_websocket_event_data_t *e = data;
	char event[24] = "", sentence[8] = "", err[160] = "";
	bool dummy = false;

	if (id == WEBSOCKET_EVENT_CONNECTED) {
		xSemaphoreGive(s_tts->connected);
		return;
	}
	if (id == WEBSOCKET_EVENT_DISCONNECTED) { /* G3：快失败 */
		s_tts->failed = true;
		xSemaphoreGive(s_tts->started);
		xSemaphoreGive(s_tts->finished);
		return;
	}
	if (id != WEBSOCKET_EVENT_DATA || !e->data_ptr || !e->data_len)
		return;
	if (e->op_code == WS_TRANSPORT_OPCODES_BINARY) {
		if (!s_tts->first_pcm_us)
			s_tts->first_pcm_us = esp_timer_get_time();
		if (s_tts->ev.on_pcm)
			s_tts->ev.on_pcm(s_tts->ev.user,
					 (const uint8_t *)e->data_ptr,
					 e->data_len);
		return;
	}
	if (e->op_code != WS_TRANSPORT_OPCODES_TEXT)
		return;
	if (json_event_parse(e->data_ptr, e->data_len, event,
			     sizeof(event), sentence, sizeof(sentence),
			     &dummy, err, sizeof(err)) < 0)
		return;

	if (strcmp(event, "task-started") == 0) {
		xSemaphoreGive(s_tts->started);
	} else if (strcmp(event, "task-finished") == 0) {
		s_tts->done = true;
		xSemaphoreGive(s_tts->finished);
	} else if (strcmp(event, "task-failed") == 0) {
		ESP_LOGE(TAG, "tts task-failed: %s", err);
		s_tts->failed = true;
		xSemaphoreGive(s_tts->finished);
		xSemaphoreGive(s_tts->started);
	}
}

int ks_tts_open(const struct ks_tts_cfg *cfg,
		const struct ks_tts_events *ev)
{
	char auth[192];
	char *run;
	esp_websocket_client_config_t wc = {
		.buffer_size = 4096,
		.network_timeout_ms = 10000,
		.crt_bundle_attach = esp_crt_bundle_attach,
	};

	if (s_tts)
		return -1;
	s_tts = calloc(1, sizeof(*s_tts));
	if (!s_tts)
		return -1;
	s_tts->ev = ev ? *ev : (struct ks_tts_events){ 0 };
	task_id_gen(s_tts->task_id);
	s_tts->connected = xSemaphoreCreateBinary();
	s_tts->started = xSemaphoreCreateBinary();
	s_tts->finished = xSemaphoreCreateBinary();

	{
		char host[192];
		size_t n = strlen(cfg->url);

		while (n && cfg->url[n - 1] == '/')
			n--;
		strlcpy(host, cfg->url, n + 1 > sizeof(host) ?
			sizeof(host) : n + 1);
		strlcat(host, KS_URI_PATH, sizeof(host));
		wc.uri = host;
	}
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
		 cfg->api_key);
	wc.headers = auth;

	s_tts->ws = esp_websocket_client_init(&wc);
	if (!s_tts->ws)
		goto fail;
	ESP_ERROR_CHECK(esp_websocket_register_events(
		s_tts->ws, WEBSOCKET_EVENT_ANY, tts_ws_event, NULL));
	if (esp_websocket_client_start(s_tts->ws) != ESP_OK)
		goto fail;
	if (!xSemaphoreTake(s_tts->connected, pdMS_TO_TICKS(10000))) {
		ESP_LOGE(TAG, "tts ws connect timeout");
		goto fail;
	}

	run = run_task_build(s_tts->task_id, cfg->model, cfg->voice,
			     cfg->sample_rate);
	int rc = esp_websocket_client_send_text(s_tts->ws, run,
						strlen(run),
						KS_TX_TIMEOUT_MS);
	free(run);
	if (rc < 0)
		goto fail;
	if (!xSemaphoreTake(s_tts->started, pdMS_TO_TICKS(10000))) {
		ESP_LOGE(TAG, "tts task-started timeout");
		goto fail;
	}
	if (s_tts->failed)
		goto fail;
	return 0;
fail:
	ks_tts_close();
	return -2;
}

int ks_tts_feed(const char *text, bool last, uint32_t timeout_ms)
{
	char *f;
	int rc = 0;

	if (!s_tts)
		return -2;
	if (!s_tts->feed_us)
		s_tts->feed_us = esp_timer_get_time();
	if (text && text[0]) {
		f = cont_task_build(s_tts->task_id, text);
		rc = esp_websocket_client_send_text(s_tts->ws, f,
						    strlen(f),
						    KS_TX_TIMEOUT_MS);
		free(f);
		if (rc < 0)
			return -3;
	}
	if (last) {
		f = fin_task_build(s_tts->task_id);
		rc = esp_websocket_client_send_text(s_tts->ws, f,
						    strlen(f),
						    KS_TX_TIMEOUT_MS);
		free(f);
		if (rc < 0)
			return -3;
		if (!xSemaphoreTake(s_tts->finished,
				    pdMS_TO_TICKS(timeout_ms))) {
			ESP_LOGE(TAG, "tts finish timeout");
			return -4;
		}
		return s_tts->failed ? -5 : 0;
	}
	return 0;
}

int64_t ks_tts_first_pcm_us(void)
{
	if (!s_tts || !s_tts->first_pcm_us)
		return -1;                  /* G4：与头文件一致     */
	return s_tts->first_pcm_us;
}

void ks_tts_close(void)
{
	if (!s_tts)
		return;
	if (s_tts->ws) {
		esp_websocket_client_close(s_tts->ws,
					   pdMS_TO_TICKS(2000));
		esp_websocket_client_destroy(s_tts->ws);
	}
	vSemaphoreDelete(s_tts->connected);
	vSemaphoreDelete(s_tts->started);
	vSemaphoreDelete(s_tts->finished);
	free(s_tts);
	s_tts = NULL;
}
#endif  /* !CONFIG_KS_TTS_PROVIDER_MINIMAX */
