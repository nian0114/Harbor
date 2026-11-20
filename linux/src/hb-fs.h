#ifndef HB_FS_H
#define HB_FS_H

#include <gtk/gtk.h>
#include <harbor/sftp.h>
#include <harbor/session.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------
 * HbFs —— 文件系统抽象（SFTP 双栏浏览器的后端）。
 *
 * 异步 API：所有操作立即返回，结果经 hb_fs_cb 在 GTK 主线程回调。
 * 本地后端在 GTask 工作线程跑 POSIX；远端后端走 hb_sftp_* 请求，
 * 结果经 HB_SESS_EV_SFTP 事件回到主线程。
 *
 * hb_fs_result 字段仅在回调期间有效（借用）。
 * ------------------------------------------------------------------ */

typedef struct _HbFs HbFs;

typedef struct {
    gboolean           ok;
    hb_sftp_status     status;
    const char        *message;       /* 借用于回调期间 */
    hb_sftp_entry     *entries;
    size_t             n;             /* list/stat */
    const char        *path;          /* realpath 结果 */
} hb_fs_result;

typedef void (*hb_fs_cb)(const hb_fs_result *r, gpointer ud);

/* 本地 POSIX 后端（GTask 线程；递归遍历不跟随符号链接） */
HbFs *hb_fs_local_new(void);
/* 远端 SFTP 后端：session 借用；事件经 hb_fs_handle_event 喂入 */
HbFs *hb_fs_remote_new(hb_session *s);

/* remote 后端：owner 把每个 HB_SESS_EV_SFTP 事件喂过来；
 * 返回 TRUE = 已被某请求消费 */
gboolean hb_fs_handle_event(HbFs *fs, const hb_sess_event *ev);

/* 所有操作：cb 在主线程调用；destroy 在对象释放/请求完成后回收 ud */
void hb_fs_list(HbFs *fs, const char *dir,
                hb_fs_cb cb, gpointer ud, GDestroyNotify destroy);
void hb_fs_stat(HbFs *fs, const char *path,
                hb_fs_cb cb, gpointer ud, GDestroyNotify destroy);
void hb_fs_realpath(HbFs *fs, const char *path,   /* NULL/"." → home */
                    hb_fs_cb cb, gpointer ud, GDestroyNotify destroy);
void hb_fs_mkdir(HbFs *fs, const char *path, uint32_t mode,
                 hb_fs_cb cb, gpointer ud, GDestroyNotify destroy);
void hb_fs_rename(HbFs *fs, const char *from, const char *to,
                  gboolean overwrite,
                  hb_fs_cb cb, gpointer ud, GDestroyNotify destroy);
void hb_fs_remove(HbFs *fs, const char *path, gboolean recursive,
                  hb_fs_cb cb, gpointer ud, GDestroyNotify destroy);
void hb_fs_chmod(HbFs *fs, const char *path, uint32_t mode,
                 hb_fs_cb cb, gpointer ud, GDestroyNotify destroy);

/* 释放后端；未完成请求的回调不再触发，ud 仍按 destroy 回收 */
void hb_fs_free(HbFs *fs);

/* entry 数组深拷贝（回调外留存时用） */
hb_sftp_entry *hb_fs_entries_dup(const hb_sftp_entry *e, size_t n);
void           hb_fs_entries_free(hb_sftp_entry *e, size_t n);

G_END_DECLS

#endif
