/* hostio.c — 主机导入（ssh_config → vault）与 JSON bundle 导出/导入。
 *
 * 不含 secrets：导出不写任何加密字段；导入也只接受 data 字段。
 * 主机匹配统一大小写不敏感比较 name/address/proto（user 精确比较）。
 */

#include "harbor/hostio.h"
#include "harbor/ssh.h"          /* hb_ssh_parse_target */
#include "obj_priv.h"            /* obj->data */
#include "cJSON.h"

#include <ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------ 小工具 ------------------------------ */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void r_warn(hb_import_result *r, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    char **nv;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    nv = realloc(r->warnings, (r->nwarn + 1) * sizeof(char *));
    if (!nv) return;
    r->warnings = nv;
    r->warnings[r->nwarn] = xstrdup(msg);
    if (r->warnings[r->nwarn]) r->nwarn++;
}

static void r_id(hb_import_result *r, const char *id)
{
    char **nv = realloc(r->created_ids, (r->ncreated + 1) * sizeof(char *));
    if (!nv) return;
    r->created_ids = nv;
    r->created_ids[r->ncreated] = xstrdup(id);
    if (r->created_ids[r->ncreated]) r->ncreated++;
}

void hb_import_result_free(hb_import_result *r)
{
    if (!r) return;
    hb_strfreev(r->warnings, r->nwarn);
    hb_strfreev(r->created_ids, r->ncreated);
    memset(r, 0, sizeof *r);
}

static const char *local_user(void)
{
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name) return pw->pw_name;
    {
        const char *u = getenv("USER");
        if (u && *u) return u;
    }
    return "user";
}

/* ------------------------------ host 查找 ------------------------------ */

static hb_obj *find_by_name(hb_obj **hs, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!strcasecmp(hb_obj_get_str(hs[i], "name") ?: "", name))
            return hs[i];
    return NULL;
}

static hb_obj *find_by_id(hb_obj **hs, size_t n, const char *id)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(hb_obj_id(hs[i]), id))
            return hs[i];
    return NULL;
}

static hb_obj *find_by_tuple(hb_obj **hs, size_t n, const char *name,
                             const char *address, int64_t port,
                             const char *user, const char *proto)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const char *u = hb_obj_get_str(hs[i], "user");
        const char *pr = hb_obj_get_str(hs[i], "proto");
        if (strcasecmp(hb_obj_get_str(hs[i], "name") ?: "", name ?: ""))
            continue;
        if (strcasecmp(hb_obj_get_str(hs[i], "address") ?: "", address ?: ""))
            continue;
        if (hb_obj_get_int(hs[i], "port", 0) != port)
            continue;
        if (strcmp(u ?: "", user ?: ""))
            continue;
        if (proto && strcasecmp(pr ?: "", proto))
            continue;
        return hs[i];
    }
    return NULL;
}

static hb_obj *mk_host_obj(const char *name, const char *address,
                           int64_t port, const char *user)
{
    hb_obj *o = hb_obj_new("host");
    if (!o) return NULL;
    hb_obj_set_str(o, "name", name);
    hb_obj_set_str(o, "address", address);
    hb_obj_set_int(o, "port", port > 0 ? port : 22);
    hb_obj_set_str(o, "user", user && *user ? user : local_user());
    hb_obj_set_str(o, "proto", "ssh");
    hb_obj_set_str(o, "os", "auto");
    hb_obj_set_str(o, "auth", "auto");
    return o;
}

/* ------------------------------ ssh_config 导入 ------------------------------ */

typedef struct { const char *alias; const char *id; } aid;

/* 单个 ProxyJump 元素 → host id；无法解析返回 NULL。
 * 新建的字面量跳板主机会就地挂进 phs/pnh 匹配池。 */
static const char *resolve_jump_elem(hb_vault *v, hb_obj ***phs, size_t *pnh,
                                     size_t *pcap, aid *map, size_t nmap,
                                     const char *elem, hb_import_result *r)
{
    size_t i;
    hb_obj **hs = *phs, *o;
    size_t nh = *pnh;
    char *pu = NULL, *ph = NULL;
    int pp = 0;

    /* (a) 本配置里的别名 */
    for (i = 0; i < nmap; i++)
        if (!strcasecmp(map[i].alias, elem))
            return map[i].id;
    /* (b) vault 里已存在同名主机 */
    o = find_by_name(hs, nh, elem);
    if (o) return hb_obj_id(o);
    /* (c) 解析 [user@]host[:port] → 复用或新建 */
    if (hb_ssh_parse_target(elem, &pu, &ph, &pp) != HB_OK || !ph || !*ph) {
        free(pu); free(ph);
        r_warn(r, HB_TR("跳板 '%s' 无法解析，已忽略"), elem);
        return NULL;
    }
    if (pp <= 0) pp = 22;
    for (i = 0; i < nh; i++) {
        const char *hu = hb_obj_get_str(hs[i], "user");
        const char *ha = hb_obj_get_str(hs[i], "address");
        int64_t hp = hb_obj_get_int(hs[i], "port", 22);
        if (ha && !strcasecmp(ha, ph) && hp == pp &&
            (!pu || (hu && !strcmp(hu, pu)))) {
            free(pu); free(ph);
            return hb_obj_id(hs[i]);
        }
    }
    o = mk_host_obj(elem, ph, pp, pu);
    if (o && hb_vault_put(v, o) == HB_OK) {
        if (nh >= *pcap) {
            hb_obj **nhs = realloc(hs, (*pcap *= 2) * sizeof *nhs);
            if (nhs) { hs = nhs; *phs = nhs; }
        }
        if (nh < *pcap) {
            hs[nh] = o;
            *pnh = nh + 1;
            r->jumps_created++;
            r_id(r, hb_obj_id(o));      /* UI 撤销也要删它 */
            free(pu); free(ph);
            return hb_obj_id(o);
        }
    }
    hb_obj_free(o);
    free(pu); free(ph);
    return NULL;
}

hb_err hb_hosts_import_sshcfg(hb_vault *v, const hb_sshcfg *c,
                              hb_import_result *r)
{
    hb_obj **hs = NULL, **all = NULL;
    size_t nh = 0, ncap = 0, i;
    aid *map = NULL;
    size_t *created_idx = NULL;   /* entry i 的新建 host（在 hs 里的下标），SIZE_MAX=复用 */
    hb_err e = HB_OK;

    if (!v || !c || !r) return HB_ERR_INVALID;
    memset(r, 0, sizeof *r);
    if (hb_vault_list(v, "host", &all, &nh) != HB_OK)
        all = NULL, nh = 0;
    /* 容量预留：已有 + 可能新建的别名 + 跳板字面量 */
    ncap = nh + c->n + 64;
    hs = calloc(ncap, sizeof(hb_obj *));
    map = calloc(c->n ? c->n : 1, sizeof *map);
    created_idx = malloc(c->n * sizeof *created_idx);
    if (!hs || !map || (c->n && !created_idx)) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    memcpy(hs, all, nh * sizeof *hs);
    free(all); all = NULL;

    /* pass 1：别名 → host id（新建或复用） */
    for (i = 0; i < c->n; i++) {
        const hb_sshcfg_host *h = &c->hosts[i];
        hb_obj *ex = find_by_name(hs, nh, h->alias);
        created_idx[i] = SIZE_MAX;
        if (ex) {
            r->skipped++;
            map[i].alias = h->alias;
            map[i].id = hb_obj_id(ex);
            continue;
        }
        {
            hb_obj *o = mk_host_obj(h->alias, h->hostname, h->port,
                                    h->user);
            if (!o) { e = HB_ERR_NOMEM; goto out; }
            if (h->identity_file)
                hb_obj_set_str(o, "identity_file", h->identity_file);
            if (h->keepalive > 0)
                hb_obj_set_int(o, "keepalive", h->keepalive);
            e = hb_vault_put(v, o);
            if (e != HB_OK) { hb_obj_free(o); goto out; }
            r->created++;
            r_id(r, hb_obj_id(o));
            if (nh < ncap) hs[nh++] = o; else hb_obj_free(o);
            map[i].alias = h->alias;
            map[i].id = hb_obj_id(hs[nh - 1]);
            created_idx[i] = nh - 1;
        }
    }

    /* pass 2：仅新建主机解析 jump（外→内） */
    for (i = 0; i < c->n; i++) {
        const hb_sshcfg_host *h = &c->hosts[i];
        hb_obj *o;
        const char **jids;
        size_t nj = 0, k;
        if (created_idx[i] == SIZE_MAX || !h->proxy_jump) continue;
        o = hs[created_idx[i]];
        jids = calloc(1, sizeof *jids);
        for (k = 0; h->proxy_jump[k]; k++) {
            const char *id;
            if (!jids) break;
            id = resolve_jump_elem(v, &hs, &nh, &ncap, map, c->n,
                                   h->proxy_jump[k], r);
            /* resolve 可能 realloc hs —— o 指针仍有效（hb_obj 本体不动） */
            if (!id) continue;
            if (!strcmp(id, hb_obj_id(o))) {
                r_warn(r, HB_TR("%s: 跳板 '%s' 指向自身，已忽略"),
                       h->alias, h->proxy_jump[k]);
                continue;
            }
            {
                const char **njids = realloc(jids, (nj + 2) * sizeof *jids);
                if (!njids) break;
                jids = njids;
                jids[nj++] = id;
            }
        }
        if (jids) {
            if (nj > 0) {
                hb_obj_set_strv(o, "jump", jids, nj);
                hb_vault_put(v, o);
            }
            free(jids);
        }
    }

out:
    for (i = 0; i < nh; i++) hb_obj_free(hs[i]);
    free(hs);
    free(map);
    free(created_idx);
    hb_obj_list_free(all, all ? nh : 0);
    return e;
}

/* ------------------------------ JSON bundle ------------------------------ */

hb_err hb_hosts_export_json(hb_vault *v, char **out)
{
    cJSON *root, *groups, *hosts;
    hb_obj **gs = NULL, **hs = NULL;
    size_t ng = 0, nh = 0, i;
    char *str;
    hb_err e = HB_OK;

    if (!v || !out) return HB_ERR_INVALID;
    *out = NULL;
    root = cJSON_CreateObject();
    if (!root) return HB_ERR_NOMEM;
    cJSON_AddStringToObject(root, "format", "harbor-hosts");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "exported_at", (double)hb_now_ms());
    groups = cJSON_AddArrayToObject(root, "groups");
    hosts = cJSON_AddArrayToObject(root, "hosts");
    if (!groups || !hosts) { cJSON_Delete(root); return HB_ERR_NOMEM; }

    e = hb_vault_list(v, "group", &gs, &ng);
    if (e != HB_OK && e != HB_ERR_NOT_FOUND) { cJSON_Delete(root); return e; }
    for (i = 0; i < ng; i++) {
        cJSON *g = cJSON_CreateObject();
        if (!g) continue;
        cJSON_AddStringToObject(g, "id", hb_obj_id(gs[i]));
        if (hb_obj_get_str(gs[i], "name"))
            cJSON_AddStringToObject(g, "name",
                                    hb_obj_get_str(gs[i], "name"));
        cJSON_AddNumberToObject(g, "order",
                                (double)hb_obj_get_int(gs[i], "order", 0));
        cJSON_AddItemToArray(groups, g);
    }
    e = hb_vault_list(v, "host", &hs, &nh);
    if (e != HB_OK && e != HB_ERR_NOT_FOUND) {
        cJSON_Delete(root); hb_obj_list_free(gs, ng); return e;
    }
    for (i = 0; i < nh; i++) {
        const char *proto = hb_obj_get_str(hs[i], "proto");
        cJSON *d, *hj;
        char *dj;
        if (proto && !strcmp(proto, "local"))
            continue;
        dj = hb_obj_to_json(hs[i]);
        d = dj ? cJSON_Parse(dj) : NULL;
        free(dj);
        if (!d) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(d, "_fv");
        hj = cJSON_CreateObject();
        cJSON_AddItemToObject(hj, "id",
                              cJSON_CreateString(hb_obj_id(hs[i])));
        {
            cJSON *f = NULL;
            cJSON_ArrayForEach(f, d) {
                cJSON_AddItemToObject(hj, f->string,
                                      cJSON_Duplicate(f, 1));
            }
        }
        cJSON_Delete(d);
        cJSON_AddItemToArray(hosts, hj);
    }
    hb_obj_list_free(gs, ng);
    hb_obj_list_free(hs, nh);
    str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return HB_ERR_NOMEM;
    *out = str;
    return HB_OK;
}

/* ------------------------------ JSON 导入 ------------------------------ */

typedef struct { char *old_id, *new_id; } idmap;

static void idmap_push(idmap **m, size_t *n, const char *o, const char *nw)
{
    idmap *nm = realloc(*m, (*n + 1) * sizeof *nm);
    if (!nm) return;
    *m = nm;
    (*m)[*n].old_id = xstrdup(o);
    (*m)[*n].new_id = xstrdup(nw);
    (*n)++;
}

static const char *idmap_get(const idmap *m, size_t n, const char *o)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (m[i].old_id && !strcmp(m[i].old_id, o))
            return m[i].new_id;
    return NULL;
}

static int obj_exists_typed(hb_vault *v, const char *id, const char *type)
{
    hb_obj *o = NULL;
    int ok = 0;
    if (!id || !*id) return 0;
    if (hb_vault_get(v, id, &o) != HB_OK || !o) return 0;
    if (!hb_obj_tombstone(o) && !strcmp(hb_obj_type(o), type))
        ok = 1;
    hb_obj_free(o);
    return ok;
}

hb_err hb_hosts_import_json(hb_vault *v, const char *json, hb_import_result *r)
{
    cJSON *root, *hosts_arr = NULL, *groups_arr = NULL, *it;
    hb_obj **hs = NULL, **gs = NULL, **all = NULL;
    size_t nh = 0, ng = 0, nall = 0, cap, idx = 0;
    idmap *map = NULL;
    size_t nmap = 0;
    hb_err e = HB_OK;

    if (!v || !json || !r) return HB_ERR_INVALID;
    memset(r, 0, sizeof *r);
    root = cJSON_Parse(json);
    if (!root) return HB_ERR_INVALID;
    if (cJSON_IsArray(root)) {
        hosts_arr = root;
    } else if (cJSON_IsObject(root)) {
        cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "hosts");
        if (ha && cJSON_IsArray(ha)) {
            hosts_arr = ha;
            groups_arr = cJSON_GetObjectItemCaseSensitive(root, "groups");
        }
    }
    if (!hosts_arr) { cJSON_Delete(root); return HB_ERR_INVALID; }

    e = hb_vault_list(v, "host", &all, &nall);
    if (e != HB_OK && e != HB_ERR_NOT_FOUND) goto fail;
    cap = nall + 64;
    hs = calloc(cap, sizeof *hs);
    if (!hs) { e = HB_ERR_NOMEM; goto fail; }
    nh = nall;
    memcpy(hs, all, nall * sizeof *hs);
    free(all); all = NULL;

    /* groups：按 name 去重建组 */
    if (groups_arr && cJSON_IsArray(groups_arr)) {
        cJSON *g;
        hb_vault_list(v, "group", &gs, &ng);
        cJSON_ArrayForEach(g, groups_arr) {
            cJSON *gn;
            size_t i;
            int exists = 0;
            if (!cJSON_IsObject(g)) continue;
            gn = cJSON_GetObjectItemCaseSensitive(g, "name");
            if (!gn || !cJSON_IsString(gn)) continue;
            for (i = 0; i < ng; i++)
                if (!strcasecmp(hb_obj_get_str(gs[i], "name") ?: "",
                                gn->valuestring)) { exists = 1; break; }
            if (exists) continue;
            {
                hb_obj *go = hb_obj_new("group");
                if (!go) continue;
                hb_obj_set_str(go, "name", gn->valuestring);
                {
                    cJSON *ord = cJSON_GetObjectItemCaseSensitive(g, "order");
                    hb_obj_set_int(go, "order",
                                   ord && cJSON_IsNumber(ord)
                                       ? (int64_t)ord->valuedouble : 0);
                }
                if (hb_vault_put(v, go) == HB_OK) {
                    hb_obj **ngs = realloc(gs, (ng + 1) * sizeof *gs);
                    if (ngs) { gs = ngs; gs[ng++] = go; }
                    else hb_obj_free(go);
                } else hb_obj_free(go);
            }
        }
    }

    /* hosts 第一遍：匹配或创建，记 id 映射 */
    cJSON_ArrayForEach(it, hosts_arr) {
        cJSON *jn, *ja, *jp, *ju, *jpr, *jid, *f;
        const char *name, *address, *user = NULL, *proto = NULL, *old_id = NULL;
        int64_t port = 0;
        hb_obj *ex, *o;
        if (!cJSON_IsObject(it)) {
            r_warn(r, HB_TR("hosts[%zu]: 非对象元素，已跳过"), idx);
            idx++;
            continue;
        }
        jn = cJSON_GetObjectItemCaseSensitive(it, "name");
        ja = cJSON_GetObjectItemCaseSensitive(it, "address");
        if (!cJSON_IsString(jn) || !cJSON_IsString(ja)) {
            r_warn(r, HB_TR("hosts[%zu]: 缺少 name/address，已跳过"), idx);
            idx++;
            continue;
        }
        name = jn->valuestring; address = ja->valuestring;
        jp = cJSON_GetObjectItemCaseSensitive(it, "port");
        if (cJSON_IsNumber(jp)) port = (int64_t)jp->valuedouble;
        ju = cJSON_GetObjectItemCaseSensitive(it, "user");
        if (cJSON_IsString(ju)) user = ju->valuestring;
        jpr = cJSON_GetObjectItemCaseSensitive(it, "proto");
        if (cJSON_IsString(jpr)) proto = jpr->valuestring;
        jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (cJSON_IsString(jid)) old_id = jid->valuestring;

        ex = find_by_tuple(hs, nh, name, address, port, user, proto);
        if (ex) {
            r->skipped++;
            if (old_id) idmap_push(&map, &nmap, old_id, hb_obj_id(ex));
            idx++;
            continue;
        }
        o = hb_obj_new("host");
        if (!o) { idx++; continue; }
        cJSON_ArrayForEach(f, it) {
            if (!f->string || f->string[0] == '_' ||
                !strcmp(f->string, "id"))
                continue;
            cJSON_AddItemToObject(o->data, f->string,
                                  cJSON_Duplicate(f, 1));
        }
        e = hb_vault_put(v, o);
        if (e != HB_OK) { hb_obj_free(o); goto fail; }
        r->created++;
        r_id(r, hb_obj_id(o));
        if (old_id) idmap_push(&map, &nmap, old_id, hb_obj_id(o));
        /* 挂进匹配池供 5 元组与 jump/via 匹配 */
        if (nh >= cap) {
            hb_obj **nhs = realloc(hs, (cap *= 2) * sizeof *hs);
            if (nhs) { hs = nhs; }
            else { hb_obj_free(o); idx++; continue; }
        }
        hs[nh++] = o;
        idx++;
    }

    /* hosts 第二遍：对新建主机重映射 jump[]/via/key_id/startup_snippet */
    for (idx = 0; idx < nh; idx++) {
        hb_obj *o = hs[idx];
        const char **jv = NULL;
        size_t nj = 0, k;
        const char *via, *kid, *snip;
        int dirty = 0;
        /* 只对新建（在 map 的 new_id 一侧）处理？—— 直接看该 host 是否
         * 新建的：其 id 出现在 map.new_id 里 */
        {
            size_t mi;
            int is_new = 0;
            for (mi = 0; mi < nmap; mi++)
                if (map[mi].new_id &&
                    !strcmp(map[mi].new_id, hb_obj_id(o))) { is_new = 1; break; }
            if (!is_new) continue;
        }
        nj = hb_obj_get_strv(o, "jump", &jv);
        if (nj > 0) {
            const char **njv = calloc(nj + 1, sizeof *njv);
            size_t njv_n = 0;
            for (k = 0; k < nj; k++) {
                const char *mapped = idmap_get(map, nmap, jv[k]);
                if (mapped) {
                    njv[njv_n++] = mapped;
                } else if (find_by_id(hs, nh, jv[k])) {
                    njv[njv_n++] = jv[k];
                } else {
                    r_warn(r, HB_TR("%s: jump 引用的主机 %s 不存在，已丢弃"),
                           hb_obj_get_str(o, "name") ?: "?", jv[k]);
                }
            }
            if (njv_n > 0)
                hb_obj_set_strv(o, "jump", njv, njv_n);
            else
                hb_obj_set_str(o, "jump", NULL);
            free(njv);
            dirty = 1;
        }
        free((void *)jv);
        via = hb_obj_get_str(o, "via");
        if (via && *via) {
            const char *mapped = idmap_get(map, nmap, via);
            if (mapped) { hb_obj_set_str(o, "via", mapped); dirty = 1; }
            else if (find_by_id(hs, nh, via)) { /* keep */ }
            else {
                r_warn(r, HB_TR("%s: via 引用的主机 %s 不存在，已丢弃"),
                       hb_obj_get_str(o, "name") ?: "?", via);
                hb_obj_set_str(o, "via", NULL);
                dirty = 1;
            }
        }
        kid = hb_obj_get_str(o, "key_id");
        if (kid && *kid && !obj_exists_typed(v, kid, "key")) {
            r_warn(r, HB_TR("%s: key_id 引用的密钥不存在，已丢弃"),
                   hb_obj_get_str(o, "name") ?: "?");
            hb_obj_set_str(o, "key_id", NULL);
            dirty = 1;
        }
        snip = hb_obj_get_str(o, "startup_snippet");
        if (snip && *snip && !obj_exists_typed(v, snip, "snippet")) {
            r_warn(r, HB_TR("%s: startup_snippet 引用的片段不存在，已丢弃"),
                   hb_obj_get_str(o, "name") ?: "?");
            hb_obj_set_str(o, "startup_snippet", NULL);
            dirty = 1;
        }
        if (dirty)
            hb_vault_put(v, o);
    }

fail:
    if (hs) hb_obj_list_free(hs, nh);
    if (all) hb_obj_list_free(all, nall);
    if (gs) hb_obj_list_free(gs, ng);
    {
        size_t i;
        for (i = 0; i < nmap; i++) { free(map[i].old_id); free(map[i].new_id); }
        free(map);
    }
    cJSON_Delete(root);
    return e;
}
