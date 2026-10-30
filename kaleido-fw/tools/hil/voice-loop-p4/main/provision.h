/*
 * provision.h — OOBE Web 配置页（V-42，负责人指示：密钥经浏览器配置）
 *
 * 两种承载（同一套 HTTP 服务）：
 *   无 WiFi 凭据（首电）：SoftAP "Kaleido-OOBE" 开放网络，手机连上后
 *     浏览器打开 http://192.168.4.1；
 *   已入网（密钥轮换/补配）：同一页面跑在 STA 局域网 IP 上（菜单 'k'）。
 * 安全口径（V-1，登记于门禁）：开放 AP + 明文 HTTP，密钥在用户自家
 * 环境/局域网传输；AP 态仅在未配置时存在，STA 态服务器按需开启。
 * 产品化再上 WPA2-PIN / HTTPS。
 */
#ifndef VOICE_LOOP_P4_PROVISION_H
#define VOICE_LOOP_P4_PROVISION_H

#include <stdbool.h>

/* 启 SoftAP（开放网络）并起 HTTP 服务。返回 0 成功 */
int provision_ap_start(void);

/* 只起 HTTP 服务（跑在当前已入网的 STA 接口上） */
int provision_server_start(void);

/* 服务器是否已在跑 */
bool provision_server_running(void);

#endif /* VOICE_LOOP_P4_PROVISION_H */
