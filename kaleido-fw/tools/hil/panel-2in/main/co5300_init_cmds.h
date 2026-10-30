/*
 * co5300_init_cmds.h — AM200MT460460FLS2 面板初始化序列
 *
 * 来源：osptek 官方示例 esp32p4-idf6-co5300-mipi-lvgl-port-lvgl9
 *（CC BY 4.0，https://gitee.com/osptek/display-amoled-2inch-460x460）。
 * 本拷贝去 lvgl.h 依赖：像素格式固定 RGB565（0x3A=0x55），要 888 时
 * 改 0x77 并同步 DSI lane 速率 330→500Mbps 与 bits_per_pixel。
 * 命令语义：FE/00 页选；3A 像素格式；53=0x20 亮度控制使能（DCS 0x51
 * 可调）；51 初值 0（亮度走 esp_lcd_panel_co5300_set_brightness）；
 * 63 HPM 限流 0xFF；2A/2B GRAM 窗口（列偏移 10——480 列 GRAM 取 460
 * 有效区）；11 睡醒（120ms 规格等待）；29 开显。
 */
#ifndef CO5300_INIT_CMDS_H
#define CO5300_INIT_CMDS_H

#include "esp_lcd_co5300.h"

#ifdef __cplusplus
extern "C" {
#endif

static co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},	/* RGB565 */
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x0A, 0x01, 0xD5}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xCB}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static const size_t lcd_init_cmds_size =
    sizeof(lcd_init_cmds) / sizeof(co5300_lcd_init_cmd_t);

#ifdef __cplusplus
}
#endif

#endif /* CO5300_INIT_CMDS_H */
