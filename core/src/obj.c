#include "obj_priv.h"

#include <stdlib.h>
#include <string.h>

static char *xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

hb_obj *hb_obj_new_with_id(const char *type, const char *id)
{
    hb_obj *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->id = xstrdup(id);
    o->type = xstrdup(type);
    o->data = cJSON_CreateObject();
    o->vv = cJSON_CreateObject();
    if (!o->id || !o->type || !o->data || !o->vv) {
        hb_obj_free(o);
        return NULL;
    }
    return o;
}

hb_obj *hb_obj_new(const char *type)
{
    char id[37];
    hb_uuid(id);
    return hb_obj_new_with_id(type, id);
}

hb_obj *hb_obj_copy(const hb_obj *o)
{
    hb_obj *c;
    if (!o)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->id = xstrdup(o->id);
    c->type = xstrdup(o->type);
    c->data = cJSON_Duplicate(o->data, 1);
    c->vv = cJSON_Duplicate(o->vv, 1);
    c->updated_at = o->updated_at;
    c->tombstone = o->tombstone;
    if (!c->id || !c->type || !c->data || !c->vv) {
        hb_obj_free(c);
        return NULL;
    }
    return c;
}

hb_obj *hb_obj_clone_new(const hb_obj *o)
{
    hb_obj *c;
    char id[37];
    if (!o)
        return NULL;
    hb_uuid(id);
    c = hb_obj_new_with_id(o->type, id);
    if (!c)
        return NULL;
    cJSON_Delete(c->data);
    c->data = cJSON_Duplicate(o->data, 1);
    if (!c->data) {
        hb_obj_free(c);
        return NULL;
    }
    /* 克隆的对象是新对象：清掉 _fv，put 时按全字段变更重新生成 */
    cJSON_DeleteItemFromObjectCaseSensitive(c->data, "_fv");
    return c;
}

void hb_obj_free(hb_obj *o)
{
    if (!o)
        return;
    free(o->id);
    free(o->type);
    cJSON_Delete(o->data);
    cJSON_Delete(o->vv);
    free(o);
}

void hb_obj_list_free(hb_obj **v, size_t n)
{
    if (!v)
        return;
    for (size_t i = 0; i < n; i++)
        hb_obj_free(v[i]);
    free(v);
}

const char *hb_obj_id(const hb_obj *o)         { return o ? o->id : NULL; }
const char *hb_obj_type(const hb_obj *o)       { return o ? o->type : NULL; }
int64_t     hb_obj_updated_at(const hb_obj *o) { return o ? o->updated_at : 0; }
bool        hb_obj_tombstone(const hb_obj *o)  { return o ? o->tombstone != 0 : false; }

static const cJSON *get_item(const hb_obj *o, const char *key)
{
    if (!o || !o->data || !key)
        return NULL;
    return cJSON_GetObjectItemCaseSensitive(o->data, key);
}

const char *hb_obj_get_str(const hb_obj *o, const char *key)
{
    const cJSON *it = get_item(o, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

int64_t hb_obj_get_int(const hb_obj *o, const char *key, int64_t def)
{
    const cJSON *it = get_item(o, key);
    return cJSON_IsNumber(it) ? (int64_t)it->valuedouble : def;
}

double hb_obj_get_double(const hb_obj *o, const char *key, double def)
{
    const cJSON *it = get_item(o, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

bool hb_obj_get_bool(const hb_obj *o, const char *key, bool def)
{
    const cJSON *it = get_item(o, key);
    if (cJSON_IsBool(it))
        return cJSON_IsTrue(it);
    return def;
}

size_t hb_obj_get_strv(const hb_obj *o, const char *key, const char ***out)
{
    const cJSON *arr, *it;
    const char **v;
    size_t n = 0, i = 0;
    *out = NULL;
    arr = get_item(o, key);
    if (!cJSON_IsArray(arr))
        return 0;
    cJSON_ArrayForEach(it, arr)
        n++;
    v = malloc(n * sizeof *v);
    if (!v)
        return 0;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it))
            v[i++] = it->valuestring;
    }
    *out = v;
    return i;
}

static bool key_reserved(const char *key)
{
    return !key || key[0] == '_';
}

void hb_obj_set_str(hb_obj *o, const char *key, const char *val)
{
    if (!o || key_reserved(key))
        return;
    if (!val) {
        cJSON_DeleteItemFromObjectCaseSensitive(o->data, key);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, key);
    cJSON_AddItemToObject(o->data, key, cJSON_CreateString(val));
}

void hb_obj_set_int(hb_obj *o, const char *key, int64_t val)
{
    if (!o || key_reserved(key))
        return;
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, key);
    cJSON_AddItemToObject(o->data, key, cJSON_CreateNumber((double)val));
}

void hb_obj_set_double(hb_obj *o, const char *key, double val)
{
    if (!o || key_reserved(key))
        return;
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, key);
    cJSON_AddItemToObject(o->data, key, cJSON_CreateNumber(val));
}

void hb_obj_set_bool(hb_obj *o, const char *key, bool val)
{
    if (!o || key_reserved(key))
        return;
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, key);
    cJSON_AddItemToObject(o->data, key,
                          val ? cJSON_CreateTrue() : cJSON_CreateFalse());
}

void hb_obj_set_strv(hb_obj *o, const char *key, const char *const *v, size_t n)
{
    cJSON *arr;
    if (!o || key_reserved(key))
        return;
    arr = cJSON_CreateArray();
    if (!arr)
        return;
    for (size_t i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(v[i]));
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, key);
    cJSON_AddItemToObject(o->data, key, arr);
}

bool hb_obj_has(const hb_obj *o, const char *key)
{
    if (!o || !key)
        return false;
    return cJSON_GetObjectItemCaseSensitive(o->data, key) != NULL;
}

char *hb_obj_to_json(const hb_obj *o)
{
    if (!o || !o->data)
        return NULL;
    return cJSON_Print(o->data);
}
