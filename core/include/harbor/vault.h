#ifndef HB_VAULT_H
#define HB_VAULT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* hb_obj：通用同步对象（JSON data + 元数据）。                         */
/*                                                                     */
/* data 顶层字段即业务字段；以 '_' 开头的键保留给内部元数据（如 _fv），   */
/* 公开 setter 会拒绝这些键。敏感字段不放在 data 里，走 secret 接口。     */
/* ------------------------------------------------------------------ */

typedef struct hb_obj hb_obj;

hb_obj *hb_obj_new(const char *type);             /* fresh uuid */
hb_obj *hb_obj_copy(const hb_obj *o);             /* deep copy, same id */
hb_obj *hb_obj_clone_new(const hb_obj *o);        /* deep copy, new id, fresh vv（"复制主机"） */
void    hb_obj_free(hb_obj *o);
void    hb_obj_list_free(hb_obj **v, size_t n);

const char *hb_obj_id(const hb_obj *o);
const char *hb_obj_type(const hb_obj *o);
int64_t     hb_obj_updated_at(const hb_obj *o);
bool        hb_obj_tombstone(const hb_obj *o);

const char *hb_obj_get_str(const hb_obj *o, const char *key);   /* NULL if missing */
int64_t     hb_obj_get_int(const hb_obj *o, const char *key, int64_t def);
double      hb_obj_get_double(const hb_obj *o, const char *key, double def);
bool        hb_obj_get_bool(const hb_obj *o, const char *key, bool def);
/* 返回元素数；*out 为 malloc 的指针数组（元素为借用指针），调用方 free() 数组本身。 */
size_t      hb_obj_get_strv(const hb_obj *o, const char *key, const char ***out);

/* val == NULL 表示删除该字段。键以 '_' 开头会被拒绝（保留前缀）。 */
void hb_obj_set_str(hb_obj *o, const char *key, const char *val);
void hb_obj_set_int(hb_obj *o, const char *key, int64_t val);
void hb_obj_set_double(hb_obj *o, const char *key, double val);
void hb_obj_set_bool(hb_obj *o, const char *key, bool val);
void hb_obj_set_strv(hb_obj *o, const char *key, const char *const *v, size_t n);
bool hb_obj_has(const hb_obj *o, const char *key);

/* data 的 JSON 序列化（含内部 _fv），malloc'd，调用方 free()。 */
char *hb_obj_to_json(const hb_obj *o);

/* ------------------------------------------------------------------ */
/* hb_vault：SQLite 持久化对象库（WAL + user_version 迁移）。             */
/* ------------------------------------------------------------------ */

typedef struct hb_vault hb_vault;

/* 打开/创建 vault。自动创建父目录（0700）与文件（0600）。dek 为 32 字节数据密钥。 */
hb_err hb_vault_open(const char *path, const uint8_t dek[32], hb_vault **out);
void   hb_vault_close(hb_vault *v);

const char *hb_vault_device_id(const hb_vault *v);

hb_err hb_vault_put(hb_vault *v, hb_obj *o);
/* 返回 tombstoned 对象时需检查 hb_obj_tombstone。未找到返回 HB_ERR_NOT_FOUND。 */
hb_err hb_vault_get(hb_vault *v, const char *id, hb_obj **out);
/* 非 tombstone，按 int 字段 "order" 升序、再按 "name"/"title" NOCASE 排序。 */
hb_err hb_vault_list(hb_vault *v, const char *type, hb_obj ***out, size_t *n);
/* tombstone=1 并 bump vv（data+secret 保留，撤销可完整恢复）。 */
hb_err hb_vault_delete(hb_vault *v, const char *id);
hb_err hb_vault_undelete(hb_vault *v, const char *id);
hb_err hb_vault_purge_tombstones(hb_vault *v, int64_t older_than_ms);

/* 敏感字段：value==NULL 清除该字段；不存在对象返回 HB_ERR_NOT_FOUND。 */
hb_err hb_vault_set_secret(hb_vault *v, const char *id, const char *field, const char *value);
/* 返回值用 hb_secret_free() 释放；无此字段/解密失败返回 NULL。 */
char  *hb_vault_get_secret(hb_vault *v, const char *id, const char *field);

/* 变更通知：commit 后在调用线程同步回调。 */
typedef void (*hb_vault_change_cb)(hb_vault *v, const char *type, const char *id, void *ud);
void hb_vault_set_change_cb(hb_vault *v, hb_vault_change_cb cb, void *ud);

/* ------------------------------- 领域辅助 ------------------------------- */

/* 引用该密钥（data.key_id）的非 tombstone 主机数量。 */
size_t hb_vault_key_ref_count(hb_vault *v, const char *key_id);

/* 删除密钥时被连带清空的主机：撤销时逐台恢复 key_id + 原 auth。 */
typedef struct { char *host_id; char *prev_auth; } hb_key_ref;
/* tombstone 该密钥对象；所有引用它的主机移除 key_id 且 auth 置 "auto"
 *（不是 "none"：none 表示完全不认证，主机会直接失败）。
 * refs_out/n_out 可传 NULL 不要快照；否则 malloc 数组，
 * 用 hb_key_refs_free 释放。整个变更在一个事务里。 */
hb_err hb_vault_delete_key_cascade(hb_vault *v, const char *key_id,
                                   hb_key_ref **refs_out, size_t *n_out);
/* 撤销级联删除：复活密钥；refs 中每台仍存活且未设 key_id 的主机恢复
 * key_id 与 prev_auth（已删除的主机跳过）。同一事务。 */
hb_err hb_vault_undo_key_cascade(hb_vault *v, const char *key_id,
                                 const hb_key_ref *refs, size_t n);
void   hb_key_refs_free(hb_key_ref *r, size_t n);

/* JSON 数组导出（非 tombstone）：data 字段 + "id"，不含 _fv/secret。 */
hb_err hb_vault_export_json(hb_vault *v, const char *type, char **out);
/* 导入数组：每个对象分配新 id（"id"/"_fv" 字段被忽略）；跳过非对象元素。 */
hb_err hb_vault_import_json(hb_vault *v, const char *type, const char *json, size_t *imported);

/* 设置单例（id="settings", type="settings"）；不存在时返回填好默认值的未存储对象。 */
hb_err hb_vault_settings(hb_vault *v, hb_obj **out);

/* --------------------------------- 日志 ---------------------------------- */

typedef struct {
    int64_t id, ts;
    const char *user, *host, *host_id, *proto, *result, *reason;
    int64_t duration_s;   /* -1 = ongoing */
    bool saved;
} hb_log;

hb_err hb_vault_log_add(hb_vault *v, const hb_log *l, int64_t *out_id);
hb_err hb_vault_log_finish(hb_vault *v, int64_t id, const char *result, const char *reason, int64_t duration_s);
hb_err hb_vault_log_set_saved(hb_vault *v, int64_t id, bool saved);

/* return false 停止遍历。回调内 hb_log 的字符串指针为借用，仅在回调期间有效。 */
typedef bool (*hb_log_cb)(const hb_log *l, void *ud);
/* filter 为 user/host/proto/result 的子串过滤，NULL 全部；desc=按时间倒序。 */
hb_err hb_vault_log_foreach(hb_vault *v, const char *filter, bool desc, hb_log_cb cb, void *ud);

/* UTF-8 BOM + 表头 Date,User,Host,Protocol,Result,Reason,Duration,Saved；RFC4180 引号。 */
hb_err hb_vault_log_export_csv(hb_vault *v, char **out);
hb_err hb_vault_log_export_json(hb_vault *v, char **out);
/* retention_days <= 0 表示不清理。 */
hb_err hb_vault_log_purge(hb_vault *v, int retention_days);
/* 进程崩溃/被杀留下的未收尾行（duration_s<0）→ result="interrupted"，
 * duration 记为 ts 到当前的上限。启动时调用一次。 */
hb_err hb_vault_log_sweep(hb_vault *v);
/* 全量删除日志（“清空日志”）；与 purge 不同不受保留期影响。 */
hb_err hb_vault_log_clear(hb_vault *v);

/* --------------------------------- 历史 ---------------------------------- */

/* 忽略空白命令；同一 host_id 连续重复命令不入库。host_id NULL = 本地会话。 */
hb_err hb_vault_history_add(hb_vault *v, const char *host_id, const char *cmd);
/* 子串匹配、最近优先、去重；结果用 hb_strfreev 释放。 */
hb_err hb_vault_history_search(hb_vault *v, const char *prefix_or_substr, size_t limit,
                               char ***out, size_t *n);

typedef struct { int64_t id, ts; const char *host_id, *cmd; } hb_history_entry;
typedef bool (*hb_history_cb)(const hb_history_entry *e, void *ud);
/* newest first；substr NULL/"" = 全部，host_id NULL = 全部主机、
 * "" = 仅本地（host_id IS NULL）。回调返回 false 停止；字符串借用。 */
hb_err hb_vault_history_foreach(hb_vault *v, const char *substr,
                                const char *host_id,
                                hb_history_cb cb, void *ud);
hb_err hb_vault_history_delete(hb_vault *v, int64_t id);
hb_err hb_vault_history_clear(hb_vault *v);

#ifdef __cplusplus
}
#endif

#endif
