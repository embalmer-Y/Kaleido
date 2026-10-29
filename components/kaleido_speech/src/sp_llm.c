/*
 * sp_llm.c — 流式 chat/completions（OpenAI 兼容 SSE）客户端
 *
 * 从 voice-loop-p4 HIL 的 llm_stream.c 升格入组件（V-41 后 LLM 与
 * ASR/TTS 同属 provider 层，M2 对话状态机直接复用）。
 *
 * 协议事实（V-41 实测，MiniMax chatcompletion_v2 关思考模式）：
 *  - SSE 全程只有 ~4 行，末行 data 事件把整段回复+usage 一次打包，
 *    单行可达 700B+（长回复更长）——行缓冲必须 ≥2048 并能容忍截断。
 *  - thinking 字段带上 disable 后才有上述行为，默认思考模式每行小包。
 *
 * 句读切送：delta 原样攒句，遇 。？！；\n（cut_clauses 再加 ，、）
 * 即回调一次，供"首句即送 TTS"流水线；容量冲刷只在 UTF-8 字符边界
 * 触发——中文 3 字节序列被拦腰截断会让下游 TTS 收到非法 UTF-8。
 */
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kaleido_speech.h"
#include "sp_internal.h"

#define MM_LLM_HOST	"https://api.minimaxi.com"
#define MM_LLM_URLFMT	MM_LLM_HOST "/v1/text/chatcompletion_v2?GroupId=%s"

static const char *TAG = "ks-llm";

struct ks_llm_ctx {
	ks_llm_sent_cb cb;
	void *user;
	int64_t t0;
	int64_t first_token_us;
	char pend[2048];	/* 跨 chunk 的 SSE 残行缓冲      */
	size_t pend_len;
	char sent[512];		/* 攒句缓冲（子句上限，配合 HIL
				 * 512B 队列项使用）              */
	size_t sent_len;
	int cont;		/* UTF-8 待续字节数（0=字符边界） */
	bool cut_clauses;
	bool abort;
	int status;
};

/* ---- MiniMax JWT → GroupID → chatcompletion_v2 URL ---- */

int sp_mm_group_id(const char *jwt, char *out, size_t n)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	const char *seg = strchr(jwt, '.');
	const char *dot2;
	const char *p;
	char dec[512];
	size_t di = 0;
	size_t len;
	int acc = 0, bits = 0;
	size_t l = 0;

	if (!jwt || !out || !n)
		return -1;
	if (!seg)
		return -1;
	seg++;		/* payload 段：第 1 个点之后到第 2 个点（无第 2 个点
			 * 视为到串尾，兼容无签名 token） */
	dot2 = strchr(seg, '.');
	len = dot2 ? (size_t)(dot2 - seg) : strlen(seg);

	/* payload 解码上限 512B（JWT payload 实测 ~200B，GroupID 必在其中） */
	if (len >= sizeof(dec) * 4 / 3)
		return -1;
	for (size_t i = 0; i < len; i++) {
		const char *v = strchr(b64, seg[i]);
		int d = v ? (int)(v - b64) : 0;

		if (!v)
			continue;	/* 非法字符（含 '=' 填充）跳过 */
		acc = (acc << 6) | d;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			dec[di++] = (char)((acc >> bits) & 0xFF);
		}
	}
	dec[di] = '\0';

	p = strstr(dec, "\"GroupID\":\"");
	if (!p)
		return -1;
	p += strlen("\"GroupID\":\"");
	while (p[l] && p[l] != '"' && l + 1 < n)
		l++;
	if (p[l] != '"' && p[l] != '\0')
		return -2;	/* 未闭合即超容量：截断串拼 URL 只会 404 */
	if (!l)
		return -1;
	memcpy(out, p, l);
	out[l] = '\0';
	return 0;
}

int ks_llm_minimax_url(char *buf, size_t n, const char *jwt)
{
	char gid[48];
	int w;

	if (!buf || n == 0)
		return -1;
	if (sp_mm_group_id(jwt, gid, sizeof(gid)) != 0)
		return -1;
	w = snprintf(buf, n, MM_LLM_URLFMT, gid);
	return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

/* ---- 句读切送 ---- */

static void emit(struct ks_llm_ctx *c, const char *delta, size_t n)
{
	/* 句读 UTF-8 序列：。？！；＋子句级 ，、 */
	static const unsigned char pun_end[3] = { 0xe3, 0x80, 0x82 };
	static const unsigned char pun_q[3] = { 0xef, 0xbc, 0x9f };
	static const unsigned char pun_e[3] = { 0xef, 0xbc, 0x81 };
	static const unsigned char pun_s[3] = { 0xef, 0xbc, 0x9b };
	static const unsigned char pun_c[3] = { 0xef, 0xbc, 0x8c };
	static const unsigned char pun_d[3] = { 0xe3, 0x80, 0x81 };

	for (size_t i = 0; i < n; i++) {
		unsigned char ch = delta[i];

		/* 容量冲刷只在字符边界：留 6B 余量容纳一个 4 字节字符
		 *（UTF-8 最长）+ NUL，绝不切开多字节序列 */
		if (c->cont == 0 && c->sent_len + 6 > sizeof(c->sent)) {
			c->sent[c->sent_len] = '\0';
			if (!c->cb(c->user, c->sent, false))
				c->abort = true;
			c->sent_len = 0;
		}
		c->sent[c->sent_len++] = ch;

		/* 维护"处于多字节字符中间"状态（续字节 = 0b10xxxxxx） */
		if ((ch & 0xC0) != 0x80) {
			if (ch < 0x80)
				c->cont = 0;
			else if ((ch & 0xE0) == 0xC0)
				c->cont = 1;
			else if ((ch & 0xF0) == 0xE0)
				c->cont = 2;
			else if ((ch & 0xF8) == 0xF0)
				c->cont = 3;
			else
				c->cont = 0;	/* 非法序列按独立字节处理 */
		} else if (c->cont > 0) {
			c->cont--;
		}

		bool cut = (ch == '\n');
		const unsigned char *t =
			(const unsigned char *)c->sent + c->sent_len - 3;

		if (!cut && c->sent_len >= 3)
			cut = !memcmp(t, pun_end, 3) ||
			      !memcmp(t, pun_q, 3) ||
			      !memcmp(t, pun_e, 3) ||
			      !memcmp(t, pun_s, 3) ||
			      (c->cut_clauses &&
			       (!memcmp(t, pun_c, 3) || !memcmp(t, pun_d, 3)));
		if (cut) {
			c->sent[c->sent_len] = '\0';
			if (!c->cb(c->user, c->sent, false))
				c->abort = true;
			c->sent_len = 0;
		}
	}
	if (c->abort)
		ESP_LOGW(TAG, "consumer aborted stream");
}

/* ---- SSE 行解析 ---- */

static void handle_line(struct ks_llm_ctx *c, const char *line, size_t len)
{
	static const char pfx[] = "data: ";
	cJSON *root, *ch, *delta, *content;
	const char *s;

	if (len <= sizeof(pfx) - 1 ||
	    strncmp(line, pfx, sizeof(pfx) - 1) != 0)
		return;
	s = line + sizeof(pfx) - 1;
	if (strncmp(s, "[DONE]", 6) == 0)
		return;
	root = cJSON_ParseWithLength(s, len - (sizeof(pfx) - 1));
	if (!root)
		return;
	ch = cJSON_GetObjectItem(root, "choices");
	if (ch)
		ch = cJSON_GetArrayItem(ch, 0);
	delta = ch ? cJSON_GetObjectItem(ch, "delta") : NULL;
	content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
	if (content && cJSON_IsString(content) && content->valuestring[0]) {
		if (!c->first_token_us)
			c->first_token_us = esp_timer_get_time() - c->t0;
		emit(c, content->valuestring, strlen(content->valuestring));
	}
	cJSON_Delete(root);
}

/* 读循环事件回调：HTTP chunk → 按 \n 切行 → handle_line */
static esp_err_t http_event(esp_http_client_event_t *ev)
{
	struct ks_llm_ctx *c = ev->user_data;

	if (ev->event_id != HTTP_EVENT_ON_DATA)
		return ESP_OK;
	if (c->status == 0)
		c->status = esp_http_client_get_status_code(ev->client);
	if (c->status != 200)
		return ESP_OK;

	{
		const char *p = ev->data;
		size_t left = ev->data_len;

		while (left) {
			const char *nl = memchr(p, '\n', left);

			if (!nl) {
				/* 残行入 pend；超长行截断尾部并告警
				 * （绝不越界写） */
				size_t room = sizeof(c->pend) - c->pend_len - 1;
				size_t cp = left;

				if (cp > room) {
					ESP_LOGW(TAG, "sse残行溢出,丢%uB",
						 (unsigned)(cp - room));
					cp = room;
				}
				memcpy(c->pend + c->pend_len, p, cp);
				c->pend_len += cp;
				break;
			}
			size_t adv = (size_t)(nl - p) + 1;  /* 真实行宽 */
			size_t ll = adv - 1;
			char line[2048];

			if (ll > sizeof(line) - 1) {
				ESP_LOGW(TAG, "sse行%uB超限截断",
					 (unsigned)ll);
				ll = sizeof(line) - 1;
			}
			if (c->pend_len) {
				size_t cp = c->pend_len;

				if (cp + ll > sizeof(line) - 1) {
					/* 拼不下的极端行：丢残行头保新
					 * 数据（仅超长单行才会走到） */
					ESP_LOGW(TAG,
						 "sse合并行溢出,丢残行头");
					cp = 0;
				}
				memcpy(line, c->pend, cp);
				memcpy(line + cp, p, ll);
				line[cp + ll] = '\0';
				c->pend_len = 0;
				handle_line(c, line, cp + ll);
			} else {
				memcpy(line, p, ll);
				line[ll] = '\0';
				handle_line(c, line, ll);
			}
			left -= adv;	/* 用真实行宽推进（截断只影响拷贝） */
			p = nl + 1;
		}
	}
	return ESP_OK;
}

int ks_llm_chat(const struct ks_llm_cfg *cfg, ks_llm_sent_cb cb, void *user,
		int64_t *first_token_us)
{
	struct ks_llm_ctx c = { .cb = cb, .user = user,
				.cut_clauses = cfg ? cfg->cut_clauses : false };
	char auth[768];		/* MiniMax JWT ~690B：192 会截断致 401 */
	char body[1024];
	char *s;
	cJSON *root, *msgs, *m;
	esp_http_client_handle_t cl;
	int code;

	if (!cfg || !cfg->url || !cfg->api_key || !cfg->model ||
	    !cfg->user_msg || !cb)
		return -1;
	if (!sp_sess_take(15000))	/* V-47：TTS/LLM 全组件单飞 */
		return -6;

	c.t0 = esp_timer_get_time();

	/* 组请求体：system 可选；no_think 附 MiniMax thinking disabled */
	root = cJSON_CreateObject();
	if (!root) {
		sp_sess_give();
		return -1;
	}
	cJSON_AddStringToObject(root, "model", cfg->model);
	cJSON_AddBoolToObject(root, "stream", true);
	if (cfg->no_think) {
		cJSON *th = cJSON_AddObjectToObject(root, "thinking");

		cJSON_AddStringToObject(th, "type", "disabled");
	}
	msgs = cJSON_AddArrayToObject(root, "messages");
	if (cfg->system) {
		m = cJSON_CreateObject();
		cJSON_AddStringToObject(m, "role", "system");
		cJSON_AddStringToObject(m, "content", cfg->system);
		cJSON_AddItemToArray(msgs, m);
	}
	m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", "user");
	cJSON_AddStringToObject(m, "content", cfg->user_msg);
	cJSON_AddItemToArray(msgs, m);
	s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!s) {
		sp_sess_give();
		return -1;
	}
	if (strlen(s) >= sizeof(body))
		ESP_LOGW(TAG, "请求体超%uB截断（ASR 文本过长?）",
			 (unsigned)sizeof(body));
	strlcpy(body, s, sizeof(body));
	cJSON_free(s);

	snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
	esp_http_client_config_t hc = {
		.url = cfg->url,
		.method = HTTP_METHOD_POST,
		.timeout_ms = 30000,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.buffer_size = 2048,
		.buffer_size_tx = 2048,
		.event_handler = http_event,
		.user_data = &c,
	};
	cl = esp_http_client_init(&hc);
	if (!cl) {
		sp_sess_give();
		return -1;
	}
	esp_http_client_set_header(cl, "Authorization", auth);
	esp_http_client_set_header(cl, "Content-Type", "application/json");

	if (esp_http_client_open(cl, strlen(body)) != ESP_OK ||
	    esp_http_client_write(cl, body, strlen(body)) !=
		    (int)strlen(body)) {
		esp_http_client_cleanup(cl);
		sp_sess_give();
		return -1;
	}
	esp_http_client_fetch_headers(cl);

	/* 读循环驱动 HTTP_EVENT_ON_DATA（SSE 逐 chunk 进回调） */
	{
		char buf[1024];
		int n;

		while (!c.abort &&
		       (n = esp_http_client_read(cl, buf, sizeof(buf))) > 0)
			;
	}

	/* 尾句收口：有残留则整段下发；否则空串通告流结束 */
	if (c.sent_len) {
		c.sent[c.sent_len] = '\0';
		cb(c.user, c.sent, true);
	} else {
		cb(c.user, "", true);
	}

	code = esp_http_client_get_status_code(cl);
	esp_http_client_cleanup(cl);
	if (first_token_us)
		*first_token_us = c.first_token_us;
	if (code != 200) {
		ESP_LOGE(TAG, "http %d", code);
		sp_sess_give();
		return -2;
	}
	sp_sess_give();
	return 0;
}
