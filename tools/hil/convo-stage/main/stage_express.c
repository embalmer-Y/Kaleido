/*
 * stage_express.c — 表达桥实现（V-45 L0：四态可辨的色环语言）
 *
 * 视觉口径（无 CJK 字体的 L0 约定，词表/口型属 M1.6 后续）：
 *   UNCONFIGURED  白环慢闪（开箱引导：还差一把钥匙）
 *   IDLE          蓝灰环，呼吸式透明度起伏
 *   LISTENING     青色弧段持续增长扫圈（在听）
 *   THINKING      橙色小弧段旋转（在想）
 *   SPEAKING      绿色整环脉冲（在说）
 * 环挂在舞台（325² canvas）外圈，直径 356px 居中。
 *
 * 数据流：LVGL 定时器 100ms 轮询 ①ksec_state()（无 key → 引导态）
 * ②convo_state_get()（会话四态）→ kex_set_state()（表达态唯一持有者）
 * → kex 回调驱动本文件的手工动画参数。轮询在 LVGL 上下文，天然串行。
 */
#include <math.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "convo_fsm.h"
#include "kaleido_express.h"
#include "kaleido_secret.h"
#include "stage_express.h"

static const char *TAG = "kex-bridge";

#define RING_DIAM	356	/* 舞台外圈（canvas 325² + 31px 余量） */

static lv_obj_t *s_ring;
static lv_timer_t *s_timer;

/* ---- 手工动画：定时器里按状态推进参数 ---- */

static void ring_style(enum kex_state st, int64_t now_us)
{
	static const uint32_t col[] = {
		[KEX_UNCONFIGURED] = 0xFFFFFF,
		[KEX_IDLE] = 0x5A6B85,
		[KEX_LISTENING] = 0x22C5D8,
		[KEX_THINKING] = 0xF5A623,
		[KEX_SPEAKING] = 0x35C46A,
	};
	int phase = (int)((now_us / 1000) % 1600);	/* 1.6s 节拍 */

	lv_obj_set_style_arc_color(s_ring, lv_color_hex(col[st]), LV_PART_MAIN);
	lv_obj_set_style_arc_color(s_ring, lv_color_hex(col[st]),
				   LV_PART_INDICATOR);
	lv_arc_set_range(s_ring, 0, 360);
	switch (st) {
	case KEX_UNCONFIGURED:	/* 慢闪 */
		lv_arc_set_value(s_ring, 360);
		lv_obj_set_style_arc_opa(s_ring,
					 phase < 800 ? LV_OPA_70 : LV_OPA_10,
					 LV_PART_INDICATOR);
		break;
	case KEX_IDLE:		/* 呼吸 */
		lv_arc_set_value(s_ring, 360);
		lv_obj_set_style_arc_opa(s_ring,
					 LV_OPA_20 + (LV_OPA_50 * (phase + 200)) / 1600,
					 LV_PART_INDICATOR);
		break;
	case KEX_LISTENING:	/* 增长扫圈 */
		lv_arc_set_value(s_ring, (phase * 360) / 1600);
		lv_obj_set_style_arc_opa(s_ring, LV_OPA_80,
					 LV_PART_INDICATOR);
		break;
	case KEX_THINKING: {	/* 旋转小弧 */
		int base = (phase * 360) / 1600;

		lv_arc_set_bg_angles(s_ring, base, base + 70);
		lv_arc_set_value(s_ring, 360);
		lv_obj_set_style_arc_opa(s_ring, LV_OPA_80,
					 LV_PART_INDICATOR);
		break;
	}
	case KEX_SPEAKING:	/* 脉冲整环 */
		lv_arc_set_value(s_ring, 360);
		lv_obj_set_style_arc_opa(s_ring,
					 phase < 400 ? LV_OPA_90 :
					 phase < 800 ? LV_OPA_40 :
					 phase < 1200 ? LV_OPA_90 : LV_OPA_40,
					 LV_PART_INDICATOR);
		break;
	}
}

/* ---- 状态来源轮询 ---- */

static enum kex_state target_state(void)
{
	/* OOBE 优先：云端 key 未配齐 → 引导态（方案 §9 第一列） */
	if ((ksec_state() & (KSEC_BIT_ASR | KSEC_BIT_LLM)) !=
	    (KSEC_BIT_ASR | KSEC_BIT_LLM))
		return KEX_UNCONFIGURED;
	switch (convo_state_get()) {
	case CONVO_LISTENING:
		return KEX_LISTENING;
	case CONVO_THINKING:
		return KEX_THINKING;
	case CONVO_SPEAKING:
		return KEX_SPEAKING;
	default:	/* OFF（FSM 未启/菜单模式）与 IDLE 同视觉 */
		return KEX_IDLE;
	}
}

static void bridge_timer_cb(lv_timer_t *t)
{
	(void)t;
	kex_set_state(target_state());
	ring_style(kex_get_state(), esp_timer_get_time());
}

/* kex 回调（在 bridge_timer_cb 上下文执行，LVGL 安全）：
 * 转换计数打串口，供门禁判分 */
static void on_kex_state(void *user, enum kex_state st)
{
	(void)user;
	printf("VL,KEX,%s,n=%d\r\n", kex_state_name(st),
	       kex_transition_count());
	fflush(stdout);
}

int stage_express_init(void)
{
	lv_obj_t *scr = lv_screen_active();

	s_ring = lv_arc_create(scr);
	lv_obj_set_size(s_ring, RING_DIAM, RING_DIAM);
	lv_obj_align(s_ring, LV_ALIGN_CENTER, 0, 0);
	lv_arc_set_rotation(s_ring, 270);
	lv_arc_set_bg_angles(s_ring, 0, 360);
	lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);	/* 无旋钮 */
	lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
	lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(s_ring, lv_color_hex(0x1A2332),
				   LV_PART_MAIN);
	lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

	kex_init(on_kex_state, NULL);
	s_timer = lv_timer_create(bridge_timer_cb, 100, NULL);
	if (!s_timer) {
		ESP_LOGE(TAG, "bridge timer fail");
		return -1;
	}
	return 0;
}
