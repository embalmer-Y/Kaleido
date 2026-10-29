/*
 * panel-2in main — CO5300 AMOLED 460×460 MIPI-DSI 点屏自检（无 LVGL）
 *
 * 参数来源：osptek 官方示例 esp32p4-idf6-co5300-mipi-lvgl-port-lvgl9
 *（与本板同为 osptek 板系，DSI PHY 供电同为 LDO3@2.5V），LC 口径：
 *   - DSI 总线：1 data lane，RGB565 330Mbps / RGB888 500Mbps
 *   - 像素钟 16MHz ≈60fps（porch：h 72/4/32，v 4/2/6）
 *   - 无 RST 脚（-1）；亮度走 DCS（0x51），AMOLED 无背光 PWM
 *   - 窗口偏移：0x2A 列起 10（GRAM 480 列中取 460 有效）
 *
 * 自检画面（屏上应看到，否则 bring-up 失败）：
 *   1. 左侧 8 色条（含纯红/绿/蓝——验证 RGB 序与通道完整性）
 *   2. 右侧灰阶渐变（验证 DSI 链路稳定性：错条/雪花=lane 误码）
 *   3. 移动方块（验证帧率：60fps 下方块平滑横移）
 *   4. 串口每秒打 FPS + 帧计数
 */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_co5300.h"
#include "co5300_init_cmds.h"

#define LCD_H_RES	460
#define LCD_V_RES	460
#define DSI_LANE_MBPS	330			/* RGB565 */
#define DPI_CLK_MHZ	16
#define LDO_CHAN	3				/* LDO_VO3 → VDD_MIPI_DPHY */
#define LDO_MV		2500
#define PIN_LCD_RST	-1				/* 模组无 RST */

static const char *TAG = "p2in";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_dsi_bus_handle_t s_bus;

/* RGB565 写点（自绘路径唯一入口——渲染器图元层后续在此之上建） */
static inline void px(uint16_t *fb, int x, int y, uint16_t c)
{
	fb[y * LCD_H_RES + x] = c;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void draw_test(uint16_t *fb, int frame)
{
	/* 左 1/3：8 色条 */
	static const uint8_t bars[8][3] = {
		{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
		{255, 255, 0}, {0, 255, 255}, {255, 0, 255}, {0, 0, 0},
	};
	int bar_w = LCD_H_RES / 3 / 8;

	for (int y = 0; y < LCD_V_RES; y++)
		for (int x = 0; x < LCD_H_RES / 3; x++) {
			int b = x / bar_w;

			if (b > 7)
				b = 7;
			px(fb, x, y, rgb565(bars[b][0], bars[b][1], bars[b][2]));
		}
	/* 右 2/3：横向灰阶渐变（DIAG：任何 lane 误码在此表现为竖错纹） */
	for (int y = 0; y < LCD_V_RES; y++)
		for (int x = LCD_H_RES / 3; x < LCD_H_RES; x++) {
			uint8_t g = (uint8_t)((x - LCD_H_RES / 3) * 255 /
					      (LCD_H_RES - LCD_H_RES / 3));

			px(fb, x, y, rgb565(g, g, g));
		}
	/* 移动方块：60fps × 4px/帧 ≈ 3.6s 横穿一圈 */
	int bx = (frame * 4) % (LCD_H_RES - 40);

	for (int y = 40; y < 80; y++)
		for (int x = bx; x < bx + 40; x++)
			px(fb, x, y, rgb565(255, 128, 0));
}

void app_main(void)
{
	/* 1) DSI PHY 供电（与 convo-stage/dsi-lvgl 同板同通道） */
	esp_ldo_channel_handle_t ldo;
	esp_ldo_channel_config_t ldo_cfg = {
		.chan_id = LDO_CHAN,
		.voltage_mv = LDO_MV,
	};

	ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));
	ESP_LOGI(TAG, "DSI PHY powered (LDO%d @%dmV)", LDO_CHAN, LDO_MV);

	/* 2) DSI 总线：单 lane */
	esp_lcd_dsi_bus_config_t bus_cfg = {
		.bus_id = 0,
		.num_data_lanes = 1,
		.lane_bit_rate_mbps = DSI_LANE_MBPS,
	};
	ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &s_bus));

	/* 3) DBI 命令通道（经 DSI 总线发 DCS） */
	esp_lcd_dbi_io_config_t dbi_cfg = CO5300_PANEL_IO_DBI_CONFIG();
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_bus, &dbi_cfg, &s_io));

	/* 4) DPI 面板（官方时序） */
	esp_lcd_dpi_panel_config_t dpi_cfg = {
		.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
		.dpi_clock_freq_mhz = DPI_CLK_MHZ,
		.virtual_channel = 0,
		.in_color_format = LCD_COLOR_FMT_RGB565,
		.num_fbs = 2,
		.video_timing = {
			.h_size = LCD_H_RES,
			.v_size = LCD_V_RES,
			.hsync_back_porch = 72,
			.hsync_pulse_width = 4,
			.hsync_front_porch = 32,
			.vsync_back_porch = 4,
			.vsync_pulse_width = 2,
			.vsync_front_porch = 6,
		},
	};
	co5300_vendor_config_t vc = {
		.init_cmds = lcd_init_cmds,
		.init_cmds_size = lcd_init_cmds_size,
		.flags.use_mipi_interface = 1,
		.mipi_config = { .dsi_bus = s_bus, .dpi_config = &dpi_cfg },
	};
	esp_lcd_panel_dev_config_t panel_cfg = {
		.reset_gpio_num = PIN_LCD_RST,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
		.vendor_config = &vc,
	};
	ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel));
	ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
	ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
	/* 亮度渐进：AMOLED 直射。初始 5%——点亮瞬态是峰值电流主力，
	 * USB 供电新板曾把 3.3V 轨拉塌（Guru 后直接 POWERON=掉电复位
	 * 循环）；bring-up 稳定后再调高 */
	ESP_ERROR_CHECK(esp_lcd_panel_co5300_set_brightness(s_panel, 5));
	ESP_LOGI(TAG, "CO5300 panel up, 460x460 RGB565, FPS test starts");

	/* 5) 测试图循环：双 FB 轮换，每秒打 FPS */
	void *fbs[2];

	ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fbs[0], &fbs[1]));
	int frame = 0, flip = 0;
	int64_t t0 = esp_timer_get_time(), t_report = t0;
	int fps = 0;

	for (;;) {
		draw_test((uint16_t *)fbs[flip], frame++);
		ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
							 LCD_H_RES - 1,
							 LCD_V_RES - 1,
							 fbs[flip]));
		flip ^= 1;
		fps++;
		int64_t now = esp_timer_get_time();

		if (now - t_report >= 1000000) {
			ESP_LOGI(TAG, "FPS=%d frame=%d int_free=%uk",
				 fps, frame,
				 (unsigned)(heap_caps_get_free_size(
					 MALLOC_CAP_INTERNAL) >> 10));
			fps = 0;
			t_report = now;
		}
		/* 60fps 节拍：DPI 自扫描，这里只管生产节奏 */
		vTaskDelay(pdMS_TO_TICKS(16));
	}
}
