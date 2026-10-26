/*
 * audio_io.h — ES8311 采集/播放（osptek P4C5 板，P4 平台专用）
 *
 * 板级事实（26 号调研 A1，原理图矢量走线三重验证）：
 *   I2C SDA=7 SCL=8（与 in-cell 触摸共总线）；I2S MCLK=13 BCLK=12
 *   WS=10 DOUT=9（P4→DAC）DIN=11（ADC→P4）；NS4150 PA=GPIO53。
 * S3 Korvo-1 时代的 ES8389 版本冻结在 tools/hil/voice-loop（历史证据）。
 *
 * 并发契约：capture/play 各自内部串行（互斥锁），但同一函数的并发
 * 调用者会互相排队——采音/播放的"单所有权"由调用方（轮编排）保证。
 */
#ifndef VOICE_LOOP_P4_AUDIO_IO_H
#define VOICE_LOOP_P4_AUDIO_IO_H

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"

i2c_master_bus_handle_t aio_i2c_bus(void);	/* 共享 I2C0（触摸复用）*/

int aio_init(void);                    /* codec + I2S 全双工对         */
/* 采集 mono 16k PCM（stereo 帧内取左声道）；返回样本数 */
int aio_capture(int16_t *mono, size_t samples, uint32_t timeout_ms);
/* 播放 mono 16k PCM（内部复制到双声道槽）；返回写入样本数 */
int aio_play(const int16_t *mono, size_t samples, uint32_t timeout_ms);

#endif /* VOICE_LOOP_P4_AUDIO_IO_H */
