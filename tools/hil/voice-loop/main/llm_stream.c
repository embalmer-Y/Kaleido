/*
 * llm_stream.c — SSE 流式 chat：esp_http_client + cJSON 逐 delta 解析
 */
#include "llm_stream.h"

#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "llm";

struct llm_ctx {
	llm_sent_cb cb;
	void *user;
	int64_t t0;
	int64_t first_token_us;
	char pend[512];      /* 跨 chunk 的 SSE 行缓冲             */
	size_t pend_len;
	char sent[512];      /* 攒句缓冲                            */
	size_t sent_len;
	bool abort;
	int status;
};

/* 句读即回调：。？！；\n 或缓冲将满 */
static void emit(struct llm_ctx *c, const char *delta, size_t n)
{
	static const unsigned char pun_end[3] = { 0xe3, 0x80, 0x82 };
	static const unsigned char pun_q[3] = { 0xef, 0xbc, 0x9f };
	static const unsigned char pun_e[3] = { 0xef, 0xbc, 0x81 };
	static const unsigned char pun_s[3] = { 0xef, 0xbc, 0x9b };

	for (size_t i = 0; i < n; i++) {
		unsigned char ch = delta[i];

		if (c->sent_len + 3 >= sizeof(c->sent)) {
			c->sent[c->sent_len] = '\0';
			if (!c->cb(c->user, c->sent, false))
				c->abort = true;
			c->sent_len = 0;
			/* G5：flush 后当前字节照常入缓冲        */
		}
		c->sent[c->sent_len++] = ch;
		bool cut = (ch == '\n');
		const unsigned char *t =
			(const unsigned char *)c->sent + c->sent_len - 3;

		if (!cut && c->sent_len >= 3)
			cut = !memcmp(t, pun_end, 3) ||
			      !memcmp(t, pun_q, 3) ||
			      !memcmp(t, pun_e, 3) ||
			      !memcmp(t, pun_s, 3);
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

static void handle_line(struct llm_ctx *c, const char *line,
			size_t len)
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
		emit(c, content->valuestring,
		     strlen(content->valuestring));
	}
	cJSON_Delete(root);
}

static esp_err_t http_event(esp_http_client_event_t *ev)
{
	struct llm_ctx *c = ev->user_data;

	switch (ev->event_id) {
	case HTTP_EVENT_ON_DATA:
		if (c->status == 0)
			c->status = esp_http_client_get_status_code(
				ev->client);
		if (c->status != 200)
			break;
		{
			const char *p = ev->data;
			size_t left = ev->data_len;

			while (left) {
				const char *nl = memchr(p, '\n', left);

				if (!nl) {
					/* 残行入 pend */
					if (c->pend_len + left >=
					    sizeof(c->pend)) {
						c->pend_len = 0;
					}
					memcpy(c->pend + c->pend_len, p,
					       left);
					c->pend_len += left;
					break;
				}
				size_t ll = nl - p;
				char line[512];

				if (c->pend_len) {
					size_t cp = c->pend_len;

					if (cp + ll >= sizeof(line))
						cp = 0;
					memcpy(line, c->pend, cp);
					memcpy(line + cp, p, ll);
					line[cp + ll] = '\0';
					c->pend_len = 0;
					handle_line(c, line,
						    cp + ll);
				} else {
					if (ll < sizeof(line)) {
						memcpy(line, p, ll);
						line[ll] = '\0';
						handle_line(c, line, ll);
					}
				}
				left -= ll + 1;
				p = nl + 1;
			}
		}
		break;
	default:
		break;
	}
	return ESP_OK;
}

int llm_stream_chat(const struct llm_cfg *cfg, llm_sent_cb cb,
		    void *user, int64_t *first_token_us)
{
	struct llm_ctx c = { .cb = cb, .user = user };
	char auth[192];
	char body[1024];
	int ret;
	cJSON *root, *msgs, *m;

	c.t0 = esp_timer_get_time();

	root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", cfg->model);
	cJSON_AddBoolToObject(root, "stream", true);
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
	{
		char *s = cJSON_PrintUnformatted(root);

		strlcpy(body, s, sizeof(body));
		cJSON_free(s);
	}
	cJSON_Delete(root);

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
	esp_http_client_handle_t cl = esp_http_client_init(&hc);
	esp_http_client_set_header(cl, "Authorization", auth);
	esp_http_client_set_header(cl, "Content-Type",
				   "application/json");

	ret = esp_http_client_open(cl, strlen(body));
	if (ret != ESP_OK) {
		esp_http_client_cleanup(cl);
		return -1;
	}
	esp_http_client_write(cl, body, strlen(body));
	esp_http_client_fetch_headers(cl);
	{
		char buf[1024];
		int n;

		/* 读循环驱动 HTTP_EVENT_ON_DATA（SSE 逐 chunk 进回调） */
		while (!c.abort &&
		       (n = esp_http_client_read(cl, buf,
						 sizeof(buf))) > 0)
			;
	}
	if (c.sent_len) {           /* 尾句 */
		c.sent[c.sent_len] = '\0';
		cb(c.user, c.sent, true);
		c.sent_len = 0;
	} else {
		cb(c.user, "", true);   /* 流结束信号 */
	}
	int code = esp_http_client_get_status_code(cl);

	esp_http_client_cleanup(cl);
	if (first_token_us)
		*first_token_us = c.first_token_us;
	if (code != 200) {
		ESP_LOGE(TAG, "http %d", code);
		return -2;
	}
	return 0;
}
