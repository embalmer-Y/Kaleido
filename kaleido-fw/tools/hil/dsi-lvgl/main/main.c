/*
 * main.c — dsi-lvgl HIL：osptek 10.1" JD9366（800×1280 MIPI-DSI）点屏基线
 *
 * SPDX-FileCopyrightText: Copyright 2026 OSPTEK
 * SPDX-License-Identifier: CC-BY-4.0
 * https://github.com/osptek
 *
 * 原厂例程移植，已按 docs/coding-style.md 转内核风格。
 * 启动流程：DSI 面板 → 触摸 → LVGL（direct_mode + avoid_tearing）→
 * widgets 冒烟 → 2s 后自动 FPS 基准 → 角色舞台常驻（lottie_stage.c）。
 */

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lvgl_port.h"
#include "lv_demos.h"

#include "esp_lcd_jd9366.h"
#include "jd9366_init_cmds.h"
#include "esp_lcd_touch_jd9366.h"

/* ---- 面板参数：与厂商包 vendor/tft-10.1-800x1280-mipi-jd9366 对应 ---- */
#define LCD_H_RES			800
#define LCD_V_RES			1280
/* DSI 2 lane @1500Mbps、DPI 像素钟 76MHz；porch 组来自面板 spec，
 * 错位表现为滚屏/撕裂，不要随手改（V-38 点板实测口径） */
#define LCD_DSI_LANE_MBPS		1500
#define LCD_DPI_CLK_MHZ			76

/* VDD_MIPI_DPHY 需 2.5V：走 P4 内部 LDO 通道 3（LDO_VO3） */
#define LCD_DSI_PHY_PWR_LDO_CHAN	3
#define LCD_DSI_PHY_PWR_LDO_VOLTAGE_MV	2500

#define LCD_BK_LIGHT_ON_LEVEL		1
#define LCD_BK_LIGHT_OFF_LEVEL		!LCD_BK_LIGHT_ON_LEVEL
#define LCD_PIN_NUM_BK_LIGHT		20
/* V-39：LCD_RST 必须接 GPIO5 做硬复位。曾置 -1 依靠软复位，热重启时
 * 面板残留上次寄存器态，屏上出现竖色条+高频闪烁，断电才消——面板
 * 硬复位是唯一可靠起点（见 docs/gates/V-39） */
#define LCD_PIN_NUM_RST			5

/* 触摸（I2C）：JD9366 触摸芯，RST/INT 未接 */
#define TOUCH_I2C_NUM			0
#define TOUCH_I2C_CLK_HZ		400000
#define TOUCH_I2C_SCL			(GPIO_NUM_8)
#define TOUCH_I2C_SDA			(GPIO_NUM_7)
#define TOUCH_PIN_RST			(GPIO_NUM_NC)
#define TOUCH_PIN_INT			(GPIO_NUM_NC)

#if LV_COLOR_DEPTH == 16
#define MIPI_DPI_PX_FORMAT		(LCD_COLOR_FMT_RGB565)
#define LCD_COLOR_DEPTH			(16)
#define LV_COLOR_FORMAT			LV_COLOR_FORMAT_RGB565
#elif LV_COLOR_DEPTH == 24
#define MIPI_DPI_PX_FORMAT		(LCD_COLOR_FMT_RGB888)
#define LCD_COLOR_DEPTH			(24)
#define LV_COLOR_FORMAT			LV_COLOR_FORMAT_RGB888
#endif

static const char *TAG = "bsp";

/* 面板句柄非 static：lottie_stage.c 差异诊断需读 DPI fb 指针 */
esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

static void bsp_dsi_phy_power_on(void)
{
	/* MIPI DSI PHY 上电：从无电态进入关机态，DSI 总线创建时再唤醒 */
	esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
	esp_ldo_channel_config_t ldo_cfg = {
		.chan_id = LCD_DSI_PHY_PWR_LDO_CHAN,
		.voltage_mv = LCD_DSI_PHY_PWR_LDO_VOLTAGE_MV,
	};

	ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));
	ESP_LOGI(TAG, "MIPI DSI PHY powered on");
}

static void bsp_lcd_backlight_init(void)
{
	gpio_config_t bk_gpio_cfg = {
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = 1ULL << LCD_PIN_NUM_BK_LIGHT,
	};

	ESP_ERROR_CHECK(gpio_config(&bk_gpio_cfg));
}

static void bsp_lcd_backlight_set(uint32_t on)
{
	gpio_set_level(LCD_PIN_NUM_BK_LIGHT,
		       on ? LCD_BK_LIGHT_ON_LEVEL : LCD_BK_LIGHT_OFF_LEVEL);
}

static esp_err_t app_lcd_init(void)
{
	esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
	esp_err_t ret = ESP_OK;

	bsp_dsi_phy_power_on();
	bsp_lcd_backlight_init();
	bsp_lcd_backlight_set(0);

	/* DSI 总线创建（同时初始化 PHY）→ DBI 命令通道 → DPI 面板 */
	esp_lcd_dsi_bus_config_t bus_cfg = {
		.bus_id = 0,
		.num_data_lanes = 2,
		.lane_bit_rate_mbps = LCD_DSI_LANE_MBPS,
	};
	ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus),
			  err, TAG, "create DSI bus failed");

	esp_lcd_dbi_io_config_t dbi_cfg = JD9366_MIPI_PANEL_IO_DBI_CONFIG();
	ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_cfg,
						   &io_handle),
			  err, TAG, "create DBI IO failed");

	/* num_fbs=2：esp_lvgl_port avoid_tearing 模式要求双 DPI fb 轮换 */
	esp_lcd_dpi_panel_config_t dpi_cfg = {
		.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
		.dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
		.virtual_channel = 0,
		.in_color_format = MIPI_DPI_PX_FORMAT,
		.num_fbs = 2,
		.video_timing = {
			.h_size = LCD_H_RES,
			.v_size = LCD_V_RES,
			.hsync_back_porch = 28,
			.hsync_pulse_width = 8,
			.hsync_front_porch = 40,
			.vsync_back_porch = 20,
			.vsync_pulse_width = 8,
			.vsync_front_porch = 140,
		},
	};
	jd9366_vendor_config_t vendor_cfg = {
		.init_cmds = lcd_init_cmds,
		.init_cmds_size = lcd_init_cmds_size,
		.mipi_config = {
			.dsi_bus = mipi_dsi_bus,
			.dpi_config = &dpi_cfg,
		},
	};
	const esp_lcd_panel_dev_config_t panel_cfg = {
		.reset_gpio_num = LCD_PIN_NUM_RST,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = LCD_COLOR_DEPTH,
		.vendor_config = &vendor_cfg,
	};
	ESP_GOTO_ON_ERROR(esp_lcd_new_panel_jd9366(io_handle, &panel_cfg,
						   &lcd_panel),
			  err, TAG, "create panel failed");
	ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd_panel), err, TAG,
			  "reset panel failed");
	ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd_panel), err, TAG,
			  "init panel failed");
	ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), err,
			  TAG, "disp on failed");

	/* 面板出图后再点背光，避免初始化期白屏闪 */
	bsp_lcd_backlight_set(1);
	return ret;

err:
	if (lcd_panel)
		esp_lcd_panel_del(lcd_panel);
	if (io_handle)
		esp_lcd_panel_io_del(io_handle);
	if (mipi_dsi_bus)
		esp_lcd_del_dsi_bus(mipi_dsi_bus);
	return ret;
}

static esp_err_t app_touch_init(void)
{
	i2c_master_bus_config_t i2c_bus_cfg = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = TOUCH_I2C_NUM,
		.scl_io_num = TOUCH_I2C_SCL,
		.sda_io_num = TOUCH_I2C_SDA,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t i2c_bus;

	ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));

	esp_lcd_panel_io_handle_t tp_io = NULL;
	esp_lcd_panel_io_i2c_config_t tp_io_cfg =
					ESP_LCD_TOUCH_IO_I2C_JD9366_CONFIG();

	tp_io_cfg.scl_speed_hz = TOUCH_I2C_CLK_HZ;
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

	esp_lcd_touch_config_t tp_cfg = {
		.x_max = LCD_H_RES,
		.y_max = LCD_V_RES,
		.rst_gpio_num = TOUCH_PIN_RST,
		.int_gpio_num = TOUCH_PIN_INT,
		.levels = {
			.reset = 0,
			.interrupt = 0,
		},
		.flags = {
			.swap_xy = 0,
			.mirror_x = 0,
			.mirror_y = 0,
		},
	};

	return esp_lcd_touch_new_i2c_jd9366(tp_io, &tp_cfg, &touch_handle);
}

static esp_err_t app_lvgl_init(void)
{
	/* 32K 栈：官方控件 demo 的建议值，thorvg/lottie 场景也在该任务渲染 */
	lvgl_port_cfg_t lvgl_cfg = {
		.task_priority = 4,
		.task_stack = 4096 * 8,
		.task_affinity = -1,
		.task_max_sleep_ms = 500,
		.timer_period_ms = 5,
	};

	ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG,
			    "LVGL port init failed");

	lvgl_port_display_cfg_t disp_cfg = {
		.io_handle = io_handle,
		.panel_handle = lcd_panel,
		.buffer_size = LCD_H_RES * LCD_V_RES,
		.double_buffer = true,
		.hres = LCD_H_RES,
		.vres = LCD_V_RES,
		.monochrome = false,
		.color_format = LV_COLOR_FORMAT,
		.rotation = {
			.swap_xy = false,
			.mirror_x = false,
			.mirror_y = false,
		},
		.flags = {
			.buff_dma = false,
			.buff_spiram = true,
			.sw_rotate = true,
			.swap_bytes = false,
			.full_refresh = false,
			/* 直接模式 + 防撕裂：LVGL 画进 DPI 自身帧缓冲，
			 * 换帧与 vsync 对齐。port 仅在 avoid_tearing 时做
			 * fb 映射，且必须搭配 direct_mode，否则 flush 死
			 * 等信号量（V-35R 实测口径） */
			.direct_mode = true,
		},
	};
	lvgl_port_display_dsi_cfg_t dpi_cfg = {
		.flags = {
			.avoid_tearing = true,
		},
	};

	lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
	ESP_RETURN_ON_FALSE(lvgl_disp, ESP_FAIL, TAG,
			    "add DSI display failed");

	lvgl_port_touch_cfg_t touch_cfg = {
		.disp = lvgl_disp,
		.handle = touch_handle,
	};
	lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
	ESP_RETURN_ON_FALSE(lvgl_touch_indev, ESP_FAIL, TAG,
			    "add touch failed");

	return ESP_OK;
}

/* ---- 自建 FPS 基准（自动跑，无串口命令）----
 * lv_demo_benchmark 在本板渲染汇总表时崩溃（V-35R 记录），弃用；
 * 改挂 LV_EVENT_REFR_READY 计数 + 自绘动画场景（arc/bar/label），
 * 10 秒采样后场景保留常动，供人眼判闪烁/撕裂。 */
static volatile int s_refr_cnt;
static volatile int s_tmr_cnt;
static lv_obj_t *s_fps_label;
static lv_obj_t *s_fps_arc, *s_fps_bar;

static void refr_cb(lv_event_t *e)
{
	s_refr_cnt++;
}

static void fps_scene_timer_cb(lv_timer_t *t)
{
	static int step;
	lv_obj_t *arc = s_fps_arc;
	lv_obj_t *bar = s_fps_bar;

	(void)t;
	s_tmr_cnt++;
	step++;
	if (arc)
		lv_arc_set_value(arc, 30 + (step * 7) % 140);
	if (bar) {
		lv_obj_set_x(bar, 50 + (step * 13) % 600);
		if (step % 50 == 0) {
			static const lv_palette_t pals[] = {
				LV_PALETTE_RED, LV_PALETTE_BLUE,
				LV_PALETTE_GREEN, LV_PALETTE_ORANGE,
				LV_PALETTE_PURPLE
			};

			lv_obj_set_style_bg_color(
				bar, lv_palette_main(pals[(step / 50) % 5]), 0);
		}
	}
	if (s_fps_label)
		lv_label_set_text_fmt(s_fps_label, "step %d", step);
}

int lottie_stage_start(void);

static void fps_test(void)
{
	int64_t t_lk0 = esp_timer_get_time();
	bool lk = lvgl_port_lock(10000);

	printf("DSI,FPS,lock=%d,wait_ms=%lld\r\n", lk,
	       (esp_timer_get_time() - t_lk0) / 1000);
	fflush(stdout);
	if (!lk)
		return;

	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_t *arc = lv_arc_create(scr);
	lv_obj_t *bar = lv_obj_create(scr);
	lv_obj_t *title = lv_label_create(scr);

	s_fps_label = lv_label_create(scr);
	s_fps_arc = arc;
	s_fps_bar = bar;
	lv_arc_set_range(arc, 0, 100);
	lv_obj_set_size(arc, 300, 300);
	lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 120);
	lv_obj_set_size(bar, 200, 60);
	lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -160);
	lv_label_set_text(title, "Kaleido FPS test 800x1280");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
	lv_obj_align(s_fps_label, LV_ALIGN_BOTTOM_MID, 0, -60);

	lv_display_add_event_cb(lv_display_get_default(), refr_cb,
				LV_EVENT_REFR_READY, NULL);
	lv_timer_create(fps_scene_timer_cb, 16, NULL);
	lv_screen_load(scr);
	lvgl_port_unlock();
	printf("DSI,FPS,scene_loaded\r\n");
	fflush(stdout);

	/* 10 秒采样（锁外，LVGL 任务才能跑）；每秒报一次 */
	for (int sec = 1; sec <= 10; sec++) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		printf("DSI,FPS,sec=%d,frames=%d,ticks=%d\r\n", sec,
		       s_refr_cnt, s_tmr_cnt);
		fflush(stdout);
		s_refr_cnt = 0;
		s_tmr_cnt = 0;
	}
	printf("DSI,FPS,DONE\r\n");
	fflush(stdout);
}

static void fps_auto_task(void *arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(2000));
	fps_test();

	/* 角色舞台 L0 常驻：pthread 渲染 + LVGL 合成
	 * （V-35R 的 lottie_bench 已归档，撤出自动流） */
	if (!lvgl_port_lock(10000)) {
		printf("DSI,STAGE,ERROR=lock\r\n");
		fflush(stdout);
		vTaskDelete(NULL);
		return;
	}
	lottie_stage_start();
	lvgl_port_unlock();
	vTaskDelete(NULL);
}

void app_main(void)
{
	ESP_ERROR_CHECK(app_lcd_init());
	ESP_ERROR_CHECK(app_touch_init());
	ESP_ERROR_CHECK(app_lvgl_init());

	/* widgets demo：点屏冒烟（2s 后被 FPS 场景替换） */
	lvgl_port_lock(10000);
	lv_demo_widgets();
	lvgl_port_unlock();

	printf("DSI,READY,auto fps in 2s\r\n");
	fflush(stdout);

	xTaskCreate(fps_auto_task, "fpsauto", 8192, NULL, 3, NULL);
	for (;;)
		vTaskDelay(pdMS_TO_TICKS(1000));
}
