/*
 * kaleido_app.c — KApp 框架核心实现
 *
 * M0 范围：注册表 + manifest 校验/JSON 解析 + 权限 + 生命周期状态机。
 * Lua 动态 App、NVS 持久化、商店目录扫描在 M1 接入（架构 §9）。
 */
#include "kaleido_app.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "kapp";

#define KAPP_REGISTRY_MAX 32 /* M0：内置+动态合计上限，超出走 DISABLED-LRU */

static kapp_app_t *s_registry[KAPP_REGISTRY_MAX];
static size_t s_count;
static kapp_event_cb_t s_event_cb;

/* ---------------- 注册表 ---------------- */

int kapp_register(kapp_app_t *app)
{
	if (app == NULL || kapp_manifest_validate(&app->manifest) != 0) {
		return -1;
	}
	if (kapp_find(app->manifest.id) != NULL) {
		return 0; /* 幂等 */
	}
	if (s_count >= KAPP_REGISTRY_MAX) {
		ESP_LOGE(TAG, "registry full (%u)", KAPP_REGISTRY_MAX);
		return -2;
	}
	app->state = KAPP_INSTALLED;
	app->crash_count = 0;
	s_registry[s_count++] = app;
	ESP_LOGI(TAG, "registered %s v%s (perm=0x%02x src=%d)",
		 app->manifest.id, app->manifest.version,
		 app->manifest.permissions, app->manifest.source);
	return 0;
}

kapp_app_t *kapp_find(const char *id)
{
	if (id == NULL)
		return NULL;
	for (size_t i = 0; i < s_count; i++) {
		if (strncmp(s_registry[i]->manifest.id, id, KAPP_ID_MAX) == 0) {
			return s_registry[i];
		}
	}
	return NULL;
}

size_t kapp_count(void)
{
	return s_count;
}

/* ---------------- manifest ---------------- */

static bool id_wellformed(const char *id)
{
	/* 反域名：≥2 段、段内 [a-z0-9_]、总长 ≥5（"a.b" 最短形态放宽到 3） */
	size_t dots = 0;
	for (const char *p = id; *p; p++) {
		if (*p == '.') {
			dots++;
			continue;
		}
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      *p == '_')) {
			return false;
		}
	}
	return dots >= 1 && strlen(id) >= 3;
}

int kapp_manifest_validate(const kapp_manifest_t *m)
{
	if (m == NULL)
		return -1;
	if (!id_wellformed(m->id))
		return -1;
	if (m->name[0] == '\0')
		return -1;
	if (m->version[0] == '\0')
		return -1;
	if (m->api != KAPP_API_VERSION)
		return -1;
	if ((m->permissions & ~KPERM_ALL) != 0)
		return -1;
	return 0;
}

static int perm_from_string(const char *s, uint32_t *bit)
{
	struct {
		const char *name;
		uint32_t v;
	} tab[] = {
	    {"ui", KPERM_UI},
	    {"audio_play", KPERM_AUDIO_PLAY},
	    {"audio_rec", KPERM_AUDIO_REC},
	    {"net", KPERM_NET},
	    {"storage", KPERM_STORAGE},
	    {"sensor", KPERM_SENSOR},
	    {"accessory", KPERM_ACCESSORY},
	    {"system", KPERM_SYSTEM},
	};
	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
		if (strcmp(s, tab[i].name) == 0) {
			*bit = tab[i].v;
			return 0;
		}
	}
	return -1;
}

int kapp_manifest_parse_json(const char *json, size_t len, kapp_manifest_t *out)
{
	if (json == NULL || out == NULL)
		return -1;
	memset(out, 0, sizeof(*out));

	cJSON *root = cJSON_ParseWithLength(json, len);
	if (root == NULL)
		return -3;

	const cJSON *id = cJSON_GetObjectItem(root, "id");
	const cJSON *name = cJSON_GetObjectItem(root, "name");
	const cJSON *ver = cJSON_GetObjectItem(root, "version");
	const cJSON *api = cJSON_GetObjectItem(root, "api");

	if (!cJSON_IsString(id) || !cJSON_IsString(name) ||
	    !cJSON_IsString(ver) || !cJSON_IsNumber(api)) {
		cJSON_Delete(root);
		return -4;
	}
	if ((uint8_t)api->valueint != KAPP_API_VERSION) {
		cJSON_Delete(root);
		return -5;
	}

	strlcpy(out->id, id->valuestring, KAPP_ID_MAX);
	strlcpy(out->name, name->valuestring, KAPP_NAME_MAX);
	strlcpy(out->version, ver->valuestring, KAPP_VER_MAX);
	out->api = (uint8_t)api->valueint;

	const cJSON *entry = cJSON_GetObjectItem(root, "entry");
	if (cJSON_IsString(entry))
		strlcpy(out->entry, entry->valuestring, sizeof(out->entry));
	const cJSON *desc = cJSON_GetObjectItem(root, "desc");
	if (cJSON_IsString(desc))
		strlcpy(out->desc, desc->valuestring, KAPP_DESC_MAX);

	const cJSON *perms = cJSON_GetObjectItem(root, "permissions");
	if (cJSON_IsArray(perms)) {
		cJSON *p;
		cJSON_ArrayForEach(p, perms)
		{
			uint32_t bit;
			if (cJSON_IsString(p) &&
			    perm_from_string(p->valuestring, &bit) == 0) {
				out->permissions |= bit;
			} /* 未知权限名：忽略（向前兼容），记日志 */
		}
	}

	const cJSON *src = cJSON_GetObjectItem(root, "source");
	out->source =
	    cJSON_IsNumber(src) ? (kapp_source_t)src->valueint : KAPP_SRC_CLAW;

	cJSON_Delete(root);
	return kapp_manifest_validate(out) == 0 ? 0 : -4;
}

/* ---------------- 权限 ---------------- */

bool kapp_has_perms(const kapp_app_t *app, uint32_t want)
{
	return app != NULL && (app->manifest.permissions & want) == want;
}

/* ---------------- 生命周期 ---------------- */

void kapp_set_event_listener(kapp_event_cb_t cb)
{
	s_event_cb = cb;
}

static void notify(kapp_app_t *app, kapp_state_t from)
{
	if (s_event_cb)
		s_event_cb(app, from, app->state);
}

/* 状态推进：只允许 INSTALLED→LOADED→RUNNING⇄PAUSED 的合法路径 */
int kapp_transition(kapp_app_t *app, kapp_state_t target)
{
	if (app == NULL)
		return -1;
	if (app->state == KAPP_DISABLED)
		return -10; /* 需用户 reenable */

	kapp_state_t from = app->state;
	while (app->state != target) {
		switch (app->state) {
		case KAPP_INSTALLED:
			if (app->on_load && app->on_load(app) != 0)
				goto fail;
			app->state = KAPP_LOADED;
			break;
		case KAPP_LOADED:
			if (target == KAPP_INSTALLED) {
				app->state = KAPP_INSTALLED;
				break;
			}
			if (app->on_start && app->on_start(app) != 0)
				goto fail;
			app->state = KAPP_RUNNING;
			break;
		case KAPP_RUNNING:
			if (target == KAPP_PAUSED) {
				if (app->on_pause)
					app->on_pause(app);
				app->state = KAPP_PAUSED;
			} else if (target == KAPP_INSTALLED) {
				if (app->on_pause)
					app->on_pause(app);
				if (app->on_stop)
					app->on_stop(app);
				app->state = KAPP_INSTALLED;
			} else { /* target==LOADED：非目标态，停在这里 */
				return 0;
			}
			break;
		case KAPP_PAUSED:
			if (target == KAPP_RUNNING) {
				if (app->on_start && app->on_start(app) != 0)
					goto fail;
				app->state = KAPP_RUNNING;
			} else if (target == KAPP_INSTALLED) {
				if (app->on_stop)
					app->on_stop(app);
				app->state = KAPP_INSTALLED;
			} else
				return 0;
			break;
		default:
			return -10;
		}
	}
	notify(app, from);
	return 0;

fail:
	app->crash_count++;
	ESP_LOGE(TAG, "%s lifecycle callback failed (crash=%u)",
		 app->manifest.id, app->crash_count);
	if (app->crash_count >= 2) {
		from = app->state;
		app->state = KAPP_DISABLED; /* 回滚保护：自动禁用 */
		notify(app, from);
	}
	return -10;
}

int kapp_reenable(kapp_app_t *app)
{
	if (app == NULL)
		return -1;
	app->crash_count = 0;
	app->state = KAPP_INSTALLED;
	return 0;
}
