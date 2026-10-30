/*
 * kaleido_core.c — KF 应用框架实现（设计约束见 kaleido_core.h 文件头）
 */
#include "kaleido_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "portmacro.h"

#include "esp_log_write.h"

#define KF_EV_N		192	/* 事件环容量（76B/条 ≈14.6KB 内部） */
#define KF_EV_MSG	60
#define KF_BEAT_N	10

static const char *TAG = "kf";

struct kf_ev_ent {
	uint32_t t_ms;
	char mod[10];
	char msg[KF_EV_MSG];
};

struct kf_beat_ent {
	char mod[10];
	uint32_t last_ms;
	bool seen;
};

static struct kf_ev_ent s_ev[KF_EV_N];
static uint16_t s_ev_w;			/* 下一个写槽 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static struct kf_beat_ent s_beat[KF_BEAT_N];
static bool s_dbg;
static bool s_inited;

/* ---- 日志通道（缺陷 B 根治）：单写者任务独占 stdout ---- */
#define KF_LINE_MAX	160
#define KF_POOL_N	32
static char s_pool[KF_POOL_N][KF_LINE_MAX];	/* 行池（内部 RAM 5KB：
	 * 日志需在 PSRAM 异常场景存活） */
static bool s_used[KF_POOL_N];
static QueueHandle_t s_lq;
static volatile bool s_lready;	/* logger 就绪前 kf_log 直落（早启期） */

struct kf_line { char *p; bool heap; };

/* 行槽获取：池优先，满则堆回落，再满返回 NULL（调用方放弃该行） */
static char *line_get(struct kf_line *out)
{
	for (int i = 0; i < KF_POOL_N; i++) {
		if (!s_used[i]) {
			s_used[i] = true;
			out->p = s_pool[i];
			out->heap = false;
			return out->p;
		}
	}
	out->p = malloc(KF_LINE_MAX);
	out->heap = out->p != NULL;
	return out->p;
}

static void line_put(struct kf_line *l)
{
	if (l->heap) {
		free(l->p);
		return;
	}
	for (int i = 0; i < KF_POOL_N; i++)
		if (s_pool[i] == l->p) {
			s_used[i] = false;
			return;
		}
}

static int kf_enqueue(const char *line)
{
	struct kf_line l;

	if (!line_get(&l))
		return 0;	/* 池+堆俱满：丢弃（计数走 stat） */
	strlcpy(l.p, line, KF_LINE_MAX);
	if (xQueueSend(s_lq, &l, 0) != pdTRUE) {
		line_put(&l);
		return 0;
	}
	return 1;
}

static void logger_task(void *arg)
{
	struct kf_line l;

	(void)arg;
	s_lready = true;
	for (;;) {
		if (xQueueReceive(s_lq, &l, portMAX_DELAY) == pdTRUE) {
			printf("%s\r\n", l.p);
			fflush(stdout);	/* 全系统唯一 fflush 点 */
			line_put(&l);
		}
	}
}

/* IDF 组件日志（ESP_LOGx）重定向：同样入队，stdout 全系统单写者 */
static int kf_log_vprintf(const char *fmt, va_list args)
{
	char tmp[KF_LINE_MAX];

	vsnprintf(tmp, sizeof(tmp), fmt, args);
	if (s_lready)
		kf_enqueue(tmp);
	else
		printf("%s\r\n", tmp);
	return strlen(tmp);
}

void kf_log(const char *fmt, ...)
{
	char tmp[KF_LINE_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (s_lready)
		kf_enqueue(tmp);
	else
		printf("%s\r\n", tmp);
}

void kf_log_raw(const char *text)
{
	if (!s_lready) {
		printf("%s", text);
		return;
	}
	/* 大块：整段堆拷贝入队（logger 原样输出后释放） */
	size_t n = strlen(text) + 1;
	char *heap = malloc(n);

	if (!heap)
		return;
	memcpy(heap, text, n);
	struct kf_line l = { .p = heap, .heap = true };

	if (xQueueSend(s_lq, &l, 0) != pdTRUE)
		free(heap);
}

static uint32_t now_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

void kf_init(void)
{
	if (s_inited)
		return;
	s_inited = true;
	s_lq = xQueueCreate(KF_POOL_N + 8, sizeof(struct kf_line));
	if (s_lq) {
		if (xTaskCreate(logger_task, "kf_log", 3072, NULL, 3,
				NULL) == pdPASS)
			esp_log_set_vprintf(kf_log_vprintf);
	}
	kf_boot_forensics();
}

bool kf_dbg_get(void)
{
	return s_dbg;
}

bool kf_dbg_toggle(void)
{
	s_dbg = !s_dbg;
	/* 全局级别随动：模块内 ESP_LOGx 的可见性一次到位 */
	esp_log_level_set("*", s_dbg ? ESP_LOG_DEBUG : ESP_LOG_WARN);
	return s_dbg;
}

void kf_ev(const char *mod, const char *fmt, ...)
{
	struct kf_ev_ent *e;
	va_list ap;

	portENTER_CRITICAL(&s_mux);
	e = &s_ev[s_ev_w];
	e->t_ms = now_ms();
	strlcpy(e->mod, mod, sizeof(e->mod));
	portEXIT_CRITICAL(&s_mux);

	va_start(ap, fmt);
	vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
	va_end(ap);

	if (s_dbg)
		kf_log("KF,%s,%s", mod, e->msg);
	if (++s_ev_w == KF_EV_N)
		s_ev_w = 0;
}

void kf_beat(const char *mod)
{
	uint32_t t = now_ms();

	portENTER_CRITICAL(&s_mux);
	for (int i = 0; i < KF_BEAT_N; i++) {
		if (!s_beat[i].seen) {
			strlcpy(s_beat[i].mod, mod, sizeof(s_beat[i].mod));
			s_beat[i].seen = true;
			s_beat[i].last_ms = t;
			break;
		}
		if (strcmp(s_beat[i].mod, mod) == 0) {
			s_beat[i].last_ms = t;
			break;
		}
	}
	portEXIT_CRITICAL(&s_mux);
}

void kf_health_dump(void)
{
	uint32_t heap_int = 0, heap_ps = 0;

	kf_ev("kf", "health_dump");
	heap_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	heap_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	kf_log("VL,KF,HEAP,int=%u,ps=%u,intmax=%u",
	       (unsigned)(heap_int >> 10), (unsigned)(heap_ps >> 10),
	       (unsigned)(heap_caps_get_largest_free_block(
				  MALLOC_CAP_INTERNAL) >> 10));

	portENTER_CRITICAL(&s_mux);
	uint32_t t = now_ms();

	for (int i = 0; i < KF_BEAT_N; i++) {
		if (!s_beat[i].seen)
			continue;
		kf_log("VL,KF,BEAT,%s,age_ms=%u", s_beat[i].mod,
		       (unsigned)(t - s_beat[i].last_ms));
	}
	/* 环尾 16 条（从最旧到最新） */
	uint16_t idx = (s_ev_w + KF_EV_N - 16) % KF_EV_N;

	for (int i = 0; i < 16; i++) {
		struct kf_ev_ent *e = &s_ev[idx];

		if (e->t_ms)
			kf_log("VL,KF,EV,[%u]%s:%s",
			       (unsigned)e->t_ms, e->mod, e->msg);
		if (++idx == KF_EV_N)
			idx = 0;
	}
	portEXIT_CRITICAL(&s_mux);
	fflush(stdout);
}

void kf_boot_forensics(void)
{
	esp_reset_reason_t r = esp_reset_reason();
	const char *why = NULL;

	switch (r) {
	case ESP_RST_PANIC:		why = "panic";	break;
	case ESP_RST_INT_WDT:		why = "int_wdt"; break;
	case ESP_RST_TASK_WDT:		why = "task_wdt"; break;
	case ESP_RST_WDT:		why = "other_wdt"; break;
	case ESP_RST_BROWNOUT:		why = "brownout"; break;
	default:			break;
	}
	if (!why)
		return;
	/* 死前最后一屏：上个生命周期的环还在（.bss 不被复位清除的
	 * 部分——注意：电源复位会清 RAM，此时回放为空属正常；
	 * WDT/panic 软复位路径 RAM 保持，环有效） */
	printf("VL,CRASH,last=%s\r\n", why);
	fflush(stdout);
	uint16_t idx = (s_ev_w + KF_EV_N - 24) % KF_EV_N;

	for (int i = 0; i < 24; i++) {
		struct kf_ev_ent *e = &s_ev[idx];

		if (e->t_ms)
			printf("VL,CRASH,EV,[%u]%s:%s\r\n",
			       (unsigned)e->t_ms, e->mod, e->msg);
		if (++idx == KF_EV_N)
			idx = 0;
	}
	fflush(stdout);
}

bool kf_task_psram(const char *name, size_t stack_bytes,
		   void (*fn)(void *), void *arg, int prio, int core,
		   void **stk_slot, void *tcb_slot)
{
	if (!stk_slot || !tcb_slot)
		return false;
	if (!*stk_slot)
		*stk_slot = heap_caps_malloc(stack_bytes,
					     MALLOC_CAP_SPIRAM);
	if (!*stk_slot)
		return false;
	TaskHandle_t th;

	if (core < 0) {
		/* 负核号=无亲和：*PinnedToCore 变体不接受 -1（会
		 * configASSERT——V-46c bootloop 实证），走通用变体 */
		th = xTaskCreateStatic(fn, name, stack_bytes, arg, prio,
				       (StackType_t *)*stk_slot,
				       (StaticTask_t *)tcb_slot);
	} else {
		th = xTaskCreateStaticPinnedToCore(fn, name, stack_bytes,
						   arg, prio,
						   (StackType_t *)*stk_slot,
						   (StaticTask_t *)tcb_slot,
						   core);
	}
	return th != NULL;
}
