/*
 * audio_io.h — Korvo-1 音频采集/播放（ES8389 + I2S0 全双工）
 *
 * 板级事实承 tools/hil/wakenet-korvo1（V-04）：模拟麦 → ES8389 ADC，
 * I2S0 BCLK=3/WS=4/DOUT=5/DIN=6；本文件补 DAC 播放路径（DOUT）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

int aio_init(void);                    /* codec + I2S 全双工对         */
/* 采集 mono 16k PCM（stereo 帧内取左声道）；返回样本数 */
int aio_capture(int16_t *mono, size_t samples, uint32_t timeout_ms);
/* 播放 mono 16k PCM（内部复制到双声道槽）；返回写入样本数 */
int aio_play(const int16_t *mono, size_t samples, uint32_t timeout_ms);
