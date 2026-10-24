/*
 * main.c — Kaleido-Claw 系统装配（M0 骨架）
 *
 * 启动顺序依架构文档 §8；M0 仅装配 KApp 框架并冒烟：
 * 注册内置 App → 全部推进 RUNNING → 打印注册表。
 * 显示/音频/Claw/Muse 随 M1/M2 逐层接入。
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "kaleido_app.h"
#include "kaleido_board.h"

static const char *TAG = "kaleido";

void kaleido_apps_builtin_register(void);  /* kaleido_app 组件 */

static void on_app_event(kapp_app_t *app, kapp_state_t from, kapp_state_t to)
{
    ESP_LOGI(TAG, "[app] %s: %d → %d", app->manifest.id, from, to);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Kaleido-Claw M0 boot (board=%s)", kaleido_board_caps()->name);

    kapp_set_event_listener(on_app_event);
    kaleido_apps_builtin_register();

    /* 内置清单启动（M1 改为 KShell 网格懒加载） */
    kapp_app_t *clock = kapp_find("com.kaleido.clock");
    if (clock) kapp_transition(clock, KAPP_RUNNING);

    kapp_app_t *settings = kapp_find("com.kaleido.settings");
    if (settings) kapp_transition(settings, KAPP_RUNNING);

    ESP_LOGI(TAG, "registry: %u app(s) — M0 skeleton OK", kapp_count());
}
