/*
 * lottie_stage.c — 角色舞台 L0：pthread 渲染（thorvg 1.1.2）+ LVGL 合成并行
 *
 * 30fps 架构（V-35R + 30fps 冲刺）：
 *   渲染 pthread：325² ARGB8888 三缓冲（PSRAM）按 30fps 节拍渲染，
 *     只写"既不在屏也非就绪"的缓冲，完成即置就绪标志。
 *     每帧先 memset 清缓冲——ThorVG 不清屏，移动元素的旧像素
 *     会残留成彩块重影（V-39 照片实锤）。
 *   LVGL 16ms 定时器：任务内采纳就绪帧，走 lv_canvas 换缓冲 + 
 *     invalidate（官方 esp_lv_lottie_player 同款合成路径，
 *     绕开 lv_image 换源指针的缓存/双 fb 同步盲区）。
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "thorvg_capi.h"

extern esp_lcd_panel_handle_t lcd_panel;
extern const char test_lottie_json_start[] asm("_binary_test_lottie_json_start");
extern const char test_lottie_json_end[] asm("_binary_test_lottie_json_end");

static const char *TAG = "stage";

#define STAGE_W		325
#define STAGE_H		325
#define STAGE_FPS	30

#define STAGE_NBUF	3
static uint32_t *s_buf[STAGE_NBUF];
/* 邮箱：s_ready_idx>=0 且 !=s_disp_idx 表示有就绪帧；
 * 渲染写"既不在屏也非就绪"的缓冲，LVGL 采纳就绪帧后置 -1。
 * 三缓冲消除双缓冲下的生产/消费相位失锁（实测渲染率腰斩） */
static volatile int s_ready_idx = -1;
static volatile int s_disp_idx;
static lv_obj_t *s_img;	/* lv_canvas：合成走官方播放器验证路径 */
static lv_timer_t *s_stage_timer;
static volatile int s_frozen;
static volatile int s_render_cnt;
static volatile int s_disp_cnt;
static volatile int s_adopted;	/* 当前展示的缓冲下标 */

static void *stage_pthread(void *arg)
{
	size_t json_len = test_lottie_json_end - test_lottie_json_start;
	char *json = malloc(json_len + 1);
	if (!json)
		return NULL;
	memcpy(json, test_lottie_json_start, json_len);
	json[json_len] = '\0';

	if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
		printf("DSI,STAGE,ERROR=engine\r\n");
		fflush(stdout);
		free(json);
		return NULL;
	}
	Tvg_Canvas canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);

	if (!canvas) {
		printf("DSI,STAGE,ERROR=canvas\r\n");
		fflush(stdout);
		free(json);
		return NULL;
	}
	Tvg_Animation anim = tvg_lottie_animation_new();
	Tvg_Paint pic = tvg_animation_get_picture(anim);

	if (tvg_picture_load_data(pic, json, json_len, "lot", NULL,
				  true) != TVG_RESULT_SUCCESS) {
		printf("DSI,STAGE,ERROR=load\r\n");
		fflush(stdout);
		free(json);
		return NULL;
	}
	free(json);
	tvg_picture_set_size(pic, (float)STAGE_W, (float)STAGE_H);
	tvg_canvas_add(canvas, pic);

	float total_f = 0;

	tvg_animation_get_total_frame(anim, &total_f);
	int total = (total_f > 0) ? (int)total_f : 90;
	printf("DSI,STAGE,START,%dx%d@%dfps,total_frames=%d\r\n",
	       STAGE_W, STAGE_H, STAGE_FPS, total);
	fflush(stdout);

	int frame = 0;
	int64_t loop_t0 = esp_timer_get_time();
	int skips = 0, loops = 0;
	int64_t loop_sum = 0;
	for (;;) {
		if (s_frozen) {
			vTaskDelay(pdMS_TO_TICKS(50));
			continue;
		}
		int64_t t0 = esp_timer_get_time();
		int back = -1;

		for (int i = 0; i < STAGE_NBUF; i++) {
			if (i != s_disp_idx && i != s_ready_idx) {
				back = i;
				break;
			}
		}
		loops++;
		if (back < 0)
			skips++;
		if (back >= 0) {
			memset(s_buf[back], 0, STAGE_W * STAGE_H * 4);
			tvg_swcanvas_set_target(canvas, s_buf[back], STAGE_W,
						STAGE_W, STAGE_H,
						TVG_COLORSPACE_ARGB8888);
			tvg_animation_set_frame(anim,
						(float)(frame % total));
			tvg_canvas_update(canvas);
			tvg_canvas_draw(canvas, false);
			if (tvg_canvas_sync(canvas) != TVG_RESULT_SUCCESS)
				break;
			s_ready_idx = back;
			frame++;
			s_render_cnt++;
			/* 诊断：单帧渲染耗时 */
			{
				static int s_n;
				static int64_t s_sum;

				s_sum += esp_timer_get_time() - t0;
				if (++s_n % 30 == 0) {
					int64_t now = esp_timer_get_time();

					printf("DSI,STAGE,RENDER_MS,avg=%.1f,skip=%d/%d,loop_ms=%.1f\r\n",
					       (double)s_sum / s_n / 1000.0, skips, loops,
					       (double)(now - loop_t0) / loops / 1000.0);
					fflush(stdout);
					s_n = 0;
					s_sum = 0;
					skips = 0;
					loops = 0;
					loop_t0 = now;
				}
			}
		}

		int64_t el = esp_timer_get_time() - t0;
		int64_t period = 1000000LL / STAGE_FPS;

		if (el < period)
			vTaskDelay(pdMS_TO_TICKS((period - el) / 1000));
	}
	return NULL;
}

static void stage_timer_cb(lv_timer_t *t)
{
	static int sec;
	static int64_t last_cb;
	static int64_t cb_sum, cb_max;
	int64_t now = esp_timer_get_time();

	if (last_cb) {
		int64_t d = now - last_cb;

		cb_sum += d;
		if (d > cb_max)
			cb_max = d;
	}
	last_cb = now;
	s_disp_cnt++;

	/* LVGL 任务内采纳最新就绪帧：指针切换只发生在 LVGL 上下文 */
	int cand = s_ready_idx;

	if (cand >= 0 && cand != s_disp_idx) {
		lv_canvas_set_buffer(s_img, s_buf[cand], STAGE_W, STAGE_H,
				     LV_COLOR_FORMAT_ARGB8888);
		lv_obj_invalidate(s_img);
		s_disp_idx = cand;
		s_ready_idx = -1;
	}

	if (++sec % 30 == 0) {
		printf("DSI,STAGE,STAT,render=%d,disp=%d,cb_ms=%.1f/%.1f\r\n",
		       s_render_cnt, s_disp_cnt,
		       (double)cb_sum / 30 / 1000.0, (double)cb_max / 1000.0);
		fflush(stdout);
		s_render_cnt = 0;
		s_disp_cnt = 0;
		cb_sum = 0;
		cb_max = 0;
	}
}

static void stage_dump_probe(void)
{
	void *fbs[2] = {0};

	if (!lcd_panel ||
	    esp_lcd_dpi_panel_get_frame_buffer(lcd_panel, 2, &fbs[0],
					       &fbs[1]) != ESP_OK)
		return;
	static const int rows[] = { 300, 600, 680, 760 };

	for (unsigned r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
		int y = rows[r];

		for (int fb = 0; fb < 2; fb++) {
			uint16_t *a = fbs[fb];

			printf("DSI,STAGE,DUMP,fb=%d,y=%d:", fb, y);
			for (int x = 440; x < 600; x += 8)
				printf("%04X ", a[y * 800 + x]);
			printf("\r\n");
		}
	}
	fflush(stdout);
}

static void stage_console_task(void *arg)
{
	int c;

	while ((c = getchar()) != EOF) {
		if (c == 'f') {
			s_frozen = !s_frozen;
			printf("DSI,STAGE,FROZEN,%d\r\n", s_frozen);
			fflush(stdout);
		} else if (c == 'd') {
			stage_dump_probe();
		}
	}
	vTaskDelete(NULL);
}

int lottie_stage_start(void)
{
	pthread_attr_t attr;
	pthread_t th;

	for (int i = 0; i < STAGE_NBUF; i++) {
		s_buf[i] = heap_caps_malloc(STAGE_W * STAGE_H * 4,
					    MALLOC_CAP_SPIRAM);
		if (!s_buf[i]) {
			ESP_LOGE(TAG, "DSI,STAGE,ERROR=buf_alloc");
			return -1;
		}
		memset(s_buf[i], 0, STAGE_W * STAGE_H * 4);
	}
	s_disp_idx = 0;
	s_ready_idx = -1;

	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_t *title = lv_label_create(scr);

	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101828), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	s_img = lv_canvas_create(scr);
	lv_canvas_set_buffer(s_img, s_buf[0], STAGE_W, STAGE_H,
			     LV_COLOR_FORMAT_ARGB8888);
	lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 40);
	lv_label_set_text(title, "Kaleido stage L0 (parallel 30fps)");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
	lv_screen_load(scr);

	s_stage_timer = lv_timer_create(stage_timer_cb, 16, NULL);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	int rc = pthread_create(&th, &attr, stage_pthread, NULL);

	if (rc) {
		ESP_LOGE(TAG, "DSI,STAGE,ERROR=pthread rc=%d", rc);
		return -1;
	}
	pthread_detach(th);

	xTaskCreate(stage_console_task, "stage_cmd", 4096, NULL, 5, NULL);
	return 0;
}
