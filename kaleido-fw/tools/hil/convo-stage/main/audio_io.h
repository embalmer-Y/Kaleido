/*
 * audio_io.h — ES8311 采集/播放（osptek P4C5 板，P4 平台专用）
 *
 * 板级事实（26 号调研 A1，原理图矢量走线三重验证）：
 *   I2C SDA=7 SCL=8（与 in-cell 触摸共总线）；I2S MCLK=13 BCLK=12
 *   WS=10 DOUT=9（P4→DAC）DIN=11（ADC→P4）；NS4150 PA=GPIO53。
 * S3 Korvo-1 时代的 ES8389 版本冻结在 tools/hil/voice-loop（历史证据）。
 *
 * 并发契约（V-46a）：采集=常驻任务持 I2S RX 写 PSRAM 环；各消费者持
 * 独立游标 aio_capture_stream() 并行取流、互不偷数据（掉队超半环
 * 自跳最新）。播放仍内部互斥（单一写者）。
 */
#ifndef VOICE_LOOP_P4_AUDIO_IO_H
#define VOICE_LOOP_P4_AUDIO_IO_H

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"

i2c_master_bus_handle_t aio_i2c_bus(void);	/* 共享 I2C0（触摸复用）*/

int aio_init(void);                    /* codec + I2S 全双工对         */
/* 采集 mono 16k PCM（stereo 帧内取左声道）；返回样本数。
 * cursor 为消费者私有游标（初值 0=从最新开始），跨调用保持连续流 */
int aio_capture_stream(int16_t *mono, size_t samples, uint32_t timeout_ms,
		       uint32_t *cursor);
/* 播放 mono 16k PCM（内部复制到双声道槽）；返回写入样本数 */
int aio_play(const int16_t *mono, size_t samples, uint32_t timeout_ms);

/* V-46c 打断：置位后 aio_play 在 100ms 块边界尽早返回 */
void aio_tx_abort(bool on);

/* V-46b AEC 参考：绝对样本位置（TX 已写入 DMA 的累计帧数）与读取。
 * 位置与 aio_capture_stream 游标同空间但不等相位——偏移实测（'R'） */
uint32_t aio_ref_now(void);
bool aio_ref_get(int16_t *out, size_t n, uint32_t pos);
/* 跨步版：从 pos 起每 stride 样本取 1 写入 out（最多 n_out 个），
 * 返回实际写入数；起点陈旧（落后半环）返回 0 */
size_t aio_ref_get_strided(int16_t *out, size_t n_out, uint32_t pos,
			   size_t stride);

#endif /* VOICE_LOOP_P4_AUDIO_IO_H */
