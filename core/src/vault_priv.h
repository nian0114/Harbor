#ifndef HB_VAULT_PRIV_H
#define HB_VAULT_PRIV_H

/* 内部接口（core 内部 + 测试使用，非公共 API）。 */

#include "harbor/vault.h"

/* 解密一个 secret blob 到 sodium_malloc 缓冲区（二进制 map 明文）。
 * 认证失败（错误的 DEK / 数据被篡改）返回 HB_ERR_CRYPTO。
 * out/out_len 成功时为 plaintext（调用方 sodium_free / sodium_memzero）。 */
hb_err hb__vault_secret_decrypt(hb_vault *v, const char *id, const char *type,
                                const uint8_t *blob, size_t blob_len,
                                uint8_t **out, size_t *out_len);

/* ------------------------------ sync 支持 ------------------------------ */

/* meta 表读写；NOT_FOUND 时 *out=NULL。value==NULL 等价于删除。 */
hb_err hb__vault_meta_get(hb_vault *v, const char *key, char **out);
hb_err hb__vault_meta_set(hb_vault *v, const char *key, const char *value);
hb_err hb__vault_meta_del(hb_vault *v, const char *key);

/* 全行快照（含 tombstone），供 sync 扫描。 */
typedef struct {
    char   *id;
    char   *type;
    char   *data;      /* JSON，含 _fv */
    char   *vv;
    int     tombstone;
    int     dirty;
    int64_t sync_seq;
    int64_t updated_at;
} hb__vault_row;

hb_err hb__vault_rows(hb_vault *v, hb__vault_row **out, size_t *n);
void   hb__vault_rows_free(hb__vault_row *r, size_t n);

/* 解密整个 secret map（二进制 map 明文，sodium_malloc'd）。无 secret → NOT_FOUND。 */
hb_err hb__vault_secret_map(hb_vault *v, const char *id,
                            uint8_t **pt, size_t *pt_len);

/* 写整条对象状态（sync 合并/落库用）：不打戳、不 bump 计数器，
 * dirty/sync_seq 显式给定；data_json 须含 _fv；sec_pt==NULL 清除 secret。
 * 提交后触发变更回调。 */
hb_err hb__vault_apply(hb_vault *v, const char *id, const char *type,
                       const char *data_json, const char *vv_json,
                       int tomb, int64_t updated_at,
                       const uint8_t *sec_pt, size_t sec_len,
                       int dirty, int64_t sync_seq);

/* 条件标干净：仅当行 vv 仍等于 vv_json（seal 时捕获）才 dirty=0；
 * sync_seq 无论如何推进。*applied 报告是否标成了干净。 */
hb_err hb__vault_mark_synced(hb_vault *v, const char *id, const char *vv_json,
                             int64_t sync_seq, bool *applied);

/* 硬删除某类型的全部对象（remote wipe）。 */
hb_err hb__vault_purge_type(hb_vault *v, const char *type);

/* 全部对象重新标 dirty 且 sync_seq=0（logout keep-local）。 */
hb_err hb__vault_mark_all_dirty(hb_vault *v);

/* sync_conflicts：一个对象一行。loser secret 为二进制 map 明文（NULL=无），
 * 以本机 DEK 加密存储。 */
hb_err hb__vault_conflict_put(hb_vault *v, const char *id, const char *type,
                              const char *fields_json, const char *loser_data,
                              const uint8_t *loser_sec, size_t loser_sec_len);
/* loser sec 返回密文 blob（需调用方用 hb__vault_secret_decrypt 解）。 */
hb_err hb__vault_conflict_get(hb_vault *v, const char *id,
                              char **type, char **fields, char **loser_data,
                              uint8_t **sec, size_t *sec_len, int64_t *created);
hb_err hb__vault_conflict_del(hb_vault *v, const char *id);

typedef struct {
    char   *id;
    char   *type;
    char   *fields;      /* JSON 数组 */
    char   *loser_data;  /* JSON */
    int64_t created;
} hb__conflict_row;

hb_err hb__vault_conflicts(hb_vault *v, hb__conflict_row **out, size_t *n);
void   hb__vault_conflicts_free(hb__conflict_row *c, size_t n);
hb_err hb__vault_conflicts_clear(hb_vault *v);

#endif
