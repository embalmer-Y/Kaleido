/*
 * provision.c — OOBE Web 配置页实现（V-42）
 *
 * 页面路由：
 *   GET  /                      配置表单（中文/移动端适配/密钥用 textarea
 *                              长条粘贴——负责人要求：长密钥在界面逐字
 *                              输入成本过高，浏览器粘贴是唯一合形态）
 *   POST /save                  表单落 NVS（留空项保持原值）→ 3s 后重启
 *   POST /erase                 抹除全部密钥（出厂复位）→ 3s 后重启
 *   GET  /status                JSON：配置掩码 + 当前 IP（调试/判分用）
 *   /generate_204 等探测 URL    302 回首页（captive portal 助推；V-1 无
 *                              DNS 服务，自动弹窗不保证，横幅明示手输地址）
 * esp_hosted 2.12.9 经标准 esp_wifi API 透传 SoftAP（V-42a 探针：RPC
 * 层 rpc_set_wifi_mode_softap/APSTA 与 AP netif 事件原生支持）。
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"	/* v6 API 拆分：default netif 创建器在此 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "kaleido_secret.h"
#include "provision.h"

static const char *TAG = "oobe";

#define OOBE_AP_SSID	"Kaleido-OOBE"
#define OOBE_BODY_MAX	4096	/* 表单体上限：JWT 690B + 余量 */

static httpd_handle_t s_httpd;
static char *s_body;	/* httpd 默认单线程，共享安全；4KB PSRAM 懒分配
				 * （V-46c 内存布局），首次请求时分配 */
static bool s_body_get(void)
{
	if (!s_body)
		s_body = heap_caps_malloc(OOBE_BODY_MAX,
					  MALLOC_CAP_SPIRAM);
	return s_body != NULL;
}

/* ---- 工具 ---- */

static void restart_task(void *arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(3000));
	esp_restart();
}

static void schedule_restart(void)
{
	xTaskCreate(restart_task, "oobe_rst", 2048, NULL, 4, NULL);
}

/* 从表单体抽字段（urlencoded，含 %XX 与 + 空格解码）。返回 0=找到 */
static int form_field(const char *body, const char *name, char *out,
		      size_t out_n)
{
	size_t nl = strlen(name);
	const char *p = body;

	while ((p = strstr(p, name))) {
		if (p[nl] == '=' &&
		    (p == body || p[-1] == '&' || p[-1] == '\n' ||
		     p[-1] == '\r')) {
			const char *v = p + nl + 1;
			const char *e = strchr(v, '&');
			size_t raw = e ? (size_t)(e - v) : strlen(v);

			/* 逐段解码（%XX 占 3 字节解码为 1，分段安全） */
			size_t di = 0;

			for (size_t i = 0; i < raw && di + 1 < out_n; i++) {
				if (v[i] == '%' && i + 2 < raw) {
					int hi = v[i + 1];
					int lo = v[i + 2];

					hi = (hi >= '0' && hi <= '9') ?
					     hi - '0' :
					     (hi >= 'a' && hi <= 'f') ?
					     hi - 'a' + 10 :
					     (hi >= 'A' && hi <= 'F') ?
					     hi - 'A' + 10 : -1;
					lo = (lo >= '0' && lo <= '9') ?
					     lo - '0' :
					     (lo >= 'a' && lo <= 'f') ?
					     lo - 'a' + 10 :
					     (lo >= 'A' && lo <= 'F') ?
					     lo - 'A' + 10 : -1;
					if (hi < 0 || lo < 0)
						return -1;
					out[di++] = (char)((hi << 4) | lo);
					i += 2;
				} else if (v[i] == '+') {
					out[di++] = ' ';
				} else {
					out[di++] = v[i];
				}
			}
			out[di] = '\0';
			return 0;
		}
		p += nl;
	}
	return -1;
}

static void oobe_ip(char *out, size_t n)
{
	/* 依次取 STA / AP 接口地址；接口存在但未拿到地址（刚切换/
	 * 未连接）时 ip 为全零，跳过继续找下一个 */
	static const char *ifkeys[] = { "WIFI_STA_DEF", "WIFI_AP_DEF" };

	out[0] = '\0';
	for (size_t i = 0; i < sizeof(ifkeys) / sizeof(ifkeys[0]); i++) {
		esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkeys[i]);
		esp_netif_ip_info_t ip;

		if (!netif)
			continue;
		if (esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
		    ip.ip.addr) {
			snprintf(out, n, IPSTR, IP2STR(&ip.ip));
			return;
		}
	}
}

/* ---- 页面 ---- */

static const char PAGE_MAIN[] =
"<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Kaleido 配置</title><style>"
"body{font-family:sans-serif;max-width:640px;margin:2em auto;"
"padding:0 1em;background:#101828;color:#e8eaf0}"
"h1{font-size:1.3em}h3{margin:1.2em 0 .2em}"
"input,textarea{width:100%;box-sizing:border-box;margin:.3em 0 .6em;"
"padding:.6em;background:#1a2332;color:#fff;"
"border:1px solid #39445c;border-radius:6px;font-size:1em}"
"button{padding:.7em 1.6em;background:#4f7cff;border:0;color:#fff;"
"border-radius:6px;font-size:1em;margin-top:.4em}"
".hint{color:#8b93a7;font-size:.85em}"
"</style></head><body>"
"<h1>🦋 Kaleido 配置</h1>"
"<p class=\"hint\">密钥直接粘贴即可；留空的项保持不变。保存后设备重启生效。</p>"
"<form method=\"POST\" action=\"/save\">"
"<h3>Wi-Fi</h3>"
"<input name=\"ssid\" placeholder=\"Wi-Fi 名称（SSID）\">"
"<input name=\"pass\" type=\"password\" placeholder=\"Wi-Fi 密码\">"
"<h3>语音识别 · 阿里百炼 Key</h3>"
"<textarea name=\"dskey\" rows=\"3\" placeholder=\"sk-…（可留空暂不配置）\"></textarea>"
"<h3>对话与音色 · MiniMax Key（JWT，很长）</h3>"
"<textarea name=\"mmkey\" rows=\"5\" placeholder=\"eyJ…（整段粘贴）\"></textarea>"
"<button type=\"submit\">保存并重启</button></form>"
"<form method=\"POST\" action=\"/erase\""
" onsubmit=\"return confirm('抹除全部密钥回到未配置态？')\">"
"<p class=\"hint\">出厂复位：</p>"
"<button type=\"submit\">抹除全部密钥</button></form>"
"</body></html>";

static const char PAGE_SAVED[] =
"<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Kaleido</title></head><body style=\"font-family:sans-serif;"
"background:#101828;color:#e8eaf0;text-align:center;padding-top:3em\">"
"<h2>已保存 ✓</h2><p>设备 3 秒后重启并按新配置连网。</p>"
"<p style=\"color:#8b93a7\">若配置了 Wi-Fi，重启后在本局域网访问设备"
" IP 可回到本页。</p></body></html>";

static const char PAGE_ERASED[] =
"<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"</head><body style=\"font-family:sans-serif;background:#101828;"
"color:#e8eaf0;text-align:center;padding-top:3em\">"
"<h2>已抹除全部密钥 ✓</h2><p>设备 3 秒后重启，回到未配置引导态"
"（热点 " OOBE_AP_SSID "）。</p></body></html>";

/* ---- 处理器 ---- */

static esp_err_t h_root(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	return httpd_resp_send(req, PAGE_MAIN, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_captive(httpd_req_t *req)
{
	/* 各 OS 联网检测 URL → 弹回配置页 */
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", "/");
	return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_status(httpd_req_t *req)
{
	char ip[20];
	char json[128];

	oobe_ip(ip, sizeof(ip));
	snprintf(json, sizeof(json),
		 "{\"configured\":%d,\"ip\":\"%s\"}",
		 ksec_state(), ip);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_save(httpd_req_t *req)
{
	int total = req->content_len;
	int got = 0;
	char field[800];	/* 单字段上限：JWT ~690B + 余量 */
	int saved = 0;

	if (!s_body_get()) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				    "oom");
		return ESP_FAIL;
	}
	if (total <= 0 || total >= OOBE_BODY_MAX) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
				    "body too large");
		return ESP_FAIL;
	}
	while (got < total) {
		int n = httpd_req_recv(req, s_body + got, total - got);

		if (n <= 0) {
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
					    "recv fail");
			return ESP_FAIL;
		}
		got += n;
	}
	s_body[got] = '\0';

	/* 留空=不改：form_field 找不到（未出现该字段）同样跳过 */
	if (form_field(s_body, "ssid", field, sizeof(field)) == 0 &&
	    field[0] &&
	    ksec_set(KSEC_KEY_WIFI_SSID, field) == 0)
		saved |= KSEC_BIT_WIFI;
	if (form_field(s_body, "pass", field, sizeof(field)) == 0 &&
	    field[0] &&
	    ksec_set(KSEC_KEY_WIFI_PASS, field) == 0)
		saved |= KSEC_BIT_WIFI;
	if (form_field(s_body, "dskey", field, sizeof(field)) == 0 &&
	    field[0] &&
	    ksec_set(KSEC_KEY_DS, field) == 0)
		saved |= KSEC_BIT_ASR;
	if (form_field(s_body, "mmkey", field, sizeof(field)) == 0 &&
	    field[0] &&
	    ksec_set(KSEC_KEY_MM, field) == 0)
		saved |= KSEC_BIT_LLM;

	ESP_LOGW(TAG, "save: fields=0x%x now-configured=0x%x", saved,
		 ksec_state());
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_send(req, PAGE_SAVED, HTTPD_RESP_USE_STRLEN);
	schedule_restart();
	return ESP_OK;
}

static esp_err_t h_erase(httpd_req_t *req)
{
	/* 吃掉 body（表单空提交也带 content-length） */
	int total = req->content_len;
	int got = 0;

	while (total > 0 && got < total) {
		int n = httpd_req_recv(req, s_body, total - got);

		if (n <= 0)
			break;
		got += n;
	}
	ksec_erase_all();
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_send(req, PAGE_ERASED, HTTPD_RESP_USE_STRLEN);
	schedule_restart();
	return ESP_OK;
}

/* ---- 服务与 AP ---- */

bool provision_server_running(void)
{
	return s_httpd != NULL;
}

int provision_server_start(void)
{
	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
	httpd_uri_t uri;
	char ip[20];

	if (s_httpd)
		return 0;
	cfg.stack_size = 8192;	/* 页面串 + 表单解析在服务任务栈上 */
	cfg.max_uri_handlers = 8;
	if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
		s_httpd = NULL;
		return -1;
	}

	memset(&uri, 0, sizeof(uri));
	uri.method = HTTP_GET;
	uri.handler = h_root;
	uri.uri = "/";
	httpd_register_uri_handler(s_httpd, &uri);

	uri.handler = h_status;
	uri.uri = "/status";
	httpd_register_uri_handler(s_httpd, &uri);

	/* 常见 captive 探测 URL：命中即弹回配置页 */
	uri.handler = h_captive;
	uri.uri = "/generate_204";		/* Android */
	httpd_register_uri_handler(s_httpd, &uri);
	uri.uri = "/hotspot-detect.html";	/* iOS/macOS */
	httpd_register_uri_handler(s_httpd, &uri);
	uri.uri = "/connecttest.txt";		/* Windows */
	httpd_register_uri_handler(s_httpd, &uri);

	uri.method = HTTP_POST;
	uri.handler = h_save;
	uri.uri = "/save";
	httpd_register_uri_handler(s_httpd, &uri);
	uri.handler = h_erase;
	uri.uri = "/erase";
	httpd_register_uri_handler(s_httpd, &uri);

	oobe_ip(ip, sizeof(ip));
	printf("VL,OOBE,WEB,http://%s/\r\n", ip[0] ? ip : "?");
	fflush(stdout);
	return 0;
}

int provision_ap_start(void)
{
	esp_netif_t *ap = esp_netif_create_default_wifi_ap();
	wifi_config_t wc = { 0 };
	wifi_init_config_t w = WIFI_INIT_CONFIG_DEFAULT();
	static bool s_wifi_inited;

	if (!ap) {
		ESP_LOGE(TAG, "ap netif fail");
		return -1;
	}
	/* 已初始化（'o' 热切换路径：STA 起过）则跳过二次 init */
	if (!s_wifi_inited) {
		ESP_ERROR_CHECK(esp_wifi_init(&w));
		s_wifi_inited = true;
	}
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	strlcpy((char *)wc.ap.ssid, OOBE_AP_SSID, sizeof(wc.ap.ssid));
	wc.ap.ssid_len = strlen(OOBE_AP_SSID);
	wc.ap.channel = 6;
	wc.ap.authmode = WIFI_AUTH_OPEN;	/* V-1 口径：见文件头 */
	wc.ap.max_connection = 2;
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
	ESP_ERROR_CHECK(esp_wifi_start());

	/* DHCP 由 esp_netif AP 默认置 192.168.4.1/24 */
	printf("VL,OOBE,AP,ssid=%s,open,url=http://192.168.4.1/\r\n",
	       OOBE_AP_SSID);
	fflush(stdout);
	return provision_server_start();
}
