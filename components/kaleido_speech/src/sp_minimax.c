/*
 * sp_minimax.c — MiniMax t2a_v2 流式 TTS（V-41 切换）
 *
 * HTTPS + SSE，音频以 hex 编码嵌在 data 事件里；按子句一请求（无会话
 * 状态，天然避开 cosyvoice 的 continue/run-task 续喂坑）。
 * 流式 hex 解码：不缓冲整行（单行可达 64KB+），标记串 "\"audio\":\""
 * 之后逐对 nibble 还原 PCM 即时下发 on_pcm。
 */
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "kaleido_speech.h"

#if CONFIG_KS_TTS_PROVIDER_MINIMAX

static const char *TAG = "ks-mm";

#define MM_HOST		"https://api.minimaxi.com"
#define MM_HEXMARK	"\"audio\":\""

static struct {
	const struct ks_tts_cfg *cfg;
	struct ks_tts_events ev;
	char gid[48];
	int64_t first_pcm_us;
} s_mm;

/* JWT payload 自解析 GroupID（key 形如 xxxxx.yyyyy.zzzzz） */
int ks_mm_group_id(const char *jwt, char *out, size_t n)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	const char *seg = strchr(jwt, '.');
	char dec[512];
	size_t di = 0;
	int acc = 0, bits = 0;

	if (!seg)
		return -1;
	seg++;	/* payload 段（第 1 个点之后到第 2 个点） */
	{
		size_t len = strchr(seg, '.') - seg;

		if (len >= sizeof(dec) * 4 / 3)
			return -1;
		for (size_t i = 0; i < len; i++) {
			const char *v = strchr(b64, seg[i]);
			int d = v ? (int)(v - b64) : 0;

			if (!v)
				continue;
			acc = (acc << 6) | d;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				dec[di++] = (char)((acc >> bits) & 0xFF);
			}
		}
		dec[di] = '\0';
	}
	const char *p = strstr(dec, "\"GroupID\":\"");

	if (!p)
		return -1;
	p += strlen("\"GroupID\":\"");
	size_t l = 0;

	while (p[l] && p[l] != '"' && l + 1 < n)
		l++;
	if (!l)
		return -1;
	memcpy(out, p, l);
	out[l] = '\0';
	return 0;
}

/* SSE 流状态机（逐字节）
 * t2a_v2 事件形态：{"data":{"audio":"<增量hex>","status":1..}
 * 末事件 status=2 的 audio 是【全量重复】（实测 hex 长度=增量之和），
 * 必须丢弃——按字段暂存，见 status 才决定下发/丢弃 */
struct mm_parse {
	bool in_hex;		/* 处于 audio hex 字段        */
	int nib;		/* 待配对高半字节，-1 无      */
	size_t mark;		/* 标记串匹配进度             */
	size_t smark;		/* "status": 标记匹配进度     */
	uint8_t *pend;		/* 当前字段解码暂存           */
	size_t pend_len, pend_cap;
	bool done;		/* [DONE]                     */
	int64_t t_first;
};

#define MM_STATMARK	"\"status\":"

static void mm_emit_pending(struct mm_parse *ps)
{
	if (!ps->pend_len)
		return;
	if (ps->t_first < 0) {
		ps->t_first = esp_timer_get_time();
		s_mm.first_pcm_us = ps->t_first;
	}
	s_mm.ev.on_pcm(s_mm.ev.user, ps->pend, ps->pend_len);
	ps->pend_len = 0;
}

static int nib_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void mm_byte(struct mm_parse *ps, char ch)
{
	if (ps->in_hex) {
		if (ch == '"') {	/* 字段结束，待 status 裁决 */
			ps->nib = -1;
			ps->in_hex = false;
			ps->mark = 0;
			ps->smark = 0;
			return;
		}
		int v = nib_val(ch);

		if (v < 0)
			return;		/* 非法字符跳过 */
		if (ps->nib < 0) {
			ps->nib = v;
			return;
		}
		if (ps->pend_len + 4 > ps->pend_cap) {
			size_t nc = ps->pend_cap ? ps->pend_cap * 2 : 8192;

			if (nc > 262144)
				nc = 262144;
			uint8_t *nb = heap_caps_realloc(ps->pend, nc,
							MALLOC_CAP_SPIRAM);
			if (!nb) {	/* 撑不住：立即下发腾空间 */
				mm_emit_pending(ps);
				nc = ps->pend_cap;
			} else {
				ps->pend = nb;
				ps->pend_cap = nc;
			}
			if (ps->pend_len + 4 > ps->pend_cap) {
				ps->nib = -1;
				return;	/* 极端无内存：丢字节 */
			}
		}
		ps->pend[ps->pend_len++] = (uint8_t)((ps->nib << 4) | v);
		ps->nib = -1;
		return;
	}
	/* "status": 标记匹配（audio 字段之后到达） */
	if (ch == MM_STATMARK[ps->smark]) {
		ps->smark++;
		if (!MM_STATMARK[ps->smark]) {
			ps->smark = 0;
			/* 下一个非空白字符是裁决值 */
			ps->pend_len = ps->pend_len; /* 状态等待在下字节 */
			ps->in_hex = false;
			/* 用 nib<0 哨兵 + 特判：直接看下一字节 */
			/* 简化：把裁决延迟到下一字节处理 */
			ps->nib = -2;	/* -2 = 下一字节是 status 值 */
			return;
		}
	} else {
		ps->smark = (ch == MM_STATMARK[0]) ? 1 : 0;
	}
	if (ps->nib == -2) {
		/* 上一轮匹配完 "status":，本字节是值 */
		if (ch != '2')
			mm_emit_pending(ps);	/* 1 等：增量块下发 */
		ps->pend_len = 0;	/* 2：全量重复丢弃 */
		ps->nib = -1;
		/* 值字符也可能开启新标记，交给下方标记逻辑 */
	}
	/* audio 标记串匹配（含跨 chunk 残留） */
	if (ch == MM_HEXMARK[ps->mark]) {
		ps->mark++;
		if (!MM_HEXMARK[ps->mark]) {
			ps->in_hex = true;
			ps->nib = -1;
			ps->mark = 0;
		}
	} else {
		ps->mark = (ch == MM_HEXMARK[0]) ? 1 : 0;
	}
}

int ks_tts_open(const struct ks_tts_cfg *cfg,
		const struct ks_tts_events *ev)
{
	const char *key = cfg->api_key;

	memset(&s_mm, 0, sizeof(s_mm));
	s_mm.cfg = cfg;
	s_mm.ev = *ev;
	s_mm.first_pcm_us = 0;
	if (!key || !key[0]) {
		ESP_LOGE(TAG, "no api key (CONFIG_KS_MM_KEY)");
		return -1;
	}
	if (ks_mm_group_id(key, s_mm.gid, sizeof(s_mm.gid)) != 0) {
		ESP_LOGE(TAG, "parse GroupID from jwt fail");
		return -1;
	}
	ESP_LOGI(TAG, "minimax tts ready model=%s voice=%s",
		 cfg->model, cfg->voice);
	return 0;
}

int ks_tts_feed(const char *text, bool last, uint32_t timeout_ms)
{
	char url[160];
	char auth[768];
	char *body;
	cJSON *root;
	int ret = 0;

	if (!text || !text[0])
		return last ? 0 : -1;	/* flush 语义：无会话状态直接过 */

	snprintf(url, sizeof(url), MM_HOST "/v1/t2a_v2?GroupId=%s",
		 s_mm.gid);
	snprintf(auth, sizeof(auth), "Bearer %s", s_mm.cfg->api_key);

	root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", s_mm.cfg->model);
	cJSON_AddStringToObject(root, "text", text);
	cJSON_AddBoolToObject(root, "stream", true);
	{
		cJSON *vs = cJSON_AddObjectToObject(root, "voice_setting");

		cJSON_AddStringToObject(vs, "voice_id", s_mm.cfg->voice);
	}
	{
		cJSON *as = cJSON_AddObjectToObject(root, "audio_setting");

		cJSON_AddNumberToObject(as, "sample_rate",
					s_mm.cfg->sample_rate);
		cJSON_AddNumberToObject(as, "bit_rate", 32000);
		cJSON_AddStringToObject(as, "format", "pcm");
		cJSON_AddNumberToObject(as, "channel", 1);
	}
	{
		char *j = cJSON_PrintUnformatted(root);

		body = malloc(strlen(j) + 1);
		strcpy(body, j);
		cJSON_free(j);
	}
	cJSON_Delete(root);

	esp_http_client_config_t hc = {
		.url = url,
		.method = HTTP_METHOD_POST,
		.timeout_ms = timeout_ms,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.buffer_size = 2048,
		.buffer_size_tx = 4096,
	};
	esp_http_client_handle_t cl = esp_http_client_init(&hc);

	esp_http_client_set_header(cl, "Authorization", auth);
	esp_http_client_set_header(cl, "Content-Type",
				   "application/json");
	if (esp_http_client_open(cl, strlen(body)) != ESP_OK ||
	    esp_http_client_write(cl, body, strlen(body)) < 0) {
		esp_http_client_cleanup(cl);
		free(body);
		return -2;
	}
	free(body);
	esp_http_client_fetch_headers(cl);

	struct mm_parse ps = {
		.nib = -1,
		.t_first = -1,
		.pend = NULL,
		.pend_len = 0,
		.pend_cap = 0,
		.mark = 0,
		.smark = 0,
		.in_hex = false,
		.done = false,
	};
	char buf[1024];
	int n;

	while ((n = esp_http_client_read(cl, buf, sizeof(buf))) >= 0) {
		if (n == 0) {
			if (esp_http_client_is_complete_data_received(cl))
				break;
			continue;
		}
		for (int i = 0; i < n; i++)
			mm_byte(&ps, buf[i]);
		if (ps.done)
			break;
	}
	mm_emit_pending(&ps);	/* 无 status 收尾：防御性下发 */
	free(ps.pend);
	int code = esp_http_client_get_status_code(cl);

	esp_http_client_cleanup(cl);
	if (code != 200) {
		ESP_LOGE(TAG, "t2a http %d", code);
		ret = -3;
	}
	return ret;
}

int64_t ks_tts_first_pcm_us(void)
{
	return s_mm.first_pcm_us ? s_mm.first_pcm_us : -1;
}

void ks_tts_close(void)
{
	/* 无会话状态：空实现 */
}

#endif  /* CONFIG_KS_TTS_PROVIDER_MINIMAX */
