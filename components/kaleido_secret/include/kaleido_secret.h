/*
 * kaleido_secret.h — BYOK 密钥的 NVV 存取（OOBE 支撑层）
 *
 * 设计依据：灵魂架构方案 v1 §9（三态矩阵：无 key 引导 → 完整态）；
 * 负责人指示 2026-10-26：密钥经设备首次上电的 Web 配置页写入。
 *
 * 取值链约定（全链统一）：NVS → 编译期缺省（CONFIG_*，HIL 开发后备，
 * 出厂镜像为空）→ 空。谁消费密钥谁走 ksec_get_fallback()，不直接读 NVS。
 *
 * 线程契约：读写均为短临界操作，但调用方应自行保证不在配置写入的
 * 同时消费（OOBE 流程里写后即重启，天然串行）。
 */
#ifndef KALEIDO_SECRET_H
#define KALEIDO_SECRET_H

#include <stddef.h>

/* NVS 条目名（≤15 字符限制内） */
#define KSEC_KEY_WIFI_SSID	"wifi_ssid"
#define KSEC_KEY_WIFI_PASS	"wifi_pass"
#define KSEC_KEY_DS		"ds_key"	/* 百炼 ASR（DashScope）   */
#define KSEC_KEY_MM		"mm_key"	/* MiniMax JWT（LLM+TTS） */

/* 配置完成度掩码（ksec_state 返回） */
#define KSEC_BIT_WIFI		0x1
#define KSEC_BIT_ASR		0x2
#define KSEC_BIT_LLM		0x4

/* 读条目到 out（含 NUL）。返回 0=有值；-1=未配置/未初始化 */
int ksec_get(const char *key, char *out, size_t n);

/* 写条目（字符串）。返回 0 成功；-1 失败（NVS 满/参数空） */
int ksec_set(const char *key, const char *val);

/* 抹除全部条目：回到无 key 态（OOBE 出厂复位） */
int ksec_erase_all(void);

/* 已配置掩码：KSEC_BIT_* 按位或 */
int ksec_state(void);

/* 取值链：NVS → dflt（可为 NULL）→ 空串。返回 0=拿到非空值 */
int ksec_get_fallback(const char *key, const char *dflt, char *out,
		      size_t n);

#endif /* KALEIDO_SECRET_H */
