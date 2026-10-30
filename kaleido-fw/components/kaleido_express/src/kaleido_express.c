/*
 * kaleido_express.c — 表达基座 L0 实现
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"

#include "kaleido_express.h"

static const char *TAG = "kex";

static volatile enum kex_state s_state = KEX_UNCONFIGURED;
static kex_state_cb s_cb;
static void *s_cb_user;
static volatile int s_transitions;
static bool s_inited;

static const char *s_names[] = {
	"unconfigured", "idle", "listening", "thinking", "speaking",
};

int kex_init(kex_state_cb cb, void *user)
{
	s_cb = cb;
	s_cb_user = user;
	s_inited = true;
	return 0;
}

void kex_set_state(enum kex_state st)
{
	if (st == s_state)
		return;
	ESP_LOGI(TAG, "kex %s -> %s (n=%d)", kex_state_name(s_state),
		 kex_state_name(st), s_transitions + 1);
	s_state = st;
	s_transitions++;
	if (s_cb)
		s_cb(s_cb_user, st);
}

enum kex_state kex_get_state(void)
{
	return s_state;
}

const char *kex_state_name(enum kex_state st)
{
	if ((int)st < 0 || st >= sizeof(s_names) / sizeof(s_names[0]))
		return "?";
	return s_names[st];
}

int kex_transition_count(void)
{
	return s_transitions;
}
