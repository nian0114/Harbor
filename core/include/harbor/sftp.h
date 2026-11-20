#ifndef HB_SFTP_H
#define HB_SFTP_H

/* ------------------------------------------------------------------ */
/* SFTP（FR-05 + FR-19 Phase 1）：SSH 会话的一种模式，继承连接阶段、    */
/* ProxyJump、TOFU/HOSTKEY、PROMPT、取消与事件/应答管线。               */
/*                                                                    */
/* 用法：hb_session_new_sftp(opts) 建会话 → 等 CONNECTED 事件后调用      */
/* hb_sftp_* 提交请求（任意线程安全，FIFO 执行；元数据请求立即跑，       */
/* 传输请求并发上限 hb_sftp_set_concurrency，默认 3）。结果经            */
/* HB_SESS_EV_SFTP 事件回来（ev.sftp 为 owned）。                        */
/* ------------------------------------------------------------------ */

#include "common.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HB_SFTP_OK = 0,
    HB_SFTP_ERR_NOENT,      /* 没有那个文件或目录 */
    HB_SFTP_ERR_PERM,       /* 权限不足 */
    HB_SFTP_ERR_EXISTS,     /* 目标已存在 */
    HB_SFTP_ERR_NOTEMPTY,   /* 目录非空 */
    HB_SFTP_ERR_IO,         /* 远端读写错误 */
    HB_SFTP_ERR_CONN,       /* 连接已断开 */
    HB_SFTP_ERR_CANCELLED,  /* 已取消 */
    HB_SFTP_ERR_LOCAL,      /* 本地文件错误 */
    HB_SFTP_ERR_OTHER
} hb_sftp_status;

typedef enum {
    HB_SFTP_OVERWRITE,      /* 覆盖同名文件 */
    HB_SFTP_SKIP,           /* 跳过（结果 skipped=true） */
    HB_SFTP_RENAME          /* 自动改名 "name (N).ext" */
} hb_sftp_conflict;

typedef struct hb_sftp_entry {
    char    *name;
    uint64_t size;
    int64_t  mtime;
    uint32_t mode;          /* 权限位（含类型位 S_IF*） */
    uint32_t uid, gid;
    bool     is_dir, is_link;
    char    *link_target;   /* NULL unless is_link */
} hb_sftp_entry;

typedef enum {
    HB_SFTP_R_DONE,         /* 终态：status/message/entries/path 有效 */
    HB_SFTP_R_PROGRESS      /* 传输进度：done/total/files_* 有效 */
} hb_sftp_rkind;

typedef struct hb_sftp_result {
    uint32_t          req;
    hb_sftp_rkind     kind;
    hb_sftp_status    status;
    char             *message;      /* 人类可读中文，OK 时为 NULL */
    hb_sftp_entry    *entries;
    size_t            n;            /* LIST / STAT（n==1） */
    char             *path;         /* REALPATH 结果，或传输最终目标路径
                                       （RENAME 策略后的实际名） */
    uint64_t          done, total;  /* 传输字节；total 0 = 未知（遍历中） */
    uint32_t          files_done, files_total;  /* 目录传输 */
    bool              skipped;      /* SKIP 策略命中 */
} hb_sftp_result;

/* 以下全部返回请求 id（>0）；会话非 SFTP 模式返回 0。任意线程可调用；
 * 结果经 HB_SESS_EV_SFTP 事件回来（event.sftp，owned）。 */
uint32_t hb_sftp_realpath(hb_session *s, const char *path);     /* "." → home */
uint32_t hb_sftp_list(hb_session *s, const char *dir);          /* 排除 . 与 ..，不跟随链接 */
uint32_t hb_sftp_stat(hb_session *s, const char *path);         /* lstat 语义 */
uint32_t hb_sftp_mkdir(hb_session *s, const char *path, uint32_t mode);
uint32_t hb_sftp_rename(hb_session *s, const char *from, const char *to,
                       bool overwrite);
uint32_t hb_sftp_remove(hb_session *s, const char *path, bool recursive);
uint32_t hb_sftp_chmod(hb_session *s, const char *path, uint32_t mode);
uint32_t hb_sftp_upload(hb_session *s, const char *local, const char *remote,
                       hb_sftp_conflict c);   /* 文件或目录（递归） */
uint32_t hb_sftp_download(hb_session *s, const char *remote, const char *local,
                         hb_sftp_conflict c); /* 文件或目录（递归） */

/* 取消排队或运行中的请求；运行中的传输会清理临时文件。 */
void hb_sftp_cancel(hb_session *s, uint32_t req);

/* 并发传输数上限：1..8，默认 3。 */
void hb_sftp_set_concurrency(hb_session *s, int n);

void hb_sftp_result_free(hb_sftp_result *r);

#ifdef __cplusplus
}
#endif

#endif /* HB_SFTP_H */
