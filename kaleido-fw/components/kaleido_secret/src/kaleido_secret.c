/*
 * kaleido_secret.c — BYOK 密钥的 NVS 存取实现
 *
 * NVS namespace "ksec"。nvs_flash_init 由启动方（app_main）先行完成；
 * 本组件不自持锁：OOBE 流程"写后即重启"天然串行（见头文件线程契约）。
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "kaleido_secret.h"

#define KSEC_NS		"ksec"

static const char *TAG = "ksec";

int ksec_get(const char *key, char *out, size_t n)
{
	nvs_handle_t h;
	size_t need = n;
	int rc = -1;

	if (!key || !out || n == 0)
		return -1;
	out[0] = '\0';
	if (nvs_open(KSEC_NS, NVS_READONLY, &h) != ESP_OK)
		return -1;
	if (nvs_get_str(h, key, out, &need) == ESP_OK)
		rc = 0;
	nvs_close(h);
	return rc;
}

int ksec_set(const char *key, const char *val)
{
	nvs_handle_t h;
	int rc = -1;

	if (!key || !val || !val[0])
		return -1;
	if (nvs_open(KSEC_NS, NVS_READWRITE, &h) != ESP_OK)
		return -1;
	if (nvs_set_str(h, key, val) == ESP_OK &&
	    nvs_commit(h) == ESP_OK)
		rc = 0;
	nvs_close(h);
	if (rc)
		ESP_LOGE(TAG, "set %s fail", key);
	return rc;
}

int ksec_erase_all(void)
{
	nvs_handle_t h;
	int rc = -1;

	if (nvs_open(KSEC_NS, NVS_READWRITE, &h) != ESP_OK)
		return -1;
	if (nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK)
		rc = 0;
	nvs_close(h);
	ESP_LOGW(TAG, "all keys erased (factory reset)");
	return rc;
}

int ksec_state(void)
{
	int mask = 0;
	static const struct {
		const char *key;
		int bit;
	} map[] = {
		{ KSEC_KEY_WIFI_SSID, KSEC_BIT_WIFI },
		{ KSEC_KEY_WIFI_PASS, KSEC_BIT_WIFI },
		{ KSEC_KEY_DS, KSEC_BIT_ASR },
		{ KSEC_KEY_MM, KSEC_BIT_LLM },
	};

	/* 存在性探测：buf 传 NULL 只取所需长度，避免"值比探测缓冲长"
	 * 被误判为不存在（密钥都远超 15 字节——V-42 实测踩过） */
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		nvs_handle_t h;
		size_t need = 0;

		if (nvs_open(KSEC_NS, NVS_READONLY, &h) != ESP_OK)
			break;
		if (nvs_get_str(h, map[i].key, NULL, &need) == ESP_OK)
			mask |= map[i].bit;
		nvs_close(h);
	}
	return mask;
}

int ksec_get_fallback(const char *key, const char *dflt, char *out,
		      size_t n)
{
	if (ksec_get(key, out, n) == 0 && out[0])
		return 0;
	if (!dflt || !dflt[0]) {
		out[0] = '\0';
		return -1;
	}
	strlcpy(out, dflt, n);
	return 0;
}
