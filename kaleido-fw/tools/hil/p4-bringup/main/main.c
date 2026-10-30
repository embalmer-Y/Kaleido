/*
 * p4-bringup main.c — osptek P4C5 板级冒烟（B-1 门禁）
 *
 * 验证链：工具链 → 构建 → ROM 模式烧写 → 启动 → 芯片/PSRAM/flash/分区自检。
 * 不初始化任何外设：屏/音频/C5 待引脚表确认后由后续 HIL 覆盖。
 */
#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_psram.h"

static const char *TAG = "bringup";

static void report_partitions(void)
{
	esp_partition_iterator_t it;

	for (it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
				     ESP_PARTITION_SUBTYPE_ANY, NULL);
	     it; it = esp_partition_next(it)) {
		const esp_partition_t *p = esp_partition_get(it);

		ESP_LOGI(TAG, "part %-10s off=0x%06" PRIx32 " size=0x%07" PRIx32,
			 p->label, p->address, p->size);
	}
	esp_partition_iterator_release(it);
}

static void heartbeat_task(void *arg)
{
	while (1) {
		ESP_LOGI(TAG, "beat up=%lus int=%uk psram=%uk",
			 (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
			 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
			 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
		vTaskDelay(pdMS_TO_TICKS(5000));
	}
}

void app_main(void)
{
	esp_chip_info_t ci;
	uint8_t mac[6] = { 0 };
	uint32_t flash_sz = 0;

	esp_chip_info(&ci);
	esp_efuse_mac_get_default(mac);
	if (esp_flash_get_size(NULL, &flash_sz) != ESP_OK)
		flash_sz = 0;

	ESP_LOGI(TAG, "P4-BRINGUP,PASS-START");
	ESP_LOGI(TAG, "idf=%s rev=v%d.%d cores=%d",
		 esp_get_idf_version(), ci.revision / 100, ci.revision % 100,
		 ci.cores);
	ESP_LOGI(TAG, "flash=%uMB psram=%uMB",
		 (unsigned)(flash_sz / (1024U * 1024U)),
		 (unsigned)(esp_psram_get_size() / (1024U * 1024U)));
	ESP_LOGI(TAG, "mac=%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	ESP_LOGI(TAG, "heap int=%uKB psram=%uKB largest_psram=%uKB",
		 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
		 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
		 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

	report_partitions();
	ESP_LOGI(TAG, "P4-BRINGUP,PASS-END");

	xTaskCreate(heartbeat_task, "beat", 4096, NULL, 5, NULL);
}
