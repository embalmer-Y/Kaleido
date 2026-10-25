/*
 * kaleido_board.h — 板级抽象
 *
 * 三板型：KALEIDO_BOARD_P4C5_OSPTEK（主开发板：鱼鹰光电 P4C5 10.1" 套件）
 *        KALEIDO_BOARD_KORVO1（已故验证板，W-7 flash 物理故障返修中）
 *        KALEIDO_BOARD_KALEIDO_V1（自制样机，引脚表待冻结）
 */
#ifndef KALEIDO_BOARD_H
#define KALEIDO_BOARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	KALEIDO_BOARD_KORVO1 = 0, /* 已故验证板（W-7） */
	KALEIDO_BOARD_P4C5_OSPTEK, /* 主开发板（D-8 平台切换） */
	KALEIDO_BOARD_KALEIDO_V1,
} kaleido_board_t;

/* 板级能力描述（KApp 框架 sensor 权限裁剪、KShell 布局自适应用） */
typedef struct {
	kaleido_board_t board;
	const char *name;
	bool has_qspi_amoled; /* korvo1: false（RGB LCD 子板）; kaleido_v1: true
			       */
	bool
	    has_pdm_mics; /* korvo1/p4c5: false（模拟麦）; kaleido_v1: true */
	bool has_dvp_camera; /* p4c5: false（MIPI-CSI 座，未上车） */
	bool has_audio_amp_i2s; /* korvo1/p4c5: false（NS4150 模拟）;
				   kaleido_v1: true（TAS5805M） */
	bool has_accessory_pogo; /* 双 POGO 位（顶部/底部）仅 kaleido_v1 */
	bool has_mipi_dsi_screen; /* p4c5: 10.1" 800×1280 JD9366 in-cell 触摸 */
	bool has_c5_wifi;	 /* p4c5: ESP32-C5HF4，SDIO + esp_hosted */
	bool has_tf_card;	 /* p4c5: microSD 卡槽 */
} kaleido_board_caps_t;

const kaleido_board_caps_t *kaleido_board_caps(void);

/* 其余（引脚表/电源树/外设句柄）随引脚表冻结后补充实现 */

#ifdef __cplusplus
}
#endif

#endif /* KALEIDO_BOARD_H */
