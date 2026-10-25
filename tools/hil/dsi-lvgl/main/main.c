/*
 * SPDX-FileCopyrightText: Copyright 2026 OSPTEK
 * SPDX-License-Identifier: CC-BY-4.0
 *
 * https://github.com/osptek
 */

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_lvgl_port.h"
#include "lv_demos.h"

#include "esp_lcd_jd9366.h"
#include "jd9366_init_cmds.h"
#include "esp_lcd_touch_jd9366.h"

/* LCD size */
#define EXAMPLE_LCD_H_RES   (800)
#define EXAMPLE_LCD_V_RES   (1280)

#if LV_COLOR_DEPTH == 16
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_FMT_RGB565)
#define BSP_LCD_COLOR_DEPTH (16)
#define LV_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#elif LV_COLOR_DEPTH == 24
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_FMT_RGB888)
#define BSP_LCD_COLOR_DEPTH (24)
#define LV_COLOR_FORMAT LV_COLOR_FORMAT_RGB888
#endif

// “VDD_MIPI_DPHY”应供电 2.5V，可从内部 LDO 稳压器或外部 LDO 芯片获取电源
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN 3 // LDO_VO3 连接至 VDD_MIPI_DPHY
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_BK_LIGHT 20
#define EXAMPLE_PIN_NUM_LCD_RST  -1

/* Touch settings */
#define EXAMPLE_TOUCH_I2C_NUM       (0)
#define EXAMPLE_TOUCH_I2C_CLK_HZ    (400000)

/* LCD touch pins */
#define EXAMPLE_TOUCH_I2C_SCL       (GPIO_NUM_8)
#define EXAMPLE_TOUCH_I2C_SDA       (GPIO_NUM_7)
#define EXAMPLE_TOUCH_RST           (GPIO_NUM_NC)
#define EXAMPLE_TOUCH_INT           (GPIO_NUM_NC)

static const char *TAG = "EXAMPLE";

/* LCD IO and panel */
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

static void example_bsp_enable_dsi_phy_power(void)
{
    // 打开 MIPI DSI PHY 的电源，使其从“无电”状态进入“关机”状态
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
#ifdef EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif
}

static void example_bsp_init_lcd_backlight(void)
{
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
#endif
}

static void example_bsp_set_lcd_backlight(uint32_t level)
{
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, level);
#endif
}

static esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    example_bsp_enable_dsi_phy_power();
    example_bsp_init_lcd_backlight();
    example_bsp_set_lcd_backlight(EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL);

    // 首先创建 MIPI DSI 总线，它还将初始化 DSI PHY
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {                    \
        .bus_id = 0,                                           \
        .num_data_lanes = 2,                                   \
        .phy_clk_src = 0,                                      \
        .lane_bit_rate_mbps = 1500,                            \
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), err, TAG, "LCD init failed");

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    // 我们使用DBI接口发送LCD命令和参数
    esp_lcd_dbi_io_config_t dbi_config = JD9366_MIPI_PANEL_IO_DBI_CONFIG();

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io_handle), err, TAG, "LCD init failed");

    // 创建JD9366控制面板
    esp_lcd_dpi_panel_config_t dpi_config = {                 \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,          \
        .dpi_clock_freq_mhz = 76,                             \
        .virtual_channel = 0,                                 \
        .in_color_format = MIPI_DPI_PX_FORMAT,                \
        .num_fbs = 2,                                         \
        .video_timing = {                                     \
            .h_size = EXAMPLE_LCD_H_RES,                      \
            .v_size = EXAMPLE_LCD_V_RES,                      \
            .hsync_back_porch = 28,                           \
            .hsync_pulse_width = 8,                           \
            .hsync_front_porch = 40,                          \
            .vsync_back_porch = 20,                           \
            .vsync_pulse_width = 8,                           \
            .vsync_front_porch = 140,                         \
        },                                                    \
    };

    jd9366_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        //.flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_COLOR_DEPTH,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_jd9366(io_handle, &panel_config, &lcd_panel), err, TAG, "LCD init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd_panel), err, TAG, "LCD init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd_panel), err, TAG, "LCD init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), err, TAG, "LCD init failed");

    // 打开背光
    example_bsp_set_lcd_backlight(EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);

    return ret;

err:
    if (lcd_panel) {
        esp_lcd_panel_del(lcd_panel);
    }
    return ret;
}

static esp_err_t app_touch_init(void)
{
    /* Initilize I2C */
    const i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = EXAMPLE_TOUCH_I2C_NUM,
        .scl_io_num = EXAMPLE_TOUCH_I2C_SCL,
        .sda_io_num = EXAMPLE_TOUCH_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_JD9366_CONFIG();
    tp_io_config.scl_speed_hz = EXAMPLE_TOUCH_I2C_CLK_HZ;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));

    /* Initialize touch HW */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_TOUCH_RST,
        .int_gpio_num = EXAMPLE_TOUCH_INT,
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
   
    return esp_lcd_touch_new_i2c_jd9366(tp_io_handle, &tp_cfg, &touch_handle);
}

static esp_err_t app_lvgl_init(void)
{
    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,         /* LVGL task priority */
        .task_stack = 4096*4,         /* LVGL 任务堆栈大小*/
        .task_affinity = -1,        /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500,   /* Maximum sleep in LVGL task */
        .timer_period_ms = 5        /* LVGL timer tick period in ms */
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = lcd_panel,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES,
        .double_buffer = true,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
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
             * 换帧与 vsync 对齐（port 仅在 avoid_tearing 时做 fb 映射，
             * 且必须搭配 direct_mode，否则 flush 死等信号量） */
            .direct_mode = true,
        }
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = true,
        }
    };

    lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

    return ESP_OK;
}

/* 'f'：自建 FPS 基准（benchmark 汇总表渲染崩溃，改用 REFR_READY 计数） */
static volatile int s_refr_cnt;
static lv_obj_t *s_fps_label;
static lv_obj_t *s_fps_arc, *s_fps_bar;

static void refr_cb(lv_event_t *e)
{
    s_refr_cnt++;
}

static volatile int s_tmr_cnt;

static void fps_scene_timer_cb(lv_timer_t *t)
{
    s_tmr_cnt++;
    /* 模拟对话 UI 负载：跳动数值 + 移动弧 + 变色条 */
    static int step;
    lv_obj_t *arc = s_fps_arc;
    lv_obj_t *bar = s_fps_bar;

    step++;
    if (arc)
        lv_arc_set_value(arc, 30 + (step * 7) % 140);
    if (bar) {
        lv_obj_set_x(bar, 50 + (step * 13) % 600);
        if (step % 50 == 0) {
            static const lv_palette_t pals[] = {
                LV_PALETTE_RED, LV_PALETTE_BLUE, LV_PALETTE_GREEN,
                LV_PALETTE_ORANGE, LV_PALETTE_PURPLE
            };
            lv_obj_set_style_bg_color(bar,
                lv_palette_main(pals[(step / 50) % 5]), 0);
        }
    }
    if (s_fps_label)
        lv_label_set_text_fmt(s_fps_label, "step %d", step);
}

static void fps_test(void);
int lottie_bench_run(void);

static void fps_auto_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    fps_test();
    /* V-35R：ThorVG/Lottie 离屏分档标定（pthread 内跑，W-6） */
    lottie_bench_run();
    vTaskDelete(NULL);
}

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
    lv_timer_t *tmr = lv_timer_create(fps_scene_timer_cb, 16, NULL);
    lv_screen_load(scr);
    lvgl_port_unlock();
    printf("DSI,FPS,scene_loaded\r\n");
    fflush(stdout);

    /* 10 秒采样（锁外，LVGL 任务才能跑）；每秒报一次 */
    for (int sec = 1; sec <= 10; sec++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("DSI,FPS,sec=%d,frames=%d,ticks=%d\r\n", sec, s_refr_cnt, s_tmr_cnt);
        fflush(stdout);
        s_refr_cnt = 0;
        s_tmr_cnt = 0;
    }
    /* 保留动画定时器：场景持续动，供人眼判闪烁/撕裂 */
    printf("DSI,FPS,DONE\r\n");
    fflush(stdout);
}

void app_main(void)
{
    /* LCD HW initialization */
    ESP_ERROR_CHECK(app_lcd_init());

    /* Touch initialization */
    ESP_ERROR_CHECK(app_touch_init());

    /* LVGL initialization */
    ESP_ERROR_CHECK(app_lvgl_init());

    /* Show LVGL objects */
    lvgl_port_lock(10000);

    // lv_demo_music();
    lv_demo_widgets();

    lvgl_port_unlock();

    printf("DSI,READY,auto fps in 2s\r\n");
    fflush(stdout);

    static StaticTask_t fps_taskbuf;
    static StackType_t fps_stack[8192];
    xTaskCreateStatic(fps_auto_task, "fpsauto", 8192, NULL, 3,
                      fps_stack, &fps_taskbuf);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
