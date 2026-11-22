#ifndef HARBOR_BACKUP_H
#define HARBOR_BACKUP_H

#include "common.h"
#include "vault.h"

/* FR-15 加密备份（docs/api/backup-format.md，.harbor-backup v1）。
 * 口令错误 / 文件损坏 → HB_ERR_INVAL（"口令错误或文件已损坏"），
 * 导入在写库前全部校验完成，失败不落任何数据。 */

#define HB_BACKUP_EXT ".harbor-backup"

typedef struct {
    size_t created;      /* 新建对象数（import） */
    size_t overwritten;  /* 覆盖对象数（import） */
    size_t total;        /* 文件内对象数（preview） */
} hb_backup_stat;

/* 导出 vault 全部非 tombstone 对象（含 secret 与设备本地字段）。
 * params 为 NULL → Argon2id OPSLIMIT/MEMLIMIT_MODERATE。
 * 成功：*out / *out_len（调用方 free）；返回 HB_OK。 */
hb_err hb_backup_export(hb_vault *v, const char *password,
                        uint8_t **out, size_t *out_len);

/* 只解密校验并统计对象数（不写库）。 */
hb_err hb_backup_preview(const uint8_t *data, size_t len,
                         const char *password, hb_backup_stat *stat);

/* 校验 → 导入：同 id 覆盖业务字段+secret，tombstoned id 恢复，
 * settings 单例按字段覆盖。 */
hb_err hb_backup_import(hb_vault *v, const uint8_t *data, size_t len,
                        const char *password, hb_backup_stat *stat);

#endif
