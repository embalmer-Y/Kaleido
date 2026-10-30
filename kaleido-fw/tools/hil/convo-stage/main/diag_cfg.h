/* diag_cfg.h — 缺陷 H 定位开关（临时诊断设施，结案后移除）
 * KALEIDO_DIAG_MIN=1：app_main 只保留 kf 框架 + 1Hz 心跳横幅，
 * 全部业务初始化（NVS/netif/aio 音频/LCD/LVGL/背光/WiFi/pump/
 * soul/wakenet/think）都不执行。用于把“应用内容”从崩溃因子中
 * 整体排除；对照构建必须同 sdkconfig（唯一差异是本开关）。 */
#define KALEIDO_DIAG_MIN 0
