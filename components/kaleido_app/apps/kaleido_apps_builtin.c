/*
 * kaleido_apps_builtin.c — 出厂内置 App（C 静态注册）
 *
 * M0 提供最小可跑集：时钟、设置占位。相机/音乐/Muse avatar 等随 BSP 就绪在 M1/M2 增补。
 */
#include "kaleido_app.h"

#include "esp_log.h"

static const char *TAG = "kapp.builtin";

/* ---- 时钟 ---- */
static int clock_on_start(kapp_app_t *app)
{
    ESP_LOGI(TAG, "clock: start（M1 接 LVGL 表盘）");
    (void)app;
    return 0;
}
static void clock_on_pause(kapp_app_t *app)
{
    ESP_LOGI(TAG, "clock: pause");
    (void)app;
}

/* ---- 设置 ---- */
static int settings_on_start(kapp_app_t *app)
{
    ESP_LOGI(TAG, "settings: start（M1 接 KShell 设置页）");
    (void)app;
    return 0;
}

kapp_app_t *kaleido_apps_builtin_list[] = {
    &(kapp_app_t){
        .manifest = {
            .id = "com.kaleido.clock",
            .name = "时钟",
            .version = "1.0.0",
            .api = KAPP_API_VERSION,
            .entry = "c",
            .permissions = KPERM_UI,
            .source = KAPP_SRC_BUILTIN,
            .desc = "桌面时钟表盘",
        },
        .on_start = clock_on_start,
        .on_pause = clock_on_pause,
    },
    &(kapp_app_t){
        .manifest = {
            .id = "com.kaleido.settings",
            .name = "设置",
            .version = "1.0.0",
            .api = KAPP_API_VERSION,
            .entry = "c",
            .permissions = KPERM_UI | KPERM_SYSTEM,
            .source = KAPP_SRC_BUILTIN,
            .desc = "系统设置",
        },
        .on_start = settings_on_start,
    },
};

void kaleido_apps_builtin_register(void)
{
    for (size_t i = 0; i < sizeof(kaleido_apps_builtin_list) / sizeof(kaleido_apps_builtin_list[0]); i++) {
        kapp_register(kaleido_apps_builtin_list[i]);
    }
}
