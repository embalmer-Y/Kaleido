/*
 * kaleido_board.h — 板级抽象
 *
 * 双实现：KALEIDO_BOARD_KORVO1（验证板 ESP32-S31-Korvo-1 V1.1）
 *         KALEIDO_BOARD_KALEIDO_V1（自制样机，引脚表待 G2 冻结——G1 报告 15 §GPIO 警示：
 *         摄像头上车后全项目余量仅 4-5 脚，此表冻结前不得投板）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KALEIDO_BOARD_KORVO1 = 0,
    KALEIDO_BOARD_KALEIDO_V1,
} kaleido_board_t;

/* 板级能力描述（KApp 框架 sensor 权限裁剪、KShell 布局自适应用） */
typedef struct {
    kaleido_board_t board;
    const char *name;
    bool has_qspi_amoled;   /* korvo1: false（RGB LCD 子板）; kaleido_v1: true  */
    bool has_pdm_mics;      /* korvo1: false（模拟麦+ES8389）; kaleido_v1: true */
    bool has_dvp_camera;
    bool has_audio_amp_i2s; /* korvo1: false（NS4150 模拟）; kaleido_v1: true（TAS5805M） */
    bool has_accessory_pogo;/* 双 POGO 位（顶部/底部）仅 kaleido_v1 */
} kaleido_board_caps_t;

const kaleido_board_caps_t *kaleido_board_caps(void);

/* 其余（引脚表/电源树/外设句柄）随 G2 冻结引脚分配表后补充实现 */

#ifdef __cplusplus
}
#endif
