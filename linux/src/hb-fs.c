#include "hb-fs.h"

#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <glib/gi18n.h>

/* ------------------------------------------------------------------
 * HbFs —— 文件系统抽象：本地 POSIX（GTask 线程）/ 远端 SFTP。
 * 结果全部回到主线程（local 经 GTask finish，remote 经事件回调）。
 * ------------------------------------------------------------------ */

typedef struct {
    hb_fs_cb        cb;
    gpointer        ud;
    GDestroyNotify  dtor;
} FsOp;

typedef struct {
    hb_fs_result    r;
    char           *msg;
    hb_sftp_entry  *ents;
    char           *path;
} Fres;

struct _HbFs {
    volatile gint   ref;
    gboolean        remote;
    volatile gint   dead;
    hb_session     *sess;         /* remote：借用 */
    GMutex          mu;
    GHashTable     *pend;         /* remote：req id → FsOp* */
};

static HbFs *fs_new(gboolean remote)
{
    HbFs *f = g_new0(HbFs, 1);
    f->ref = 1;
    f->remote = remote;
    g_mutex_init(&f->mu);
    if (remote)
        f->pend = g_hash_table_new(g_direct_hash, g_direct_equal);
    return f;
}

static HbFs *fs_ref(HbFs *f)   { g_atomic_int_inc(&f->ref); return f; }
static void  fs_unref(HbFs *f)
{
    if (!g_atomic_int_dec_and_test(&f->ref)) return;
    g_clear_pointer(&f->pend, g_hash_table_destroy);
    g_mutex_clear(&f->mu);
    g_free(f);
}

/* ------------------------- entry 工具 ------------------------- */

static void entry_fill(hb_sftp_entry *e, const char *name,
                       const struct stat *st, const char *link)
{
    memset(e, 0, sizeof *e);
    e->name = g_strdup(name);
    e->size = (uint64_t)st->st_size;
    e->mtime = (int64_t)st->st_mtime;
    e->mode = (uint32_t)(st->st_mode & 07777);
    e->uid = (uint32_t)st->st_uid;
    e->gid = (uint32_t)st->st_gid;
    e->is_dir = S_ISDIR(st->st_mode);
    e->is_link = S_ISLNK(st->st_mode);
    if (link) e->link_target = g_strdup(link);
}

hb_sftp_entry *hb_fs_entries_dup(const hb_sftp_entry *e, size_t n)
{
    hb_sftp_entry *o = g_new0(hb_sftp_entry, n);
    size_t i;
    for (i = 0; i < n; i++) {
        o[i] = e[i];
        o[i].name = g_strdup(e[i].name);
        o[i].link_target = g_strdup(e[i].link_target);
    }
    return o;
}

void hb_fs_entries_free(hb_sftp_entry *e, size_t n)
{
    size_t i;
    if (!e) return;
    for (i = 0; i < n; i++) {
        g_free(e[i].name);
        g_free(e[i].link_target);
    }
    g_free(e);
}

static hb_sftp_status errno_status(int en)
{
    switch (en) {
    case ENOENT: return HB_SFTP_ERR_NOENT;
    case EACCES:
    case EPERM:  return HB_SFTP_ERR_PERM;
    case EEXIST: return HB_SFTP_ERR_EXISTS;
    case ENOTEMPTY: return HB_SFTP_ERR_NOTEMPTY;
    default:     return HB_SFTP_ERR_LOCAL;
    }
}

/* ------------------------- local 后端 ------------------------- */

typedef enum {
    L_LIST, L_STAT, L_REALPATH, L_MKDIR, L_RENAME, L_REMOVE, L_CHMOD
} LKind;

typedef struct {
    HbFs     *fs;
    FsOp      op;
    LKind     kind;
    char     *a;          /* path / from */
    char     *b;          /* to */
    uint32_t  mode;
    gboolean  flag;       /* overwrite / recursive */
} LOp;

static Fres *fres_new(void)
{
    Fres *f = g_new0(Fres, 1);
    f->r.ok = TRUE;
    return f;
}

static Fres *fres_err(hb_sftp_status st, const char *msg)
{
    Fres *f = g_new0(Fres, 1);
    f->r.ok = FALSE;
    f->r.status = st;
    f->msg = g_strdup(msg);
    f->r.message = f->msg;
    return f;
}

static Fres *fres_errno(int en)
{
    hb_sftp_status st = errno_status(en);
    const char *zh = st == HB_SFTP_ERR_NOENT ? _("没有那个文件或目录")
                   : st == HB_SFTP_ERR_PERM ? _("权限不足")
                   : st == HB_SFTP_ERR_EXISTS ? _("目标已存在")
                   : st == HB_SFTP_ERR_NOTEMPTY ? _("目录非空")
                   : NULL;
    return fres_err(st, zh ? zh : g_strerror(en));
}

static void fres_free(Fres *f)
{
    if (!f) return;
    g_free(f->msg);
    g_free(f->path);
    hb_fs_entries_free(f->ents, f->r.n);
    g_free(f);
}

static void l_list(LOp *o, Fres *f)
{
    GDir *d = g_dir_open(o->a, 0, NULL);
    const gchar *nm;
    GArray *arr;
    if (!d) { *f = *fres_errno(errno); return; }
    arr = g_array_new(FALSE, FALSE, sizeof(hb_sftp_entry));
    while ((nm = g_dir_read_name(d))) {
        char *full = g_build_filename(o->a, nm, NULL);
        struct stat st;
        char *link = NULL;
        hb_sftp_entry e;
        if (g_lstat(full, &st) != 0) { g_free(full); continue; }
        if (S_ISLNK(st.st_mode)) {
            char t[PATH_MAX];
            ssize_t k = readlink(full, t, sizeof t - 1);
            if (k > 0) { t[k] = 0; link = t; }
        }
        entry_fill(&e, nm, &st, link);
        g_array_append_val(arr, e);
        g_free(full);
    }
    g_dir_close(d);
    f->ents = (hb_sftp_entry *)arr->data;
    f->r.n = arr->len;
    f->r.entries = f->ents;
    g_array_free(arr, FALSE);
}

static void l_stat(LOp *o, Fres *f)
{
    struct stat st;
    char *link = NULL;
    const char *base;
    if (g_lstat(o->a, &st) != 0) { *f = *fres_errno(errno); return; }
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX];
        ssize_t k = readlink(o->a, t, sizeof t - 1);
        if (k > 0) { t[k] = 0; link = t; }
    }
    base = g_path_get_basename(o->a);
    f->ents = g_new0(hb_sftp_entry, 1);
    entry_fill(f->ents, base, &st, link);
    f->r.n = 1;
    f->r.entries = f->ents;
}

static void l_realpath(LOp *o, Fres *f)
{
    char *rp;
    if (!o->a || !*o->a || !strcmp(o->a, "."))
        rp = g_strdup(g_get_home_dir());
    else {
        rp = realpath(o->a, NULL);
        if (!rp) { *f = *fres_errno(errno); return; }
    }
    f->path = rp;
    f->r.path = f->path;
}

static void l_mkdir(LOp *o, Fres *f)
{
    if (g_mkdir(o->a, (int)o->mode) != 0)
        *f = *fres_errno(errno);
}

static void l_rename(LOp *o, Fres *f)
{
    struct stat st;
    if (!o->flag && g_lstat(o->b, &st) == 0) {
        *f = *fres_errno(EEXIST);
        return;
    }
    if (g_rename(o->a, o->b) != 0)
        *f = *fres_errno(errno);
}

/* 递归删除：lstat，不跟随符号链接 */
static gboolean rm_rf(const char *path, int *en)
{
    struct stat st;
    if (g_lstat(path, &st) != 0) { *en = errno; return FALSE; }
    if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(path, 0, NULL);
        const gchar *nm;
        if (!d) { *en = errno; return FALSE; }
        while ((nm = g_dir_read_name(d))) {
            char *full = g_build_filename(path, nm, NULL);
            gboolean ok = rm_rf(full, en);
            g_free(full);
            if (!ok) { g_dir_close(d); return FALSE; }
        }
        g_dir_close(d);
        if (g_rmdir(path) != 0) { *en = errno; return FALSE; }
        return TRUE;
    }
    if (g_remove(path) != 0) { *en = errno; return FALSE; }
    return TRUE;
}

static void l_remove(LOp *o, Fres *f)
{
    struct stat st;
    int en = 0;
    if (g_lstat(o->a, &st) != 0) { *f = *fres_errno(errno); return; }
    if (S_ISDIR(st.st_mode) && !o->flag) {
        if (g_rmdir(o->a) != 0) *f = *fres_errno(errno);
        return;
    }
    if (!rm_rf(o->a, &en))
        *f = *fres_errno(en);
}

static void l_chmod(LOp *o, Fres *f)
{
    if (g_chmod(o->a, (int)o->mode) != 0)
        *f = *fres_errno(errno);
}

static void lop_work(GTask *task, gpointer src, gpointer data,
                     GCancellable *c)
{
    LOp *o = data;
    Fres *f = fres_new();
    (void)src; (void)c;
    switch (o->kind) {
    case L_LIST:     l_list(o, f); break;
    case L_STAT:     l_stat(o, f); break;
    case L_REALPATH: l_realpath(o, f); break;
    case L_MKDIR:    l_mkdir(o, f); break;
    case L_RENAME:   l_rename(o, f); break;
    case L_REMOVE:   l_remove(o, f); break;
    case L_CHMOD:    l_chmod(o, f); break;
    }
    g_task_return_pointer(task, f, (GDestroyNotify)fres_free);
}

static void lop_finish(GObject *src, GAsyncResult *res, gpointer ud)
{
    LOp *o = ud;
    Fres *f = g_task_propagate_pointer(G_TASK(res), NULL);
    (void)src;
    if (f && !g_atomic_int_get(&o->fs->dead))
        o->op.cb(&f->r, o->op.ud);
    fres_free(f);
    if (o->op.dtor) o->op.dtor(o->op.ud);
    fs_unref(o->fs);
    g_free(o->a);
    g_free(o->b);
    g_free(o);
}

static void local_submit(HbFs *fs, LKind kind, const char *a,
                         const char *b, uint32_t mode, gboolean flag,
                         hb_fs_cb cb, gpointer ud, GDestroyNotify dtor)
{
    LOp *o = g_new0(LOp, 1);
    GTask *t;
    o->fs = fs_ref(fs);
    o->op.cb = cb; o->op.ud = ud; o->op.dtor = dtor;
    o->kind = kind;
    o->a = g_strdup(a);
    o->b = g_strdup(b);
    o->mode = mode;
    o->flag = flag;
    t = g_task_new(NULL, NULL, lop_finish, o);
    g_task_set_task_data(t, o, NULL);   /* worker data */
    g_task_run_in_thread(t, lop_work);
    g_object_unref(t);
}

/* ------------------------- remote 后端 ------------------------- */

static void remote_submit(HbFs *fs, uint32_t req,
                          hb_fs_cb cb, gpointer ud, GDestroyNotify dtor)
{
    FsOp *o = g_new0(FsOp, 1);
    o->cb = cb; o->ud = ud; o->dtor = dtor;
    if (req == 0 || g_atomic_int_get(&fs->dead)) {
        /* 会话不可用 / 后端已释放：主线程异步报 CONN 错 */
        hb_fs_result r = {0};
        r.ok = FALSE;
        r.status = HB_SFTP_ERR_CONN;
        r.message = _("连接已断开");
        if (!g_atomic_int_get(&fs->dead) && cb) cb(&r, ud);
        if (dtor) dtor(ud);
        g_free(o);
        return;
    }
    g_mutex_lock(&fs->mu);
    g_hash_table_insert(fs->pend, GUINT_TO_POINTER(req), o);
    g_mutex_unlock(&fs->mu);
}

gboolean hb_fs_handle_event(HbFs *fs, const hb_sess_event *ev)
{
    FsOp *o;
    const hb_sftp_result *sr;
    if (!fs->remote || ev->type != HB_SESS_EV_SFTP || !ev->sftp)
        return FALSE;
    sr = ev->sftp;
    g_mutex_lock(&fs->mu);
    o = g_hash_table_lookup(fs->pend, GUINT_TO_POINTER(sr->req));
    if (o) g_hash_table_remove(fs->pend, GUINT_TO_POINTER(sr->req));
    g_mutex_unlock(&fs->mu);
    if (!o) return FALSE;
    if (sr->kind == HB_SFTP_R_DONE && !g_atomic_int_get(&fs->dead)) {
        hb_fs_result r = {0};
        r.ok = sr->status == HB_SFTP_OK;
        r.status = sr->status;
        r.message = sr->message;
        r.entries = sr->entries;
        r.n = sr->n;
        r.path = sr->path;
        o->cb(&r, o->ud);
    }
    /* PROGRESS 结果只针对传输请求（不进 pend） */
    if (o->dtor) o->dtor(o->ud);
    g_free(o);
    return TRUE;
}

/* ------------------------- 公共 API ------------------------- */

HbFs *hb_fs_local_new(void) { return fs_new(FALSE); }

HbFs *hb_fs_remote_new(hb_session *s)
{
    HbFs *f = fs_new(TRUE);
    f->sess = s;
    return f;
}

void hb_fs_list(HbFs *f, const char *dir,
                hb_fs_cb cb, gpointer ud, GDestroyNotify d)
{
    if (f->remote)
        remote_submit(f, hb_sftp_list(f->sess, dir), cb, ud, d);
    else
        local_submit(f, L_LIST, dir, NULL, 0, FALSE, cb, ud, d);
}

void hb_fs_stat(HbFs *f, const char *path,
                hb_fs_cb cb, gpointer ud, GDestroyNotify d)
{
    if (f->remote)
        remote_submit(f, hb_sftp_stat(f->sess, path), cb, ud, d);
    else
        local_submit(f, L_STAT, path, NULL, 0, FALSE, cb, ud, d);
}

void hb_fs_realpath(HbFs *f, const char *path,
                    hb_fs_cb cb, gpointer ud, GDestroyNotify d)
{
    if (f->remote)
        remote_submit(f, hb_sftp_realpath(f->sess,
            path && *path ? path : "."), cb, ud, d);
    else
        local_submit(f, L_REALPATH, path ? path : ".", NULL, 0, FALSE,
                     cb, ud, d);
}

void hb_fs_mkdir(HbFs *f, const char *path, uint32_t mode,
                 hb_fs_cb cb, gpointer ud, GDestroyNotify d)
{
    if (f->remote)
        remote_submit(f, hb_sftp_mkdir(f->sess, path, mode), cb, ud, d);
    else
        local_submit(f, L_MKDIR, path, NULL, mode, FALSE, cb, ud, d);
}

void hb_fs_rename(HbFs *f, const char *from, const char *to,
                  gboolean overwrite,
                  hb_fs_cb cb, gpointer ud, GDestroyNotify d)
{
    if (f->remote)
        remote_submit(f, hb_sftp_rename(f->sess, from, to,
                                        overwrite), cb, ud, d);
    else
        local_submit(f, L_RENAME, from, to, 0, overwrite, cb, ud, d);
}

void hb_fs_remove(HbFs *f, const char *path, gboolean recursive,
                  hb_fs_cb cb, gpointer ud, GDestroyNotify d)
{
    if (f->remote)
        remote_submit(f, hb_sftp_remove(f->sess, path, recursive),
                      cb, ud, d);
    else
        local_submit(f, L_REMOVE, path, NULL, 0, recursive, cb, ud, d);
}

void hb_fs_chmod(HbFs *f, const char *path, uint32_t mode,
                 hb_fs_cb cb, gpointer ud, GDestroyNotify d)
{
    if (f->remote)
        remote_submit(f, hb_sftp_chmod(f->sess, path, mode), cb, ud, d);
    else
        local_submit(f, L_CHMOD, path, NULL, mode, FALSE, cb, ud, d);
}

void hb_fs_free(HbFs *f)
{
    if (!f) return;
    g_atomic_int_set(&f->dead, 1);
    if (f->remote && f->pend) {
        /* 未完成请求：回收 ud，不再回调 */
        GHashTableIter it;
        gpointer k, v;
        g_mutex_lock(&f->mu);
        g_hash_table_iter_init(&it, f->pend);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            FsOp *o = v;
            if (o->dtor) o->dtor(o->ud);
            g_free(o);
            g_hash_table_iter_remove(&it);
        }
        g_mutex_unlock(&f->mu);
    }
    fs_unref(f);
}
