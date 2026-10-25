/*
 * kaleido_persona.c — persona.json 解析/校验 + 文件型注册表
 */
#include "kaleido_persona.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

static char s_dir[96];
static char s_active_id[KP_ID_MAX];

/* ---- 校验小件 ---- */

static bool path_safe(const char *p)
{
	if (!p[0] || p[0] == '/')
		return false;
	return !strstr(p, "..");
}

static bool num_in(const cJSON *n, double lo, double hi)
{
	return cJSON_IsNumber(n) && n->valuedouble >= lo &&
	       n->valuedouble <= hi;
}

#define REQ_STR(obj, key, dst)                                          \
	do {                                                            \
		const cJSON *_v = cJSON_GetObjectItem(obj, key);        \
		if (!_v || !cJSON_IsString(_v) || !_v->valuestring[0]) { \
			*err = key;                                     \
			return KP_E_FIELD;                              \
		}                                                       \
		strlcpy(dst, _v->valuestring, sizeof(dst));             \
	} while (0)

int kp_parse(const char *json, size_t len, struct kp_persona *out,
	     const char **err_detail)
{
	const char **err = err_detail ? err_detail : &(const char *){ 0 };
	cJSON *root;
	const cJSON *v, *voice, *arr;

	if (err_detail)
		*err_detail = "";
	memset(out, 0, sizeof(*out));
	root = cJSON_ParseWithLength(json, len);
	if (!root) {
		if (err_detail)
			*err_detail = "json";
		return KP_E_PARSE;
	}
	REQ_STR(root, "id", out->id);
	REQ_STR(root, "name", out->name);
	REQ_STR(root, "version", out->version);

	v = cJSON_GetObjectItem(root, "character_card");
	if (v) {
		const cJSON *sp = cJSON_GetObjectItem(v,
						      "system_prompt");

		if (sp && cJSON_IsString(sp)) {
			if (!path_safe(sp->valuestring)) {
				cJSON_Delete(root);
				if (err_detail)
					*err_detail = "system_prompt";
				return KP_E_PATH;
			}
			strlcpy(out->system_prompt_ref, sp->valuestring,
				sizeof(out->system_prompt_ref));
		}
	}

	voice = cJSON_GetObjectItem(root, "voice");
	if (!voice) {
		cJSON_Delete(root);
		if (err_detail)
			*err_detail = "voice";
		return KP_E_FIELD;
	}
	{
		const cJSON *fv = cJSON_GetObjectItem(voice,
						      "fallback_voice_id");

		if (!fv || !cJSON_IsString(fv) || !fv->valuestring[0]) {
			cJSON_Delete(root);   /* 兜底音色必填：25号 §2.4 */
			if (err_detail)
				*err_detail = "voice.fallback_voice_id";
			return KP_E_FIELD;
		}
		strlcpy(out->voice.fallback_voice_id,
			fv->valuestring, KP_VOICE_MAX);
	}
	v = cJSON_GetObjectItem(voice, "provider");
	if (v && cJSON_IsString(v))
		strlcpy(out->voice.provider, v->valuestring,
			sizeof(out->voice.provider));
	v = cJSON_GetObjectItem(voice, "model");
	if (v && cJSON_IsString(v))
		strlcpy(out->voice.model, v->valuestring,
			sizeof(out->voice.model));
	v = cJSON_GetObjectItem(voice, "voice_id");
	if (v && cJSON_IsString(v))
		strlcpy(out->voice.voice_id, v->valuestring,
			KP_VOICE_MAX);

	v = cJSON_GetObjectItem(root, "expression");
	arr = v ? cJSON_GetObjectItem(v, "emotions") : NULL;
	if (cJSON_IsArray(arr)) {
		const cJSON *it;

		cJSON_ArrayForEach(it, arr) {
			const cJSON *tag, *asset, *loop, *fps;
			struct kp_emotion *e;

			if (out->emotion_cnt >= KP_EMOTIONS_MAX)
				break;
			tag = cJSON_GetObjectItem(it, "tag");
			asset = cJSON_GetObjectItem(it, "asset");
			if (!tag || !asset || !cJSON_IsString(tag) ||
			    !cJSON_IsString(asset)) {
				cJSON_Delete(root);
				if (err_detail)
					*err_detail = "emotions[]";
				return KP_E_FIELD;
			}
			if (!path_safe(asset->valuestring)) {
				cJSON_Delete(root);
				if (err_detail)
					*err_detail = asset->valuestring;
				return KP_E_PATH;
			}
			e = &out->emotions[out->emotion_cnt++];
			strlcpy(e->tag, tag->valuestring,
				KP_TAG_MAX);
			strlcpy(e->asset, asset->valuestring,
				sizeof(e->asset));
			loop = cJSON_GetObjectItem(it, "loop");
			e->loop = cJSON_IsTrue(loop);
			fps = cJSON_GetObjectItem(it, "fps");
			e->fps = num_in(fps, 1, 30) ?
				 (uint8_t)fps->valuedouble : 20;
		}
	}

	v = cJSON_GetObjectItem(root, "affect");
	if (v) {
		const cJSON *ini = cJSON_GetObjectItem(v, "initial");
		const cJSON *tau = cJSON_GetObjectItem(v, "tau");

		if (ini) {
			const cJSON *f;

			f = cJSON_GetObjectItem(ini, "valence");
			if (num_in(f, -1.0, 1.0))
				out->affect.valence =
					(float)f->valuedouble;
			f = cJSON_GetObjectItem(ini, "arousal");
			if (num_in(f, 0.0, 1.0))
				out->affect.arousal =
					(float)f->valuedouble;
		}
		if (tau) {
			const cJSON *f;

			f = cJSON_GetObjectItem(tau, "int");
			if (num_in(f, 1.0, 64.0))
				out->affect.tau_int = (float)f->valuedouble;
			f = cJSON_GetObjectItem(tau, "dec");
			if (num_in(f, 4.0, 256.0))
				out->affect.tau_dec = (float)f->valuedouble;
		}
	}

	v = cJSON_GetObjectItem(root, "ignition");
	if (v) {
		const cJSON *f;

		f = cJSON_GetObjectItem(v, "proactivity");
		if (num_in(f, 0, 10))
			out->ignition.proactivity =
				(uint8_t)f->valuedouble;
		f = cJSON_GetObjectItem(v, "daily_quota");
		if (num_in(f, 0, 20))
			out->ignition.daily_quota =
				(uint8_t)f->valuedouble;
	}

	cJSON_Delete(root);
	return KP_OK;
}

/* ---- 文件型注册表：<dir>/index.json = {ids:[], active:""} ---- */

int kp_registry_init(const char *registry_dir)
{
	strlcpy(s_dir, registry_dir, sizeof(s_dir));
	s_active_id[0] = '\0';
	mkdir(s_dir, 0775);

	char path[160];
	cJSON *root;
	FILE *f;

	snprintf(path, sizeof(path), "%s/index.json", s_dir);
	f = fopen(path, "r");
	if (!f)
		return 0;               /* 首次                  */
	char buf[1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);

	buf[n] = '\0';
	fclose(f);
	root = cJSON_Parse(buf);
	if (root) {
		const cJSON *a = cJSON_GetObjectItem(root, "active");

		if (a && cJSON_IsString(a))
			strlcpy(s_active_id, a->valuestring,
				sizeof(s_active_id));
		cJSON_Delete(root);
	}
	return 0;
}

static int index_rw(cJSON **out, bool write)
{
	char path[160];
	FILE *f;
	cJSON *root;

	snprintf(path, sizeof(path), "%s/index.json", s_dir);
	if (!write) {
		f = fopen(path, "r");
		*out = NULL;
		if (!f)
			return 0;
		char buf[1024];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);

		buf[n] = '\0';
		fclose(f);
		*out = cJSON_Parse(buf);
		return *out ? 0 : -1;
	}
	root = *out;
	f = fopen(path, "w");
	if (!f)
		return -1;
	{
		char *s = cJSON_PrintUnformatted(root);

		fputs(s, f);
		cJSON_free(s);
	}
	fclose(f);
	cJSON_Delete(root);
	return 0;
}

int kp_install(const struct kp_persona *p)
{
	cJSON *idx, *ids, *dup;

	if (!p || !p->id[0])
		return KP_E_FIELD;
	if (index_rw(&idx, false) < 0)
		return KP_E_IO;
	if (!idx)
		idx = cJSON_CreateObject();
	ids = cJSON_GetObjectItem(idx, "ids");
	if (!ids) {
		ids = cJSON_AddArrayToObject(idx, "ids");
	} else {
		const cJSON *it;

		cJSON_ArrayForEach(it, ids) {
			if (cJSON_IsString(it) &&
			    !strcmp(it->valuestring, p->id)) {
				cJSON_Delete(idx);
				return KP_E_EXISTS;
			}
		}
	}
	cJSON_AddItemToArray(ids, cJSON_CreateString(p->id));
	dup = idx;
	if (index_rw(&dup, true) < 0)
		return KP_E_IO;
	return KP_OK;
}

int kp_set_active(const char *id)
{
	cJSON *idx;

	if (index_rw(&idx, false) < 0 || !idx)
		return KP_E_NOTFOUND;
	cJSON *a = cJSON_GetObjectItem(idx, "active");

	if (!a)
		a = cJSON_AddStringToObject(idx, "active", id);
	else
		cJSON_SetValuestring(a, id);
	strlcpy(s_active_id, id, sizeof(s_active_id));
	if (index_rw(&idx, true) < 0)
		return KP_E_IO;
	return KP_OK;
}

int kp_active(struct kp_persona *out)
{
	char path[192], buf[1024];
	FILE *f;
	int rc;

	if (!s_active_id[0])
		return 0;
	snprintf(path, sizeof(path), "%s/%s/persona.json", s_dir,
		 s_active_id);
	f = fopen(path, "r");
	if (!f)
		return KP_E_NOTFOUND;
	{
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);

		buf[n] = '\0';
	}
	fclose(f);
	rc = kp_parse(buf, strlen(buf), out, NULL);
	return rc == KP_OK ? 1 : rc;
}

int kp_count(void)
{
	cJSON *idx = NULL;
	int n = 0;

	if (index_rw(&idx, false) < 0 || !idx)
		return 0;
	{
		const cJSON *ids = cJSON_GetObjectItem(idx, "ids");

		if (cJSON_IsArray(ids))
			n = cJSON_GetArraySize(ids);
	}
	cJSON_Delete(idx);
	return n;
}
