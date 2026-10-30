/*
 * sp_minimax.c — MiniMax t2a_v2 流式 TTS（V-41 切换）
 *
 * HTTPS + SSE，音频以 hex 编码嵌在 data 事件里；按子句一请求（无会话
 * 状态，天然避开 cosyvoice 的 continue/run-task 续喂坑）。
 * 流式 hex 解码：不缓冲整行（单行可达 64KB+），标记串 "\"audio\":\""
 * 之后逐对 nibble 还原 PCM，暂存到字段闭合，按 status 裁决下发。
 *
 * 线上事实（V-41 抓包）与状态机不变量：
 *  - 事件为紧凑 JSON："audio" 字段先于 "status" 到达（依赖此顺序）。
 *  - status=1 增量块：audio 是本块新增 PCM —— 暂存后下发。
 *  - status=2 末事件：audio 是【全量重复】（hex 长度=增量之和）——丢弃。
 *    故 hex 字段闭合时不立即下发，等 "status": 的值字节裁决；
 *    若整个事件没有 status 字段（防御），feed 收尾统一下发暂存。
 *  - 匹配器是纯字节流状态机，SSE 的 data:/换行/事件边界只是普通字节
 *    穿过，无需行级缓冲。
 */
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "kaleido_speech.h"
#include "sp_internal.h"

#if CONFIG_KS_TTS_PROVIDER_MINIMAX

static const char *TAG = "ks-mm";

#define MM_HOST		"https://api.minimaxi.com"
#define MM_HEXMARK	"\"audio\":\""
#define MM_STATMARK	"\"status\":"

/* 暂存缓冲策略：初值 8KB，倍增至 256KB 封顶（PSRAM）。一个增量块
 * 最多 ~32KB PCM，正常远用不满；只有末事件的全量重复才能逼近上限，
 * 而它注定被裁决丢弃——到顶后静默丢字节是无害的。 */
#define MM_PEND_MAX	262144

/* cfg 由调用方持有（HIL 用 static 结构体），生命周期须覆盖全部
 * feed 调用——见 kaleido_speech.h 线程/生命周期契约。 */
static struct {
	const struct ks_tts_cfg *cfg;
	struct ks_tts_events ev;
	char gid[48];
	int64_t first_pcm_us;
} s_mm;

/* 暖连接（延迟优化 2026-10-30）：t2a 每子句一次完整 TLS 是截断→首音
 * 3.2s 基线中 ~1.3s 的大头。句柄跨 feed、跨轮存活（由 sp_sess 会话锁
 * 串行化：open→close 持锁期间只有 feed 触碰它；轮间仅 prewarm 触碰）。
 * 服务器若关连接：open/write 报错→清句柄按需重建（回退路径永远成立，
 * 复用只是快路径）。HTTP keep-alive 由 esp_http_client 对同一句柄的
 * 再次 open 自动生效。 */
static esp_http_client_handle_t s_warm;

/* 新建 t2a 客户端（feed 回退路径与 prewarm 共用；api_key 显式传入：
 * prewarm 在会话外运行，s_mm.cfg 此时为 NULL） */
static esp_http_client_handle_t mm_client_new(const char *url,
					      const char *api_key,
					      uint32_t timeout_ms)
{
	esp_http_client_config_t hc = {
		.url = url,
		.method = HTTP_METHOD_POST,
		.timeout_ms = timeout_ms ? timeout_ms : 15000,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.buffer_size = 2048,
		.buffer_size_tx = 4096,
	};
	esp_http_client_handle_t cl = esp_http_client_init(&hc);

	if (cl) {
		char auth[768];		/* JWT ~690B，小缓冲截断即 401 */

		snprintf(auth, sizeof(auth), "Bearer %s", api_key);
		esp_http_client_set_header(cl, "Authorization", auth);
		esp_http_client_set_header(cl, "Content-Type",
					   "application/json");
	}
	return cl;
}

struct mm_parse {
	bool in_hex;		/* 处于 audio hex 字段           */
	int nib;		/* 待配对高半字节；-1 无；
				 * -2 = "status": 已匹配完，
				 * 下一非空白字节是裁决值        */
	size_t mark;		/* "\"audio\":\"" 匹配进度       */
	size_t smark;		/* "\"status\":" 匹配进度        */
	uint8_t *pend;		/* 当前字段解码暂存（PSRAM）     */
	size_t pend_len, pend_cap;
	int64_t t_first;
};

static void mm_emit_pending(struct mm_parse *ps)
{
	if (!ps->pend_len)
		return;
	if (ps->t_first < 0) {
		ps->t_first = esp_timer_get_time();
		s_mm.first_pcm_us = ps->t_first;
	}
	if (s_mm.ev.on_pcm)
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
		if (ch == '"') {	/* 字段闭合，待 status 裁决 */
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

			if (nc > MM_PEND_MAX)
				nc = MM_PEND_MAX;
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
				return;	/* 极端无内存：丢字节（见头注） */
			}
		}
		ps->pend[ps->pend_len++] = (uint8_t)((ps->nib << 4) | v);
		ps->nib = -1;
		return;
	}

	/* "status": 标记匹配（紧随 audio 字段之后到达） */
	if (ch == MM_STATMARK[ps->smark]) {
		ps->smark++;
		if (!MM_STATMARK[ps->smark]) {
			ps->smark = 0;
			ps->nib = -2;	/* 下一非空白字节是裁决值 */
			return;
		}
	} else {
		ps->smark = (ch == MM_STATMARK[0]) ? 1 : 0;
	}

	if (ps->nib == -2) {
		if (ch == ' ' || ch == '\t')
			return;		/* 合法 JSON 允许冒号后空白 */
		if (ch != '2')
			mm_emit_pending(ps);	/* 1 等：增量块下发 */
		ps->pend_len = 0;	/* 2：全量重复丢弃 */
		ps->nib = -1;
		/* 值字符交给下方标记逻辑继续匹配 */
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

/* V-47 错误遥测：on_error 可 NULL；哑音/HTTP/中断统一出口 */
static void err_report(int stage, int code)
{
	if (s_mm.ev.on_error)
		s_mm.ev.on_error(s_mm.ev.user, stage, code);
}

int ks_tts_open(const struct ks_tts_cfg *cfg,
		const struct ks_tts_events *ev)
{
	const char *key;

	/* 会话互斥（V-47）：持锁至 close；忙=-6，调用方据此退避 */
	if (!sp_sess_take(0))
		return -6;
	if (!cfg || !cfg->api_key || !cfg->api_key[0]) {
		ESP_LOGE(TAG, "no cfg/api key (CONFIG_KS_MM_KEY)");
		sp_sess_give();
		return -1;
	}
	key = cfg->api_key;

	memset(&s_mm, 0, sizeof(s_mm));
	s_mm.cfg = cfg;
	s_mm.ev = ev ? *ev : (struct ks_tts_events){ 0 };
	if (sp_mm_group_id(key, s_mm.gid, sizeof(s_mm.gid)) != 0) {
		ESP_LOGE(TAG, "parse GroupID from jwt fail");
		s_mm.cfg = NULL;
		err_report(KS_TTS_ERR_OPEN, -1);
		sp_sess_give();
		return -1;
	}
	ESP_LOGI(TAG, "minimax tts ready model=%s voice=%s",
		 cfg->model, cfg->voice);
	return 0;
}

int ks_tts_feed(const char *text, bool last, uint32_t timeout_ms)
{
	char url[160];
	cJSON *root;
	char *body;
	esp_http_client_handle_t cl;
	struct mm_parse ps = {
		.nib = -1,
		.t_first = -1,
	};
	char buf[1024];
	int64_t deadline;
	int n;
	bool complete = false;
	int code;
	int ret = 0;

	if (!s_mm.cfg)
		return -2;
	if (!text || !text[0])
		return last ? 0 : -1;	/* flush 语义：无会话状态直接过 */

	if (!timeout_ms)
		timeout_ms = 15000;	/* 0=未设：给个有限缺省 */
	/* timeout_ms 同时作为 per-I/O 超时与整流墙钟上限（滴流服务器
	 * 不至于吊死调用方） */
	deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

	snprintf(url, sizeof(url), MM_HOST "/v1/t2a_v2?GroupId=%s",
		 s_mm.gid);

	root = cJSON_CreateObject();
	if (!root)
		return -2;
	cJSON_AddStringToObject(root, "model", s_mm.cfg->model);
	cJSON_AddStringToObject(root, "text", text);
	cJSON_AddBoolToObject(root, "stream", true);
	{
		cJSON *vs = cJSON_AddObjectToObject(root, "voice_setting");

		cJSON_AddStringToObject(vs, "voice_id", s_mm.cfg->voice);
		/* 情感调制通道（W2 tts_hint 消费；0 值不传=provider 缺省，
		 * 保持既有请求形态不变——防 JSON 兼容性回退） */
		if (s_mm.cfg->speech_rate > 0.0f)
			cJSON_AddNumberToObject(vs, "speech_rate",
						s_mm.cfg->speech_rate);
		if (s_mm.cfg->pitch != 0)
			cJSON_AddNumberToObject(vs, "pitch",
						s_mm.cfg->pitch);
	}
	{
		cJSON *as = cJSON_AddObjectToObject(root, "audio_setting");

		cJSON_AddNumberToObject(as, "sample_rate",
					s_mm.cfg->sample_rate);
		cJSON_AddNumberToObject(as, "bit_rate", 32000);
		cJSON_AddStringToObject(as, "format", "pcm");
		cJSON_AddNumberToObject(as, "channel", 1);
	}
	body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!body)
		return -2;

	/* 快路径：复用暖连接（免 TLS 握手）；失败即弃、新建重试一次 */
	cl = s_warm;
	if (cl) {
		esp_http_client_set_url(cl, url);
		if (esp_http_client_open(cl, strlen(body)) != ESP_OK) {
			esp_http_client_cleanup(cl);
			cl = NULL;
			s_warm = NULL;
		}
	}
	if (!cl) {
		cl = mm_client_new(url, s_mm.cfg->api_key, timeout_ms);
		if (!cl) {
			cJSON_free(body);
			err_report(KS_TTS_ERR_LOCAL, -2);
			return -2;
		}
	}

	if (esp_http_client_write(cl, body, strlen(body)) !=
			(int)strlen(body)) {
		/* 写失败：可能是复用连接的服务端半关——重建一次再试 */
		esp_http_client_cleanup(cl);
		if (s_warm == cl)
			s_warm = NULL;
		cl = mm_client_new(url, s_mm.cfg->api_key, timeout_ms);
		if (!cl ||
		    esp_http_client_open(cl, strlen(body)) != ESP_OK ||
		    esp_http_client_write(cl, body, strlen(body)) !=
			    (int)strlen(body)) {
			if (cl)
				esp_http_client_cleanup(cl);
			cJSON_free(body);
			err_report(KS_TTS_ERR_LOCAL, -2);
			return -2;
		}
	}
	cJSON_free(body);
	esp_http_client_fetch_headers(cl);

	/* 提前看状态码：非 200 直接失败，不白解析错误体 */
	code = esp_http_client_get_status_code(cl);
	if (code != 200) {
		/* 4xx 时读一小段错误体辅助定位（base_resp.status_msg） */
		n = esp_http_client_read(cl, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			ESP_LOGE(TAG, "t2a http %d: %.120s", code, buf);
		} else {
			ESP_LOGE(TAG, "t2a http %d", code);
		}
		esp_http_client_cleanup(cl);
		err_report(KS_TTS_ERR_HTTP, -3);
		return -3;
	}

	while ((n = esp_http_client_read(cl, buf, sizeof(buf))) >= 0) {
		if (n == 0) {
			if (esp_http_client_is_complete_data_received(cl)) {
				complete = true;
				break;
			}
			continue;
		}
		for (int i = 0; i < n; i++)
			mm_byte(&ps, buf[i]);
		if (esp_timer_get_time() >= deadline) {
			ESP_LOGE(TAG, "t2a stream deadline %ums", timeout_ms);
			break;
		}
	}
	/* 无 status 收尾（或流中断前的最后增量）：防御性下发 */
	mm_emit_pending(&ps);
	free(ps.pend);

	/* 服务器正常关连接时 read 也会以负值退出：以协议状态复核，
	 * 只有确实没收完整（读错/超时）才算中断 */
	if (!complete && esp_http_client_is_complete_data_received(cl))
		complete = true;

	code = esp_http_client_get_status_code(cl);
	if (code == 200 && complete) {
		/* 成功：保留句柄供下一子句/下一轮复用（keep-alive） */
		s_warm = cl;
	} else {
		/* 失败路径：错误体/中断后连接状态不可信，弃之 */
		esp_http_client_cleanup(cl);
		if (s_warm == cl)
			s_warm = NULL;
	}
	if (code != 200) {
		ESP_LOGE(TAG, "t2a http %d", code);
		err_report(KS_TTS_ERR_HTTP, -3);
		return -3;
	}
	if (!complete) {
		/* 读错误/超时中断：音频可能不完整，向上明确报错
		 * （M2 状态机据此决定重试/道歉话术，而非当成功） */
		ESP_LOGE(TAG, "t2a stream cut incomplete");
		ret = -4;
		err_report(KS_TTS_ERR_CUT, -4);
	}
	/* 哑音签名（V-47）：200 完整收流但零 PCM——间歇哑音的唯一
	 * 可观测形态，此前完全不可见；先让它落痕再谈根因 */
	if (ret == 0 && !s_mm.first_pcm_us) {
		ESP_LOGW(TAG, "t2a SILENT: 200 ok, 0 pcm bytes");
		err_report(KS_TTS_ERR_SILENT, 0);
	}
	return ret;
}

int64_t ks_tts_first_pcm_us(void)
{
	return s_mm.first_pcm_us ? s_mm.first_pcm_us : -1;
}

void ks_tts_close(void)
{
	/* 无会话状态：清 cfg 即可（也顺带解除 feed 的入口放行）；
	 * 暖连接 s_warm 有意跨轮存活 */
	s_mm.cfg = NULL;
	sp_sess_give();		/* V-47：释放会话锁 */
}

/* 连接预热（延迟优化 2026-10-30）：轮起跑前（采音期）后台调用。
 * 目标=把 DNS+TCP+TLS+证书校验（实测 ~1.3s）从首子句关键路径挪走。
 * 手法：对同一句柄发一次最小真实 t2a 请求（text="."，token 成本可
 * 忽略）并完整排水——保证连接形态与生产请求一致（keep-alive 兼容），
 * 顺带验证既有 s_warm 的存活性（服务器静默关闭则弃旧建新）。
 * 忙（-6）= 本轮起跑竞态：直接放弃预热，feed 走冷路径，无害 */
int ks_tts_prewarm(const struct ks_tts_cfg *cfg)
{
	char gid[48];
	char url[160];
	char body[384];
	char buf[512];
	esp_http_client_handle_t cl;
	int64_t deadline;
	int n;

	if (!sp_sess_take(0))
		return -6;
	if (!cfg || !cfg->api_key || !cfg->api_key[0] ||
	    sp_mm_group_id(cfg->api_key, gid, sizeof(gid)) != 0) {
		sp_sess_give();
		return -1;
	}
	snprintf(url, sizeof(url), MM_HOST "/v1/t2a_v2?GroupId=%s", gid);
	/* 固定字段 snprintf（内容无转义需求，免 cJSON 依赖） */
	snprintf(body, sizeof(body),
		 "{\"model\":\"%s\",\"text\":\".\",\"stream\":true,"
		 "\"voice_setting\":{\"voice_id\":\"%s\"},"
		 "\"audio_setting\":{\"sample_rate\":%u,\"bit_rate\":32000,"
		 "\"format\":\"pcm\",\"channel\":1}}",
		 cfg->model, cfg->voice, (unsigned)cfg->sample_rate);

	cl = s_warm;
	if (cl) {
		esp_http_client_set_url(cl, url);
		if (esp_http_client_open(cl, strlen(body)) != ESP_OK) {
			esp_http_client_cleanup(cl);
			s_warm = cl = NULL;
		}
	}
	if (!cl) {
		cl = mm_client_new(url, cfg->api_key, 8000);
		if (!cl) {
			sp_sess_give();
			return -2;
		}
	}
	if (esp_http_client_write(cl, body, strlen(body)) !=
			(int)strlen(body)) {
		esp_http_client_cleanup(cl);
		s_warm = NULL;
		sp_sess_give();
		return -2;
	}
	esp_http_client_fetch_headers(cl);
	/* 排水至完成（字节丢弃：预热音频不进播放链）。8s 墙钟上限：
	 * 采音窗 ≥3s 时本调用在首子句前完成；拖过头的收益本就归零 */
	deadline = esp_timer_get_time() + 8000000;
	while ((n = esp_http_client_read(cl, buf, sizeof(buf))) >= 0) {
		if (n == 0 &&
		    esp_http_client_is_complete_data_received(cl))
			break;
		if (esp_timer_get_time() >= deadline)
			break;
	}
	if (esp_http_client_is_complete_data_received(cl) &&
	    esp_http_client_get_status_code(cl) == 200) {
		s_warm = cl;		/* 连接确认可用，留喂下一子句 */
	} else {
		esp_http_client_cleanup(cl);
		s_warm = NULL;
	}
	sp_sess_give();
	return 0;
}

#endif  /* CONFIG_KS_TTS_PROVIDER_MINIMAX */
