/*
 * aec_probe.h — V-46b AEC 参考信号探针（对齐测量 + 抑制测量 + 成本审计）
 */
#ifndef AEC_PROBE_H
#define AEC_PROBE_H

/* 'R'：参考/麦克风对齐测量——播 3 次已知脉冲，在采音环里找峰值，
 * 实测 TX DMA 深度等引入的样本偏移（AEC 喂入的对齐常数） */
void aec_align_probe(void);

/* 'Y'：AEC 抑制测量——MR 双通道 AFE（aec_init=true）边播边喂，
 * 对比 AEC 前后 RMS 得抑制量；同时产出资源/带宽成本审计 */
void aec_probe_run(void);

/* 最近一次对齐结果（样本数；<0=未测量）。V-46c 集成取用 */
int32_t aec_align_offset(void);

#endif
