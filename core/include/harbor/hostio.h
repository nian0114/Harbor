#ifndef HARBOR_HOSTIO_H
#define HARBOR_HOSTIO_H

/* ssh_config 解析 + 主机导入/导出（FR-01 导入 OpenSSH 配置；
 * FR-18 主机 JSON bundle 导出/导入）。纯 vault 操作，不含 secrets。 */

#include "common.h"
#include "vault.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *alias;          /* 具体 Host 名（非通配、非取反） */
    char  *hostname;       /* HostName；缺省 = alias */
    char  *user;           /* NULL = 未设 */
    int    port;           /* 0 = 未设 */
    char  *identity_file;  /* 第一个 IdentityFile，已展开；NULL = 未设 */
    char **proxy_jump;     /* NULL 结尾；"none"/未设 → NULL */
    int    keepalive;      /* ServerAliveInterval；0 = 未设 */
    char  *source;         /* "path:line" of the Host line */
} hb_sshcfg_host;

typedef struct {
    hb_sshcfg_host *hosts; size_t n;
    char **warnings; size_t nwarn;
} hb_sshcfg;

hb_err hb_sshcfg_parse_file(const char *path, hb_sshcfg *out);
/* base_dir: relative Include paths resolve against it (user config → ~/.ssh) */
hb_err hb_sshcfg_parse_string(const char *text, const char *base_dir,
                              hb_sshcfg *out);
void   hb_sshcfg_free(hb_sshcfg *c);

typedef struct {
    size_t created, skipped, jumps_created;
    char **warnings; size_t nwarn;
    char **created_ids; size_t ncreated;   /* new host ids, for UI selection/undo */
} hb_import_result;
void hb_import_result_free(hb_import_result *r);

hb_err hb_hosts_import_sshcfg(hb_vault *v, const hb_sshcfg *c,
                              hb_import_result *r);
hb_err hb_hosts_export_json(hb_vault *v, char **out);
hb_err hb_hosts_import_json(hb_vault *v, const char *json, hb_import_result *r);

#ifdef __cplusplus
}
#endif

#endif /* HARBOR_HOSTIO_H */
