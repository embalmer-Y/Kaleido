/* mem.c — W4 记忆接线实现（见 mem.h 头注释） */
#include "mem.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "esp_littlefs.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "kaleido_core.h"
#include "soul.h"
#include "clause_pump.h"

#define TAG "mem"
#define MEM_MOUNT	"/storage"
#define MEM_EP_PATH	MEM_MOUNT"/episodes.jsonl"
#define MEM_CORE_PATH	MEM_MOUNT"/core.json"
#define MEM_REC_STK	6144		/* 静态内部栈：板级纪律（含 km_query
				 * ~1.2KB 深栈：769B 行缓冲+cJSON） */
#define MEM_Q_DEPTH	4
#define MEM_BLOCK_MAX	1536		/* 注入块预算（30 号 §8.3 ≤400 汉字） */

static QueueHandle_t s_q;
#define MEM_HOOK_PROBE_S	30	/* 记忆钩子探测周期（s）          */
static uint32_t s_hook_age;
static volatile float s_hook_score;	/* recorder 写/soul 读：32bit
					 * 对齐单写，RISC-V 天然原子   */
static StaticTask_t s_rec_tcb;
static StackType_t s_rec_stk[MEM_REC_STK];	/* 内部 RAM .bss   */
static bool s_up;			/* 挂载+km 就绪             */

/* ── UTF-8 边界安全裁剪（30 号 §8.2：不劈多字节） ──
 * 规则：n 收缩到 src[n] 不再是续字节——多字节整字符要么完整保留
 * 要么整字符丢弃；ASCII 恒边界。 */
static void utf8_trim(const char *src, char *dst, size_t dstsz)
{
	size_t n = strlen(src);

	if (n >= dstsz)
		n = dstsz - 1;
	while (n && (src[n] & 0xC0) == 0x80)
		n--;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* 时段 tag（30 号 §8.2 语境标签，本地即时可定） */
static void daypart_tag(char *out, size_t n)
{
	time_t now = time(NULL);
	struct tm tm_now;

	if (now < 1767225600 ||		/* 2026-01-01 前=时钟未同步 */
	    !localtime_r(&now, &tm_now)) {
		strlcpy(out, "unknown-time", n);
		return;
	}
	int h = tm_now.tm_hour;

	strlcpy(out, h < 5 ? "night" : h < 11 ? "morning" :
		h < 14 ? "noon" : h < 18 ? "afternoon" : "evening", n);
}

/* 时间相对化（30 号 §8.3 纪律 3） */
static void rel_time(time_t t, char *out, size_t n)
{
	time_t now = time(NULL);
	double days;

	if (t < 1767225600) {
		strlcpy(out, "此前", n);/* 时钟未同步的旧条目       */
		return;
	}
	days = difftime(now, t) / 86400.0;
	if (days < 0.5)
		strlcpy(out, "今天", n);
	else if (days < 1.5)
		strlcpy(out, "昨天", n);
	else if (days < 7)
		snprintf(out, n, "%d天前", (int)(days + 0.5));
	else if (days < 30)
		snprintf(out, n, "%d周前", (int)(days / 7 + 0.5));
	else
		snprintf(out, n, "%d个月前", (int)(days / 30 + 0.5));
}

/* ── recorder：唯一写盘者（静态内部栈任务） ── */
static void recorder_task(void *arg)
{
	(void)arg;
	struct km_episode ep;

	for (;;) {
		if (xQueueReceive(s_q, &ep, pdMS_TO_TICKS(1000)) == pdTRUE) {
			int64_t seq = km_append(&ep);

			if (seq == KM_FULL)
				kf_log("KM,FULL,cap=%d,write_stopped\r\n",
				       km_count());/* M2 停写告警 */
			else if (seq < 0)
				kf_log("KM,APPEND,ERR,%lld\r\n",
				       (long long)seq);
			else
				kf_log("KM,APPEND,seq=%lld,or=%c,n=%d\r\n",
				       (long long)seq, (char)ep.origin,
				       km_count());
		}
		km_housekeeping();	/* 覆盖表脏条 1Hz 兜底落盘  */
		/* W4→W3 记忆钩子探测：30s 节流（W4.1 整改：km_query 深
		 * 栈严禁上 soul tick——曾致其 3KB PSRAM 栈溢出保育
		 * 断言连环重启；探测归本任务=唯一合法 flash 触碰者） */
		if (++s_hook_age >= MEM_HOOK_PROBE_S) {
			struct km_hit hits[1];

			s_hook_age = 0;
			s_hook_score = km_count() > 0 &&
				km_query(&(struct km_query){ 0 },
					 hits, 1) > 0 ?
				hits[0].score : 0.0f;
			kf_log("KM,HOOK,score=%.2f\r\n", s_hook_score);
		}
	}
}

int mem_boot(void)
{
	esp_vfs_littlefs_conf_t cfg = {
		.base_path = MEM_MOUNT,
		.partition_label = "storage",
		.format_if_mount_failed = true,	/* 首电/坏卷自救 */
	};

	/* 首电格式化 960KB 在调用任务内连续擦写数秒，会饿死 IDLE0 触发
	 * TWDT 复位（首启 rst:0x7 实测）。窗口内临时豁免 IDLE0，挂载
	 * 完成即恢复——豁免面仅限本窗口，其余时段纪律不变 */
	TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
	bool idle_freed = esp_task_wdt_delete(idle0) == ESP_OK;

	esp_err_t err = esp_vfs_littlefs_register(&cfg);

	if (idle_freed)
		esp_task_wdt_add(idle0);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "littlefs mount fail %s", esp_err_to_name(err));
		kf_log("KM,BOOT,ERR,mount_fail\r\n");
		return -1;
	}
	size_t total = 0, used = 0;

	esp_littlefs_info("storage", &total, &used);
	if (km_init(MEM_EP_PATH, MEM_CORE_PATH) != 0) {
		kf_log("KM,BOOT,ERR,km_init\r\n");
		return -2;
	}
	s_q = xQueueCreate(MEM_Q_DEPTH, sizeof(struct km_episode));
	if (!s_q)
		return -3;
	/* 板级纪律：静态内部栈（写盘任务，见 mem.h 头注释） */
	if (xTaskCreateStatic(recorder_task, "km_rec", MEM_REC_STK, NULL, 3,
			      s_rec_stk, &s_rec_tcb) == NULL)
		return -4;
	s_up = true;
	kf_log("KM,BOOT,ok,seq=%llu,n=%d,fs=%uk/%uk\r\n",
	       (unsigned long long)km_seq_current(), km_count(),
	       (unsigned)(used / 1024), (unsigned)(total / 1024));
	return 0;
}

void mem_net_up(void)
{
	setenv("TZ", "CST-8", 1);
	tzset();
	if (!esp_sntp_enabled()) {
		esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
		esp_sntp_setservername(0, "ntp.aliyun.com");
		esp_sntp_init();
		kf_log("KM,SNTP,start\r\n");
	}
}

void mem_round_record(const char *asr_text, int rc, int origin)
{
	struct km_episode ep;

	if (!s_up || !s_q || rc != 0 || !asr_text || !asr_text[0])
		return;		/* rc!=0 不入检索集（30 号 §8.2） */
	memset(&ep, 0, sizeof(ep));
	utf8_trim(asr_text, ep.summary, sizeof(ep.summary));
	char clause[48];

	pump_first_clause(clause, sizeof(clause));
	if (clause[0])
		utf8_trim(clause, ep.facts[0], sizeof(ep.facts[0]));
	daypart_tag(ep.tags[0], sizeof(ep.tags[0]));
	ep.importance = 0.3f;
	ep.origin = (int8_t)origin;
	time_t now = time(NULL);

	ep.t = now >= 1767225600 ? now : 0;	/* 未对时→km 补 now */
	struct soul_snap snap;

	soul_snapshot(&snap);
	ep.affect_v = (int8_t)(snap.emo.state.valence * 127.0f);
	ep.affect_a = (int8_t)(snap.emo.state.arousal * 127.0f);
	xQueueSend(s_q, &ep, 0);	/* 满则丢本轮记忆（不阻塞轮） */
}

const char *mem_prompt_block(void)
{
	static char block[MEM_BLOCK_MAX];
	struct km_hit hits[KM_QUERY_TOP];
	int n;
	size_t off = 0;

	if (!s_up)
		return "";
	block[0] = '\0';
	n = km_query(&(struct km_query){ .min_importance = 0.0f },
		     hits, KM_QUERY_TOP);
	kf_log("KM,RECALL,n=%d,lib=%d\r\n", n, km_count());
	off += snprintf(block + off, sizeof(block) - off,
			"[你的长期记忆（回忆，未必精确）]\n");
	if (n <= 0) {
		/* 30 号 §8.3 纪律 2：显式空态防 LLM 编造 */
		off += snprintf(block + off, sizeof(block) - off,
				"- 你还没有和TA的共同回忆\n");
		return block;
	}
	for (int i = 0; i < n && off < sizeof(block) - 160; i++) {
		char when[24];
		struct km_episode *ep = &hits[i].ep;

		rel_time(ep->t, when, sizeof(when));
		off += snprintf(block + off, sizeof(block) - off,
				"- %s：%s%s\n", when, ep->summary,
				abs(ep->affect_v) >= 64 ? "（印象深）" : "");
	}
	/* core 事实段（与回忆段分行分开——RECALLED 禁提升为事实） */
	char core[512];

	if (km_core_read(core, sizeof(core)) > 0 && core[0]) {
		char line[256];

		utf8_trim(core, line, sizeof(line));
		off += snprintf(block + off, sizeof(block) - off,
				"[关于用户的既定事实]\n- %s\n", line);
	}
	return block;
}

void mem_stats(void)
{
	if (!s_up) {
		kf_log("KM,STATS,down\r\n");
		return;
	}
	size_t total = 0, used = 0;

	esp_littlefs_info("storage", &total, &used);
	kf_log("KM,STATS,n=%d,cap=%d,seq=%llu,fs=%u/%uk\r\n",
	       km_count(), km_cap(),
	       (unsigned long long)km_seq_current(),
	       (unsigned)(used / 1024), (unsigned)(total / 1024));
}

/* W4→W3 记忆钩子：返回 km_rec 30s 周期探测的 top1 分数缓存。
 * 纯变量读——探测本体在 km_rec（内部栈）做，见 recorder_task。
 * W4.1 整改记录：曾直接在 soul tick 上跑 km_query，769B 行缓冲+
 * cJSON 深栈溢出其 3KB PSRAM 栈（Stack protection fault 连环重启） */
float mem_hook_probe(void)
{
	return s_up ? s_hook_score : 0.0f;
}

int64_t mem_test_inject(void)
{
	struct km_episode ep;

	if (!s_up)
		return -1;
	memset(&ep, 0, sizeof(ep));
	strlcpy(ep.summary, "【测试】负责人在调试记忆功能时说的这句话",
		sizeof(ep.summary));
	strlcpy(ep.facts[0], "记忆注入链路验证", sizeof(ep.facts[0]));
	strlcpy(ep.tags[0], "test", sizeof(ep.tags[0]));
	ep.importance = 0.8f;
	ep.origin = KM_ORIGIN_USER;
	ep.t = time(NULL);
	/* 直接同步写（调试入口，main 任务内部栈，绕过队列便于取 seq） */
	return km_append(&ep);
}
