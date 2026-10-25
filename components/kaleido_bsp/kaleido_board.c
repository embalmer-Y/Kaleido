/*
 * kaleido_board.c — 板级能力画像（M0：默认 Korvo-1；kaleido_v1 待 G2 引脚冻结）
 *
 * 能力位与 G1 报告 15 附节核实结果一致：
 *   Korvo-1 V1.1 = WROOM-3 同款模组 / 2×模拟麦+ES8389+2×NS4150B（非 PDM、非 I2S
 * 功放） / DVP 摄像头座 / 4.3" RGB LCD 子板 / 无磁吸 POGO。
 */
#include "kaleido_board.h"

static const kaleido_board_caps_t s_caps = {
    .board = KALEIDO_BOARD_KORVO1,
    .name = "esp32-s31-korvo-1-v1.1",
    .has_qspi_amoled = false,
    .has_pdm_mics = false,
    .has_dvp_camera = true,
    .has_audio_amp_i2s = false,
    .has_accessory_pogo = false,
};

const kaleido_board_caps_t *kaleido_board_caps(void)
{
	return &s_caps;
}
