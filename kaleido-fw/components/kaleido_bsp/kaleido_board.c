/*
 * kaleido_board.c — 板级能力画像
 *
 * 当前主开发板：osptek P4C5 底板（D-8 平台切换，2026-10-25 起）。
 * p4c5 = ESP32-P4(16MB flash/32MB HEX PSRAM) + C5HF4(WiFi6, esp_hosted 2.12.9)
 *        / ES8311+NS4150+单模拟麦 / 10.1" 800×1280 JD9366 MIPI-DSI（in-cell
 *        触摸 I2C GPIO7/8）/ TF 卡槽 / MIPI-CSI 座（摄像头未上车）。
 */
#include "kaleido_board.h"

static const kaleido_board_caps_t s_caps = {
    .board = KALEIDO_BOARD_P4C5_OSPTEK,
    .name = "osptek-esp32-p4c5-10.1",
    .has_qspi_amoled = false,
    .has_pdm_mics = false,
    .has_dvp_camera = false,
    .has_audio_amp_i2s = false,
    .has_accessory_pogo = false,
    .has_mipi_dsi_screen = true,
    .has_c5_wifi = true,
    .has_tf_card = true,
};

const kaleido_board_caps_t *kaleido_board_caps(void)
{
	return &s_caps;
}
