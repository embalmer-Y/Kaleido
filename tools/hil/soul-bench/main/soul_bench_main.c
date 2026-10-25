// soul-bench: V-34 x V-35 cross stress bench (headless, Korvo-1 ESP32-S31).
// Everything runs in a dedicated bench task pinned to CPU1 with a PSRAM stack
// (validates 红线④: ThorVG big stack in external RAM, W-3 territory).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"
#include <pthread.h>
#include "bench_util.h"
#include "bench_a.h"
#include "bench_b.h"
#include "bench_c.h"

static const char *TAG = "soulbench";

#define BENCH_STACK_BYTES (64 * 1024)

/* ThorVG (espressif 组件, meson pthread 依赖) 在原生 FreeRTOS 任务中会
 * pthread_self() assert（v34_xip_on_v5 日志）——基准线程必须跑在 pthread 上。 */
static void *bench_task(void *arg) {
    ESP_LOGI(TAG, "SB,STACK,bench_task,start,hwm_bytes=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    heap_snapshot(TAG, "boot");
    ESP_LOGI(TAG, "SB,A,GROUP,START");
    bench_a1_memcpy_psram(30);
    heap_snapshot(TAG, "after_a1");
    bench_a2_cjson(100);
    bench_a3_fir(10);
    bench_a5_sha256(10);
    heap_snapshot(TAG, "after_a_group");
    ESP_LOGI(TAG, "SB,A,GROUP,DONE");
    ESP_LOGI(TAG, "SB,B,GROUP,START");
    bench_b_run(300);
    ESP_LOGI(TAG, "SB,B,GROUP,DONE");
    ESP_LOGI(TAG, "SB,C,GROUP,START");
    bench_c_run(300);
    ESP_LOGI(TAG, "SB,C,GROUP,DONE");
    heap_snapshot(TAG, "end");
    ESP_LOGI(TAG, "SB,STACK,bench_task,end,hwm_bytes=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "SB,ALL,DONE");
    return NULL;
}

void app_main(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "SB,BANNER,soul-bench,esp32s31,rev=v%d.%d,cores=%d",
             chip.revision / 100, chip.revision % 100, chip.cores);
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
    ESP_LOGI(TAG, "SB,VARIANT,xip=ON (SPIRAM_XIP_FROM_PSRAM=y)");
#else
    ESP_LOGI(TAG, "SB,VARIANT,xip=OFF (SPIRAM_XIP_FROM_PSRAM=n)");
#endif
    ESP_LOGI(TAG, "SB,BUILD,idf=%s,cpu_mhz=%d,psram=%s,psram_mode=%s,psram_speed=%d",
             esp_get_idf_version(), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             CONFIG_SPIRAM ? "y" : "n",
#ifdef CONFIG_SPIRAM_MODE_OCT
             "oct",
#else
             "qaud",
#endif
             CONFIG_SPIRAM_SPEED);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BENCH_STACK_BYTES);
    pthread_t th;
    int rc = pthread_create(&th, &attr, bench_task, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "SB,FATAL,pthread_create,rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "SB,STACK,bench_pthread,stack=%d,loc=internal", BENCH_STACK_BYTES);
}
