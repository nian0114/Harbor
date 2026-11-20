#ifndef HARBOR_KNOWNHOSTS_H
#define HARBOR_KNOWNHOSTS_H

/* OpenSSH known_hosts 导入/导出 → vault "known" 对象（FR-09）。
 * 复用 hostio.h 的 hb_import_result（created/skipped/warnings/created_ids）。 */

#include "common.h"
#include "hostio.h"
#include "vault.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 导入文件：仅普通文件、≤ 8 MiB（防误传大文件）。 */
hb_err hb_known_import_file(hb_vault *v, const char *path,
                            hb_import_result *r);
hb_err hb_known_import_text(hb_vault *v, const char *text,
                            hb_import_result *r);
/* 导出为 OpenSSH known_hosts 文本（一行一条，以 '\n' 结尾）；malloc，free()。 */
hb_err hb_known_export_text(hb_vault *v, char **out);

#ifdef __cplusplus
}
#endif

#endif /* HARBOR_KNOWNHOSTS_H */
