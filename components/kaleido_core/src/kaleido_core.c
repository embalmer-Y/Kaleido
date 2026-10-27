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
#include "portmacro.h"

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

static uint32_t now_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

void kf_init(void)
{
	if (s_inited)
		return;
	s_inited = true;
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

	if (s_dbg) {
		printf("KF,%s,%s\r\n", mod, e->msg);
		fflush(stdout);
	}
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
	printf("VL,KF,HEAP,int=%u,ps=%u\r\n",
	       (unsigned)(heap_int >> 10), (unsigned)(heap_ps >> 10));

	portENTER_CRITICAL(&s_mux);
	uint32_t t = now_ms();

	for (int i = 0; i < KF_BEAT_N; i++) {
		if (!s_beat[i].seen)
			continue;
		printf("VL,KF,BEAT,%s,age_ms=%u\r\n", s_beat[i].mod,
		       (unsigned)(t - s_beat[i].last_ms));
	}
	/* 环尾 16 条（从最旧到最新） */
	uint16_t idx = (s_ev_w + KF_EV_N - 16) % KF_EV_N;

	for (int i = 0; i < 16; i++) {
		struct kf_ev_ent *e = &s_ev[idx];

		if (e->t_ms)
			printf("VL,KF,EV,[%u]%s:%s\r\n",
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
