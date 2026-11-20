/* hb-sftp-view.c — SFTP 双栏文件管理（FR-05 / FR-19）
 *
 * 左右两个 pane：Select Host → Connecting（HbConnectView + HbConnCtl
 * SFTP 模式）→ Browser（GtkColumnView）。底部常驻传输队列。
 * 文件系统操作经 HbFs（本地 POSIX / 远端 SFTP）全部异步回主线程。
 */
#include "hb-sftp-view.h"
#include "hb-conn.h"
#include "hb-connect-view.h"
#include "hb-fs.h"
#include "hb-icons.h"
#include "hb-dialogs.h"
#include "hb-notify.h"
#include "hb-window.h"

#include <harbor/sftp.h>
#include <adwaita.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ================= HbFileItem ================= */

#define HB_TYPE_FILE_ITEM (hb_file_item_get_type())
G_DECLARE_FINAL_TYPE(HbFileItem, hb_file_item, HB, FILE_ITEM, GObject)

struct _HbFileItem {
    GObject parent;
    char    *name;
    char    *path;          /* cwd/name */
    uint64_t size;
    int64_t  mtime;
    uint32_t mode;
    gboolean is_dir, is_link, hidden;
    char    *link_target;
};

static char *path_join(const char *dir, const char *name);

G_DEFINE_TYPE(HbFileItem, hb_file_item, G_TYPE_OBJECT)

static void hb_file_item_finalize(GObject *o)
{
    HbFileItem *i = HB_FILE_ITEM(o);
    g_free(i->name);
    g_free(i->path);
    g_free(i->link_target);
    G_OBJECT_CLASS(hb_file_item_parent_class)->finalize(o);
}
static void hb_file_item_class_init(HbFileItemClass *k)
{ G_OBJECT_CLASS(k)->finalize = hb_file_item_finalize; }
static void hb_file_item_init(HbFileItem *i) { (void)i; }

static HbFileItem *file_item_new(const hb_sftp_entry *e, const char *cwd)
{
    HbFileItem *i = g_object_new(HB_TYPE_FILE_ITEM, NULL);
    i->name = g_strdup(e->name);
    i->path = cwd && *cwd ? path_join(cwd, e->name)
                          : g_strdup(e->name);
    i->size = e->size;
    i->mtime = e->mtime;
    i->mode = e->mode;
    i->is_dir = e->is_dir;
    i->is_link = e->is_link;
    i->hidden = e->name[0] == '.';
    i->link_target = g_strdup(e->link_target);
    return i;
}

/* ================= Pane ================= */

typedef enum { PS_SELECT, PS_CONN, PS_BROWSE } PaneState;
typedef struct _Pane Pane;
typedef struct _Xfer Xfer;

struct _Pane {
    HbSftpView *v;
    int         side;
    PaneState   state;
    GtkWidget  *stack;

    /* select */
    GtkWidget  *host_box;
    GtkWidget  *sel_entry;

    /* conn */
    HbConnCtl  *ctl;
    GtkWidget  *conn_view;
    char       *conn_host_id;   /* 尝试中的 host */

    /* browse */
    gboolean    is_local;
    char       *host_id;
    char       *hname;
    char       *os_key;
    HbFs       *fs;
    hb_session *sess;           /* 借用（remote） */
    guint       ev_src;
    char       *cwd;
    GPtrArray  *back, *fwd;
    GListStore *model;
    GtkFilterListModel *filtered;
    GtkSortListModel   *sorted;
    GtkMultiSelection  *sel;
    GtkCustomFilter    *filter;
    GtkWidget  *colview;
    GtkWidget  *browse_box;
    GtkWidget  *crumbs;
    GtkWidget  *back_b, *fwd_b, *up_b;
    GtkWidget  *filter_row, *filter_entry;
    GtkWidget  *head_av;
    gboolean    show_hidden;
    gboolean    loading;
    /* delete-undo */
    GList      *pend_dels;      /* PendDel* 未完成删除 */
    GList      *open_ctxs;      /* OpenCtx* 已打开的监视 */
    GHashTable *open_reqs;      /* req → OpenCtx（"打开"下载） */
    guint       gen;
    guint       list_gen;       /* 最新一次 list 请求序号 */
};

struct _HbSftpView {
    GtkBox parent;
    HbStore    *store;
    HbWindow   *win;
    char       *tab_id;
    Pane        pane[2];
    GList      *xers;         /* Xfer* */
    int         next_xid;
    int         conc;         /* sftp_concurrency */
    /* 队列 UI */
    GtkWidget  *xwrap;
    GtkWidget  *xhead;
    GtkWidget  *xlist;
    GtkWidget  *xclear;
};

struct _PendDel {
    Pane  *p;
    GList *paths;         /* char* 真删 */
    GPtrArray *items;     /* HbFileItem* 撤销恢复 */
    guint  gen;
    gboolean undone;      /* button-click 后 toast 仍发 dismissed */
};

struct _RenCtx { Pane *p; char *old; };

struct _OpenCtx {
    Pane   *p;
    char   *name;
    char   *remote_path;
    char   *local_path;
    char   *dir;            /* cache 目录 */
    GFileMonitor *mon;
    guint   req;
};

G_DEFINE_TYPE(HbSftpView, hb_sftp_view, GTK_TYPE_BOX)

typedef struct _PendDel PendDel;
typedef struct _OpenCtx OpenCtx;
typedef struct _RenCtx RenCtx;
static void open_ctx_free(OpenCtx *oc);
static void on_crumb(GtkButton *b, gpointer ud);
static void on_xfer_btn(GtkButton *b, gpointer ud);
static void xfer_dispatch(Pane *p, const hb_sftp_result *r);
static gboolean open_done(Pane *p, const hb_sftp_result *r);
static void on_row_menu(GtkGestureClick *g, int n, double x,
                        double y, gpointer ud);
static GdkContentProvider *drag_prepare(GtkDragSource *ds,
                                        double x, double y,
                                        gpointer ud);
static gboolean on_drop(GtkDropTarget *dt, const GValue *val,
                        double x, double y, gpointer ud);
static void on_col_press(GtkGestureClick *g, int n, double x,
                         double y, gpointer ud)
{
    Pane *p = g_object_get_data(G_OBJECT(g), "pane");
    (void)n; (void)x; (void)y; (void)ud;
    gtk_widget_grab_focus(p->colview);
}

static void on_row_activate(GtkColumnView *cv, guint pos, gpointer ud);
static gboolean on_pane_key(GtkEventControllerKey *kc, guint kv,
                            guint code, GdkModifierType st,
                            gpointer ud);
static void act_rename(Pane *p, GList *items);
static void act_chmod(Pane *p, GList *items);
static void act_open(Pane *p, GList *items);
static void on_rename_name(const char *text, gpointer ud);
static void op_cb_refresh(const hb_fs_result *r, gpointer ud);
typedef struct { Pane *p; GList *items; } DelConf;
static void on_deldir_conf(gboolean ok, gpointer ud);
static void delete_items(Pane *p, GList *items);
static void pane_pick(Pane *p, const char *host_id);
static void pane_browse_to(Pane *p, const char *path, gboolean nav);
static void pane_refresh(Pane *p);
static void pane_teardown_conn(Pane *p);
static void xfer_enqueue(HbSftpView *v, Pane *sp, GList *items /*paths*/,
                         Pane *dp);
static void xfer_fail_session(HbSftpView *v, hb_session *s,
                              const char *why);

/* ------------------------- 小工具 ------------------------- */

static char *parent_dir(const char *p)
{
    char *d = g_path_get_dirname(p);
    if (!strcmp(d, ".")) { g_free(d); return g_strdup("/"); }
    return d;
}

static char *path_join(const char *dir, const char *name)
{
    if (!strcmp(dir, "/")) return g_strdup_printf("/%s", name);
    return g_strdup_printf("%s/%s", dir, name);
}

static HbWindow *view_win(HbSftpView *v)
{
    if (v->win) return v->win;
    GtkRoot *r = gtk_widget_get_root(GTK_WIDGET(v));
    return HB_IS_WINDOW(r) ? HB_WINDOW(r) : NULL;
}

static gboolean view_tab_focused(HbSftpView *v)
{
    HbWindow *w = view_win(v);
    const char *act;
    if (!w || !v->tab_id) return FALSE;
    act = hb_window_active_tab_id(w);
    return gtk_window_is_active(GTK_WINDOW(w)) && act &&
           !strcmp(act, v->tab_id);
}

static void toast(HbSftpView *v, const char *t)
{ hb_toast(GTK_WIDGET(v), t); }

/* ------------------------- 连接状态 ------------------------- */

static void pane_set_state(Pane *p, PaneState s)
{
    static const char *nm[] = { "select", "conn", "browse" };
    p->state = s;
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), nm[s]);
}

static int read_conc(HbSftpView *v)
{
    hb_obj *o = NULL;
    int c = 3;
    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        c = (int)hb_obj_get_int(o, "sftp_concurrency", 3);
        hb_obj_free(o);
    }
    return CLAMP(c, 1, 8);
}

/* SFTP 会话事件泵：ctl → fs → 传输结果分发 */
static gboolean pane_ev_fd(int fd, GIOCondition cond, gpointer ud)
{
    Pane *p = ud;
    hb_sess_event ev;
    (void)fd;
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL) || !p->sess)
        return G_SOURCE_REMOVE;
    /* 断开事件会在循环内把 p->sess 置 NULL，必须每轮重查 */
    while (p->sess && hb_session_next_event(p->sess, &ev)) {
        if (hb_conn_ctl_handle_event(p->ctl, &ev))
            ;
        else if (p->fs && hb_fs_handle_event(p->fs, &ev))
            ;
        else if (ev.type == HB_SESS_EV_SFTP && ev.sftp) {
            /* "打开"下载 → open_reqs；其余 → 传输队列分发 */
            gboolean handled = FALSE;
            if (p->open_reqs &&
                g_hash_table_contains(p->open_reqs,
                    GUINT_TO_POINTER(ev.sftp->req)))
                handled = open_done(p, ev.sftp);
            if (!handled)
                xfer_dispatch(p, ev.sftp);
        }
        hb_sess_event_clear(&ev);
    }
    return G_SOURCE_CONTINUE;
}

static void pane_fail_xfer(Pane *p, const char *why)
{
    if (p->sess) xfer_fail_session(p->v, p->sess, why);
}

/* ctl "connected"：建远端 fs，取 home 进 browse */
static void rp_cb(const hb_fs_result *r, gpointer ud);

/* ctl "session"：会话已创建 —— 立即接事件泵（STAGE/HOSTKEY/PROMPT/
 * CONNECTED 都靠它送达），并设 SFTP 并发 */
static void on_ctl_sess(HbConnCtl *c, gpointer sess_p, gpointer ud)
{
    Pane *p = ud;
    (void)c; (void)sess_p;
    p->sess = hb_conn_ctl_session(p->ctl);
    if (!p->sess) return;
    hb_sftp_set_concurrency(p->sess, p->v->conc);
    if (p->ev_src) g_source_remove(p->ev_src);
    p->ev_src = g_unix_fd_add(hb_session_event_fd(p->sess),
        G_IO_IN | G_IO_ERR | G_IO_HUP, pane_ev_fd, p);
}

static void on_ctl_conn(HbConnCtl *c, gpointer ud)
{
    Pane *p = ud;
    (void)c;
    if (!p->sess) return;
    p->fs = hb_fs_remote_new(p->sess);
    hb_fs_realpath(p->fs, NULL, rp_cb, p, NULL);
}

static void rp_cb(const hb_fs_result *r, gpointer ud)
{
    Pane *p = ud;
    if (r->ok && r->path)
        pane_browse_to(p, r->path, TRUE);
    else
        pane_browse_to(p, "/", TRUE);
}

/* ctl "failed"：连接期失败 → conn 视图错误卡（已在） */
static void on_ctl_fail(HbConnCtl *c, int code, const char *title,
                        const char *details, gpointer ud)
{
    Pane *p = ud;
    (void)c;
    hb_connect_view_error(HB_CONNECT_VIEW(p->conn_view), title,
                          details, code);
    pane_fail_xfer(p, title ? title : "连接失败");
}

static void on_ctl_disconn(HbConnCtl *c, gpointer ud);
static void on_ctl_exited(HbConnCtl *c, int code, gpointer ud)
{
    (void)code;
    on_ctl_disconn(c, ud);
}

static void on_ctl_disconn(HbConnCtl *c, gpointer ud)
{
    Pane *p = ud;
    (void)c;
    pane_set_state(p, PS_CONN);
    hb_connect_view_error(HB_CONNECT_VIEW(p->conn_view),
                          "连接已断开", NULL, 0);
    pane_fail_xfer(p, "连接已断开");
    p->sess = NULL;             /* 死会话：防止 stale 指针复用 */
}

static void on_ctl_stage(HbConnCtl *c, int idx, const char *text,
                         gchar mark, gpointer ud)
{
    Pane *p = ud;
    (void)c;
    hb_connect_view_stage(HB_CONNECT_VIEW(p->conn_view), idx, text,
                          mark);
}

static void on_cv_close(HbConnectView *cv, gpointer ud)
{
    Pane *p = ud;
    (void)cv;
    pane_teardown_conn(p);
    pane_set_state(p, PS_SELECT);
}

static void on_cv_retry(HbConnectView *cv, gpointer ud)
{
    Pane *p = ud;
    char *id;
    (void)cv;
    id = g_strdup(p->conn_host_id);
    pane_pick(p, id);
    g_free(id);
}

/* 停掉连接/fs/事件泵（换 host、回 select、dispose） */
static void pane_teardown_conn(Pane *p)
{
    GList *l;
    if (p->ev_src) { g_source_remove(p->ev_src); p->ev_src = 0; }
    p->sess = NULL;
    g_clear_pointer(&p->open_reqs, g_hash_table_destroy);
    g_list_free_full(p->open_ctxs, (GDestroyNotify)open_ctx_free);
    p->open_ctxs = NULL;
    /* 待删项：pane 关闭/换主机前立即执行真删 */
    while ((l = p->pend_dels)) {
        PendDel *pd = l->data;
        GList *q;
        for (q = pd->paths; q && p->fs; q = q->next)
            hb_fs_remove(p->fs, q->data, TRUE, NULL, NULL, NULL);
        g_list_free_full(pd->paths, g_free);
        g_ptr_array_free(pd->items, TRUE);
        g_free(pd);
        p->pend_dels = g_list_delete_link(p->pend_dels, l);
    }
    g_clear_pointer(&p->fs, hb_fs_free);
    if (p->ctl) {
        hb_conn_ctl_cancel(p->ctl);
        g_clear_object(&p->ctl);
    }
    p->gen++;
}

/* 进入连接流程（host_id NULL=local → 直接 browse home） */
static void pane_pick(Pane *p, const char *host_id)
{
    pane_teardown_conn(p);
    g_free(p->conn_host_id);
    p->conn_host_id = g_strdup(host_id);
    g_free(p->host_id);
    p->host_id = g_strdup(host_id);
    g_clear_pointer(&p->back, g_ptr_array_unref);
    g_clear_pointer(&p->fwd, g_ptr_array_unref);
    p->back = g_ptr_array_new_with_free_func(g_free);
    p->fwd = g_ptr_array_new_with_free_func(g_free);

    if (!host_id) {
        /* Local */
        p->is_local = TRUE;
        g_free(p->hname); p->hname = g_strdup("Local");
        g_free(p->os_key); p->os_key = g_strdup("local");
        p->fs = hb_fs_local_new();
        hb_fs_realpath(p->fs, NULL, rp_cb, p, NULL);
        return;
    }
    p->is_local = FALSE;
    {
        hb_obj *h = NULL;
        char *sub = NULL;
        const char *nm = host_id, *os = "generic";
        if (hb_vault_get(hb_store_vault(p->v->store), host_id,
                         &h) == HB_OK && h) {
            const char *s = hb_obj_get_str(h, "name");
            const char *a = hb_obj_get_str(h, "address");
            const char *o2 = hb_obj_get_str(h, "os");
            int port = (int)hb_obj_get_int(h, "port", 22);
            if (s && *s) nm = s;
            if (o2 && *o2 && strcmp(o2, "auto")) os = o2;
            sub = g_strdup_printf("SFTP · SSH %s:%d",
                                  a ? a : "?", port);
        }
        g_free(p->hname); p->hname = g_strdup(nm);
        g_free(p->os_key); p->os_key = g_strdup(os);
        hb_connect_view_set_head(HB_CONNECT_VIEW(p->conn_view),
                                 p->os_key, p->hname,
                                 sub ? sub : "SFTP");
        g_free(sub);
        if (h) hb_obj_free(h);
    }
    hb_connect_view_reset(HB_CONNECT_VIEW(p->conn_view));
    pane_set_state(p, PS_CONN);
    if (!p->ctl) {
        p->ctl = hb_conn_ctl_new(p->v->store,
                                 GTK_WIDGET(p->v), HB_CONN_SFTP);
        g_signal_connect(p->ctl, "stage",
                         G_CALLBACK(on_ctl_stage), p);
        g_signal_connect(p->ctl, "session",
                         G_CALLBACK(on_ctl_sess), p);
        g_signal_connect(p->ctl, "connected",
                         G_CALLBACK(on_ctl_conn), p);
        g_signal_connect(p->ctl, "failed",
                         G_CALLBACK(on_ctl_fail), p);
        g_signal_connect(p->ctl, "disconnected",
                         G_CALLBACK(on_ctl_disconn), p);
        g_signal_connect(p->ctl, "exited",
                         G_CALLBACK(on_ctl_exited), p);
    }
    hb_conn_ctl_start(p->ctl, host_id, NULL, 0, 0);
}

/* ------------------------- Select Host 状态 ------------------------- */

static void select_row_pick(GtkButton *b, gpointer ud)
{
    Pane *p = g_object_get_data(G_OBJECT(b), "pane");
    (void)ud;
    const char *id = g_object_get_data(G_OBJECT(b), "hid");
    pane_pick(p, id);
}
static void select_local(GtkButton *b, gpointer ud)
{ (void)b; pane_pick((Pane *)ud, NULL); }

static void rebuild_host_list(Pane *p)
{
    GtkWidget *ch;
    hb_obj **hosts;
    size_t n, i;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(p->sel_entry));
    while ((ch = gtk_widget_get_first_child(p->host_box)))
        gtk_widget_unparent(ch);
    hosts = hb_store_list(p->v->store, "host", &n);
    for (i = 0; i < n; i++) {
        const char *pr = hb_obj_get_str(hosts[i], "proto");
        const char *nm;
        if (pr && strcmp(pr, "ssh")) continue;   /* 只列 SSH 主机 */
        nm = hb_obj_get_str(hosts[i], "name");
        const char *us = hb_obj_get_str(hosts[i], "user");
        const char *os = hb_obj_get_str(hosts[i], "os");
        GtkWidget *b, *row, *col;
        char *sub;
        if (!nm) nm = hb_obj_get_str(hosts[i], "address");
        if (q && *q && !(nm && strstr(nm, q)) &&
            !(us && strstr(us, q)))
            continue;
        b = gtk_button_new();
        gtk_widget_add_css_class(b, "select-host-item");
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(row),
            hb_os_avatar_new(os && *os && strcmp(os, "auto")
                             ? os : "generic", 30));
        col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        {
            GtkWidget *t = gtk_label_new(nm ? nm : "?");
            gtk_widget_add_css_class(t, "h-name");
            gtk_label_set_xalign(GTK_LABEL(t), 0);
            gtk_box_append(GTK_BOX(col), t);
        }
        sub = g_strdup_printf("ssh, %s", us ? us : "");
        {
            GtkWidget *t = gtk_label_new(sub);
            gtk_widget_add_css_class(t, "h-sub");
            gtk_label_set_xalign(GTK_LABEL(t), 0);
            gtk_box_append(GTK_BOX(col), t);
        }
        g_free(sub);
        gtk_box_append(GTK_BOX(row), col);
        gtk_button_set_child(GTK_BUTTON(b), row);
        g_object_set_data(G_OBJECT(b), "pane", p);
        g_object_set_data_full(G_OBJECT(b), "hid",
            g_strdup(hb_obj_id(hosts[i])), g_free);
        g_signal_connect(b, "clicked",
                         G_CALLBACK(select_row_pick), NULL);
        gtk_box_append(GTK_BOX(p->host_box), b);
    }
    if (hosts) hb_obj_list_free(hosts, n);
}

static void on_sel_filter(GtkEditable *e, gpointer ud)
{ (void)e; (void)ud; rebuild_host_list((Pane *)ud); }

static GtkWidget *build_select(Pane *p)
{
    GtkWidget *box, *head, *b, *scroll, *hint;
    const char *sidename = p->side == 0 ? "左栏" : "右栏";
    char *t;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "sftp-pane");

    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(head, "sftp-head");
    t = g_strdup_printf("Select Host · %s", sidename);
    {
        GtkWidget *l = gtk_label_new(t);
        gtk_widget_add_css_class(l, "h-name");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(head), l);
    }
    g_free(t);
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(head), sp);
    }
    b = gtk_button_new();
    gtk_widget_add_css_class(b, "hb-btn");
    gtk_widget_add_css_class(b, "primary");
    gtk_widget_add_css_class(b, "sm");
    {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_box_append(GTK_BOX(r), hb_icon_new("laptop", 13));
        gtk_box_append(GTK_BOX(r), gtk_label_new("Local"));
        gtk_button_set_child(GTK_BUTTON(b), r);
    }
    g_signal_connect(b, "clicked", G_CALLBACK(select_local), p);
    gtk_box_append(GTK_BOX(head), b);
    gtk_box_append(GTK_BOX(box), head);

    /* 过滤条 */
    {
        GtkWidget *f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(f, "sftp-head");
        gtk_box_append(GTK_BOX(f), hb_icon_new("search", 13));
        p->sel_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(p->sel_entry),
                                       "过滤主机…");
        gtk_widget_set_hexpand(p->sel_entry, TRUE);
        g_signal_connect(p->sel_entry, "changed",
                         G_CALLBACK(on_sel_filter), p);
        gtk_box_append(GTK_BOX(f), p->sel_entry);
        gtk_box_append(GTK_BOX(box), f);
    }

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    p->host_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(p->host_box, "select-host-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  p->host_box);
    gtk_box_append(GTK_BOX(box), scroll);

    hint = gtk_label_new(
        "选择两端后可双栏互传（远端↔远端 / 远端↔本地）· 双击文件即开始传输");
    gtk_widget_add_css_class(hint, "hb-dim");
    gtk_widget_add_css_class(hint, "sftp-empty");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_margin_top(hint, 10);
    gtk_widget_set_margin_bottom(hint, 12);
    gtk_box_append(GTK_BOX(box), hint);

    rebuild_host_list(p);
    return box;
}

/* ------------------------- Browser 状态 ------------------------- */

/* --- 模型 / 过滤 / 排序 --- */

static gboolean filter_fn(gpointer item, gpointer ud)
{
    HbFileItem *i = item;
    Pane *p = ud;
    const char *q;
    if (!p->show_hidden && i->hidden) return FALSE;
    q = gtk_editable_get_text(GTK_EDITABLE(p->filter_entry));
    if (q && *q && !strstr(i->name, q)) return FALSE;
    return TRUE;
}

static void filter_changed(Pane *p)
{
    gtk_filter_changed(GTK_FILTER(p->filter),
                       GTK_FILTER_CHANGE_DIFFERENT);
}

static int dirfirst_cmp(gconstpointer a, gconstpointer b, gpointer ud)
{
    const HbFileItem *x = a, *y = b;
    (void)ud;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return 0;
}

static int name_cmp(gconstpointer a, gconstpointer b, gpointer ud)
{
    const HbFileItem *x = a, *y = b;
    (void)ud;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return g_utf8_collate(x->name, y->name);
}
static int mtime_cmp(gconstpointer a, gconstpointer b, gpointer ud)
{
    const HbFileItem *x = a, *y = b;
    (void)ud;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}
static int size_cmp(gconstpointer a, gconstpointer b, gpointer ud)
{
    const HbFileItem *x = a, *y = b;
    (void)ud;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return x->size < y->size ? -1 : x->size > y->size;
}
static int mode_cmp(gconstpointer a, gconstpointer b, gpointer ud)
{
    const HbFileItem *x = a, *y = b;
    (void)ud;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return x->mode < y->mode ? -1 : x->mode > y->mode;
}

/* --- cell 工厂 --- */

static GtkWidget *fac_cell(GtkListItem *li)
{
    GtkWidget *w = gtk_list_item_get_child(li), *ch;
    while ((ch = gtk_widget_get_first_child(w)))
        gtk_widget_unparent(ch);
    return w;
}

static void fac_setup(GtkSignalListItemFactory *f, GtkListItem *li,
                      gpointer ud)
{
    Pane *p = ud;
    GtkWidget *w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    (void)f;
    g_object_set_data(G_OBJECT(w), "pane", p);
    g_object_set_data(G_OBJECT(w), "li", li);
    gtk_list_item_set_child(li, w);
}

static void fac_bind_name(GtkSignalListItemFactory *f, GtkListItem *li,
                          gpointer ud)
{
    HbFileItem *i = gtk_list_item_get_item(li);
    GtkWidget *w = fac_cell(li);
    (void)f; (void)ud;
    gtk_box_append(GTK_BOX(w),
        hb_icon_new(i->is_dir ? "folder"
                    : i->is_link ? "link" : "file", 14));
    {
        GtkWidget *l = gtk_label_new(i->name);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_margin_start(l, 6);
        gtk_box_append(GTK_BOX(w), l);
    }
    gtk_widget_set_tooltip_text(w, i->path);
}

static void fac_bind_mtime(GtkSignalListItemFactory *f, GtkListItem *li,
                           gpointer ud)
{
    HbFileItem *i = gtk_list_item_get_item(li);
    GtkWidget *w = fac_cell(li);
    GDateTime *dt;
    char *s;
    (void)f; (void)ud;
    dt = g_date_time_new_from_unix_local(i->mtime);
    s = dt ? g_date_time_format(dt, "%m-%d %H:%M") : g_strdup("-");
    if (dt) g_date_time_unref(dt);
    gtk_box_append(GTK_BOX(w), gtk_label_new(s));
    g_free(s);
}

static void fac_bind_size(GtkSignalListItemFactory *f, GtkListItem *li,
                          gpointer ud)
{
    HbFileItem *i = gtk_list_item_get_item(li);
    GtkWidget *w = fac_cell(li);
    char *s;
    (void)f; (void)ud;
    s = i->is_dir ? g_strdup("—") : g_format_size(i->size);
    gtk_box_append(GTK_BOX(w), gtk_label_new(s));
    g_free(s);
}

static void fac_bind_kind(GtkSignalListItemFactory *f, GtkListItem *li,
                          gpointer ud)
{
    HbFileItem *i = gtk_list_item_get_item(li);
    GtkWidget *w = fac_cell(li);
    const char *k;
    (void)f; (void)ud;
    k = i->is_dir ? "folder" : i->is_link ? "link" : "file";
    gtk_box_append(GTK_BOX(w), gtk_label_new(k));
}

static void fac_bind_mode(GtkSignalListItemFactory *f, GtkListItem *li,
                          gpointer ud)
{
    HbFileItem *i = gtk_list_item_get_item(li);
    GtkWidget *w = fac_cell(li);
    char s[10];
    int m = i->mode & 0777, j = 0, k;
    static const char *rwx = "rwx";
    (void)f; (void)ud;
    for (k = 0; k < 9; k++)
        s[j++] = (m & (1 << (8 - k))) ? rwx[k % 3] : '-';
    s[j] = 0;
    {
        GtkWidget *l = gtk_label_new(s);
        gtk_widget_add_css_class(l, "hb-mono");
        gtk_box_append(GTK_BOX(w), l);
    }
}

/* --- 列表操作 --- */

typedef struct { Pane *p; guint gen; } ListReq;

static void list_cb(const hb_fs_result *r, gpointer ud);

static void pane_refresh(Pane *p)
{
    ListReq *lr;
    if (!p->fs || !p->cwd) return;
    p->loading = TRUE;
    lr = g_new0(ListReq, 1);
    lr->p = p;
    lr->gen = ++p->list_gen;
    hb_fs_list(p->fs, p->cwd, list_cb, lr, g_free);
}

static void list_cb(const hb_fs_result *r, gpointer ud)
{
    ListReq *lr = ud;
    Pane *p = lr->p;
    size_t k;
    /* 过期结果（导航后旧目录的 list 才返回）丢弃，否则会回显旧目录 */
    if (lr->gen != p->list_gen)
        return;
    p->loading = FALSE;
    if (!r->ok) {
        char *t = g_strdup_printf("读取目录失败: %s",
            r->message ? r->message : "未知错误");
        toast(p->v, t);
        g_free(t);
        return;
    }
    /* 大目录不能逐项 append（每项一次 items-changed → O(n²)，
     * 期间主线程被占满、视图停在旧目录），一次性 splice 替换 */
    {
        GPtrArray *items = g_ptr_array_sized_new(r->n ? r->n : 1);
        g_ptr_array_set_free_func(items, g_object_unref);
        for (k = 0; k < r->n; k++) {
            if (!strcmp(r->entries[k].name, ".") ||
                !strcmp(r->entries[k].name, "..") ||
                strchr(r->entries[k].name, '/'))
                continue;
            g_ptr_array_add(items,
                            file_item_new(&r->entries[k], p->cwd));
        }
        g_list_store_splice(p->model, 0,
            g_list_model_get_n_items(G_LIST_MODEL(p->model)),
            items->pdata, items->len);
        g_ptr_array_free(items, TRUE);
    }
}

static void nav_sync(Pane *p)
{
    gtk_widget_set_sensitive(p->back_b, p->back->len > 0);
    gtk_widget_set_sensitive(p->fwd_b, p->fwd->len > 0);
    gtk_widget_set_sensitive(p->up_b,
        p->cwd && strcmp(p->cwd, "/") != 0);
    /* crumbs 重建 */
    {
        GtkWidget *ch;
        char *dup, *save, *seg;
        GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
        int k;
        while ((ch = gtk_widget_get_first_child(p->crumbs)))
            gtk_widget_unparent(ch);
        dup = g_strdup(p->cwd ? p->cwd : "/");
        for (seg = strtok_r(dup, "/", &save); seg;
             seg = strtok_r(NULL, "/", &save))
            g_ptr_array_add(parts, g_strdup(seg));
        g_free(dup);
        /* root crumb */
        {
            GtkWidget *c = gtk_button_new_with_label("/");
            gtk_widget_add_css_class(c, "crumb");
            g_object_set_data(G_OBJECT(c), "pane", p);
            g_object_set_data(G_OBJECT(c), "depth", GINT_TO_POINTER(0));
            g_signal_connect(c, "clicked",
                G_CALLBACK(on_crumb), NULL);
            gtk_box_append(GTK_BOX(p->crumbs), c);
        }
        for (k = 0; k < (int)parts->len; k++) {
            GtkWidget *c = gtk_button_new_with_label(parts->pdata[k]);
            GtkWidget *s = gtk_label_new("›");
            gtk_widget_add_css_class(s, "hb-dim");
            gtk_box_append(GTK_BOX(p->crumbs), s);
            gtk_widget_add_css_class(c, "crumb");
            g_object_set_data(G_OBJECT(c), "pane", p);
            g_object_set_data(G_OBJECT(c), "depth",
                              GINT_TO_POINTER(k + 1));
            g_signal_connect(c, "clicked",
                G_CALLBACK(on_crumb), NULL);
            gtk_box_append(GTK_BOX(p->crumbs), c);
        }
        g_ptr_array_free(parts, TRUE);
    }
    gtk_label_set_label(
        GTK_LABEL(g_object_get_data(G_OBJECT(p->browse_box),
                                    "title")),
        p->is_local ? "Local" : p->hname);
}

/* 进入目录；nav=TRUE 时把旧 cwd 压入 back 并清 fwd */
static void pane_browse_to(Pane *p, const char *path, gboolean nav)
{
    if (nav && p->cwd) {
        g_ptr_array_add(p->back, g_strdup(p->cwd));
        g_ptr_array_set_size(p->fwd, 0);
    }
    g_free(p->cwd);
    p->cwd = g_strdup(path && *path ? path : "/");
    pane_set_state(p, PS_BROWSE);
    nav_sync(p);
    pane_refresh(p);
}

/* crumb click handler（用普通函数，非闭包） */
static void on_crumb(GtkButton *b, gpointer ud)
{
    Pane *p = g_object_get_data(G_OBJECT(b), "pane");
    int depth = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(b), "depth"));
    const char *q;
    char *base, *t;
    (void)ud;
    if (!p->cwd) return;
    if (depth == 0) { pane_browse_to(p, "/", TRUE); return; }
    /* 取 cwd 前 depth 段 */
    {
        char **sp = g_strsplit(p->cwd, "/", -1);
        GString *s = g_string_new("");
        int k, n = 0;
        for (k = 0; sp[k] && n < depth; k++) {
            if (!*sp[k]) continue;
            g_string_append_c(s, '/');
            g_string_append(s, sp[k]);
            n++;
        }
        base = s->len ? g_strdup(s->str) : g_strdup("/");
        g_string_free(s, TRUE);
        g_strfreev(sp);
    }
    q = p->cwd;
    t = base;
    if (q && !strcmp(q, t)) { g_free(t); return; }
    pane_browse_to(p, t, TRUE);
    g_free(t);
}

static void on_up(GtkButton *b, gpointer ud)
{
    Pane *p = ud;
    char *d;
    (void)b;
    if (!p->cwd || !strcmp(p->cwd, "/")) return;
    d = parent_dir(p->cwd);
    pane_browse_to(p, d, TRUE);
    g_free(d);
}
static void on_back(GtkButton *b, gpointer ud)
{
    Pane *p = ud;
    char *t;
    (void)b;
    if (!p->back->len) return;
    t = g_strdup(p->back->pdata[p->back->len - 1]);
    g_ptr_array_remove_index(p->back, p->back->len - 1);
    if (p->cwd) g_ptr_array_add(p->fwd, g_strdup(p->cwd));
    g_free(p->cwd);
    p->cwd = t;
    nav_sync(p);
    pane_refresh(p);
}
static void on_fwd(GtkButton *b, gpointer ud)
{
    Pane *p = ud;
    char *t;
    (void)b;
    if (!p->fwd->len) return;
    t = g_strdup(p->fwd->pdata[p->fwd->len - 1]);
    g_ptr_array_remove_index(p->fwd, p->fwd->len - 1);
    if (p->cwd) g_ptr_array_add(p->back, g_strdup(p->cwd));
    g_free(p->cwd);
    p->cwd = t;
    nav_sync(p);
    pane_refresh(p);
}

static void on_filter_toggle(GtkButton *b, gpointer ud)
{
    Pane *p = ud;
    gboolean on = !gtk_widget_get_visible(p->filter_row);
    (void)b;
    gtk_widget_set_visible(p->filter_row, on);
    if (on) gtk_widget_grab_focus(p->filter_entry);
    else {
        gtk_editable_set_text(GTK_EDITABLE(p->filter_entry), "");
        filter_changed(p);
    }
}
static void on_filter_text(GtkEditable *e, gpointer ud)
{ (void)e; filter_changed((Pane *)ud); }

/* 选中项 → HbFileItem* 列表（调用方 g_list_free；items 借用） */
static GList *sel_items(Pane *p)
{
    GList *out = NULL;
    GtkBitset *bs = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(p->sel));
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->sel));
    GtkBitsetIter it;
    guint pos;
    if (gtk_bitset_iter_init_first(&it, bs, &pos)) {
        do {
            if (pos < n)
                out = g_list_prepend(out,
                    g_list_model_get_item(G_LIST_MODEL(p->sel), pos));
        } while (gtk_bitset_iter_next(&it, &pos));
    }
    gtk_bitset_unref(bs);
    /* items 是 get_item 来的 ref → 用完 unref */
    return g_list_reverse(out);
}
static void sel_items_free(GList *l)
{ g_list_free_full(l, g_object_unref); }

/* ================= 传输 ================= */

typedef enum { X_UP, X_DOWN, X_RELAY, X_LOCAL } XKind;
typedef enum { XS_RUN, XS_DONE, XS_FAIL, XS_CANCEL } XState;

struct _Xfer {
    HbSftpView *v;
    int      id;
    XKind    kind;
    XState   state;
    gboolean skipped;           /* 整体被跳过（SKIP 冲突） */
    Pane    *sp, *dp;         /* 借用；session 断则 fail */
    char    *name;
    char    *src_path, *dst_dir;
    char    *src_label, *dst_label;
    hb_sftp_conflict policy;
    guint    req;
    hb_session *sess;         /* 当前活动 req 所在 session */
    /* relay */
    char    *tmp_dir, *tmp_path;
    int      phase;           /* 0=download 1=upload */
    /* local copy */
    GCancellable *canc;
    gpointer    lcop;
    /* progress */
    uint64_t done, total;
    uint64_t acc;             /* 累计完成字节（跨 relay 两阶段） */
    gint64   t0;              /* 入队时刻 us */
    double   ema;             /* bytes/s */
    gint64   ema_t;           /* last ema-update sample us */
    uint64_t ema_done;
    char    *err;
    guint    tick;
    /* row */
    GtkWidget *row, *fill, *pct, *dir_lbl, *xbtn;
};

static GList *xfer_by_req(GList *l, guint req, hb_session *s)
{
    for (; l; l = l->next) {
        Xfer *x = l->data;
        if (x->state == XS_RUN && x->req == req &&
            (!s || x->sess == s))
            return l;
    }
    return NULL;
}

static void xfer_update_row(Xfer *x)
{
    char *t;
    double pct = x->total ? (double)x->done / x->total * 100.0
                          : (x->done ? 100.0 : 0.0);
    if (x->state == XS_DONE) pct = 100;   /* 完成态：填满进度条 */
    if (pct > 100) pct = 100;
    gtk_widget_set_size_request(x->fill,
        (int)(pct * 2.2), -1);     /* track 220px */
    if (x->state == XS_RUN) {
        char *sp = g_format_size((guint64)x->ema);
        char *eta = NULL;
        if (x->total && x->ema > 0 && x->done < x->total) {
            int s = (int)((x->total - x->done) / x->ema);
            eta = g_strdup_printf(" · ~%ds", s > 0 ? s : 1);
        }
        t = g_strdup_printf("%d%%%s%s · %s/s", (int)pct,
                            x->phase == 1 ? " · 上传" :
                            x->kind == X_RELAY ? " · 下载" : "",
                            eta ? eta : "", sp);
        g_free(eta);
        gtk_label_set_label(GTK_LABEL(x->pct), t);
        g_free(t); g_free(sp);
    } else if (x->state == XS_DONE) {
        double el = (g_get_monotonic_time() - x->t0) / 1e6;
        /* 完成后显示平均速度（字节/全程耗时），不受瞬时突发影响 */
        if (el > 0.05 && x->acc) {
            char *sp = g_format_size((guint64)(x->acc / el));
            char *tt = g_strdup_printf("已完成 · %s/s", sp);
            gtk_label_set_label(GTK_LABEL(x->pct), tt);
            g_free(tt); g_free(sp);
        } else
            gtk_label_set_label(GTK_LABEL(x->pct), "已完成");
        if (x->skipped)
            gtk_label_set_label(GTK_LABEL(x->pct), "已跳过");
        gtk_widget_add_css_class(x->fill, "xfer-done");
    } else if (x->state == XS_FAIL) {
        t = g_strdup_printf("失败: %s",
                            x->err ? x->err : "未知错误");
        gtk_label_set_label(GTK_LABEL(x->pct), t);
        g_free(t);
        gtk_widget_add_css_class(x->fill, "xfer-fail");
    } else {
        gtk_label_set_label(GTK_LABEL(x->pct), "已取消");
        gtk_widget_add_css_class(x->fill, "xfer-cancel");
    }
}

static void xhead_sync(HbSftpView *v)
{
    int act = 0, term = 0;
    GList *l;
    for (l = v->xers; l; l = l->next) {
        Xfer *x = l->data;
        if (x->state == XS_RUN) act++; else term++;
    }
    gtk_widget_set_visible(v->xwrap, TRUE);   /* 队列栏常驻 */
    {
        char *t = g_strdup_printf("传输队列 · %d 进行中", act);
        gtk_label_set_label(GTK_LABEL(v->xhead), t);
        g_free(t);
    }
    gtk_widget_set_visible(v->xclear, term > 0);
}

static void xfer_free(Xfer *x)
{
    g_free(x->name);
    g_free(x->src_path);
    g_free(x->dst_dir);
    g_free(x->src_label);
    g_free(x->dst_label);
    g_free(x->tmp_dir);
    g_free(x->tmp_path);
    g_free(x->err);
    if (x->canc) g_object_unref(x->canc);
    if (x->tick) g_source_remove(x->tick);
    g_free(x);
}

static void xfer_remove_row(Xfer *x)
{
    if (x->row) { gtk_widget_unparent(x->row); x->row = NULL; }
}

static void xfer_cleanup_tmp(Xfer *x)
{
    if (x->tmp_path) g_remove(x->tmp_path);
    if (x->tmp_dir) g_rmdir(x->tmp_dir);
    g_clear_pointer(&x->tmp_path, g_free);
    g_clear_pointer(&x->tmp_dir, g_free);
}

static void xfer_notify(Xfer *x, XState st)
{
    gboolean ok = st == XS_DONE;
    const char *kind = ok ? "ok" : st == XS_CANCEL ? "info" : "err";
    const char *verb = ok ? "传输完成" :
                       st == XS_CANCEL ? "传输已取消" : "传输失败";
    char *t = g_strdup_printf("%s · %s", verb, x->name);
    char *b = g_strdup_printf("%s → %s",
        x->src_label ? x->src_label : "?",
        x->dst_label ? x->dst_label : "?");
    hb_notify_post(kind, t, b, x->v->tab_id);
    if (view_tab_focused(x->v)) {
        toast(x->v, t);
    } else {
        HbWindow *w = view_win(x->v);
        if (w && x->v->tab_id)
            hb_window_set_tab_dot(w, x->v->tab_id,
                ok ? "ok" : st == XS_CANCEL ? "pending" : "err");
        /* 桌面通知（系统通知）在非焦点时投递 */
        {
            GNotification *n = g_notification_new(t);
            GApplication *app = g_application_get_default();
            g_notification_set_body(n, b);
            if (app) g_application_send_notification(app, NULL, n);
            g_object_unref(n);
        }
    }
    g_free(t); g_free(b);
}

static void xfer_finish(Xfer *x, XState st, const char *err)
{
    x->state = st;
    x->req = 0;
    x->sess = NULL;
    if (err) { g_free(x->err); x->err = g_strdup(err); }
    if (st == XS_DONE || st == XS_FAIL || st == XS_CANCEL) {
        xfer_cleanup_tmp(x);
        if (x->canc) { g_object_unref(x->canc); x->canc = NULL; }
        gtk_widget_set_visible(x->xbtn, st == XS_FAIL);
        if (x->xbtn && st == XS_FAIL)
            gtk_button_set_label(GTK_BUTTON(x->xbtn), "重试");
        if (st != XS_RUN) xfer_notify(x, st);
    }
    xfer_update_row(x);
    xhead_sync(x->v);
    /* 目的端刷新 */
    if (st == XS_DONE && x->dp && x->dp->state == PS_BROWSE)
        pane_refresh(x->dp);
}

/* SFTP DONE/PROGRESS 事件分发（owner 在每个 ev 上调用） */
static void xfer_dispatch(Pane *p, const hb_sftp_result *r)
{
    HbSftpView *v = p->v;
    GList *l = xfer_by_req(v->xers, r->req, p->sess);
    Xfer *x;
    gint64 now;
    if (!l) return;
    x = l->data;
    now = g_get_monotonic_time();
    if (r->kind == HB_SFTP_R_PROGRESS) {
        uint64_t dd = r->done;
        /* 累计字节（relay 换相时 done 会归零，按增量累加） */
        x->acc += dd >= x->done ? dd - x->done : dd;
        x->done = dd;
        /* EMA 只在 ≥250ms 的窗口上重算：间隔太短时
         * 单个大块会让瞬时速率（字节/秒）虚高 */
        if (!x->ema_t) { x->ema_t = now; x->ema_done = dd; }
        else if (dd > x->ema_done) {
            double dt = (now - x->ema_t) / 1e6;
            if (dt >= 0.25) {
                double inst = (dd - x->ema_done) / dt;
                x->ema = x->ema ? x->ema * 0.6 + inst * 0.4
                                : inst;
                x->ema_t = now; x->ema_done = dd;
            }
        }
        x->total = r->total;
        xfer_update_row(x);
        return;
    }
    /* DONE */
    if (x->kind == X_RELAY && x->phase == 0 &&
        r->status == HB_SFTP_OK) {
        /* 第一阶段完成 → 上传 */
        x->phase = 1;
        x->done = 0; x->total = 0;
        {
            char *t = path_join(x->dst_dir, x->name);
            x->req = hb_sftp_upload(x->dp->sess, x->tmp_path, t,
                                    x->policy);
            g_free(t);
        }
        x->sess = x->dp->sess;
        xfer_update_row(x);
        return;
    }
    if (r->skipped) x->skipped = TRUE;
    if (r->status == HB_SFTP_OK || r->skipped)
        xfer_finish(x, XS_DONE, NULL);
    else if (r->status == HB_SFTP_ERR_CANCELLED)
        xfer_finish(x, XS_CANCEL, NULL);
    else
        xfer_finish(x, XS_FAIL,
                    r->message ? r->message : "传输失败");
}

/* session 断开 → 其上所有 run 状态 xfer fail */
static void xfer_fail_session(HbSftpView *v, hb_session *s,
                              const char *why)
{
    GList *l;
    for (l = v->xers; l; l = l->next) {
        Xfer *x = l->data;
        if (x->state == XS_RUN &&
            (x->sess == s || (x->sp && x->sp->sess == s) ||
             (x->dp && x->dp->sess == s)))
            xfer_finish(x, XS_FAIL, why ? why : "连接已断开");
    }
}

/* ---- local copy（GTask 线程，带进度的 256K 块拷贝） ---- */

typedef struct {
    Xfer   *x;
    char   *src, *dst;
    int     policy;         /* hb_sftp_conflict */
    gboolean ok, skipped, cancelled;
    char   *err;
    uint64_t done, total;
    unsigned nskip;         /* 递归中跳过的链接/特殊文件 */
    char   *created;        /* 本次新建的最外层目录（取消时安全清除） */
    char   *partial;        /* 写入中的新建文件（取消时清除） */
} LCOp;

/* 找一个空闲的 "name (N)" 路径；冲突策略 RENAME 用。
 * 规则同 core ren_variant：文件在最后的 '.' 前插入
 * （big.bin→big (1).bin、archive.tar.gz→archive.tar (1).gz）；
 * 此外目录、点文件（'.' 在首字符）、无扩展名一律尾部追加
 * （sub→sub (1)、.bashrc→.bashrc (1)）。 */
static char *lc_rename_free(const char *path, gboolean is_dir)
{
    char *dir = g_path_get_dirname(path);
    char *base = g_path_get_basename(path);
    const char *dot = is_dir ? NULL : strrchr(base, '.');
    int stem;
    int k;
    if (dot == base) dot = NULL;        /* 点文件不算扩展名 */
    stem = dot ? (int)(dot - base) : (int)strlen(base);
    for (k = 1; k < 1000; k++) {
        char *nb = g_strdup_printf("%.*s (%d)%s",
                                   stem, base, k, dot ? dot : "");
        char *np = g_build_filename(dir, nb, NULL);
        struct stat st;
        g_free(nb);
        if (g_lstat(np, &st) != 0) {
            g_free(dir); g_free(base);
            return np;
        }
        g_free(np);
    }
    g_free(dir); g_free(base);
    return NULL;
}

/* 依冲突策略解析目标路径：NULL+skipped=跳过，NULL+err=失败 */
static char *lc_dst(LCOp *o, const char *dst, gboolean is_dir,
                    gboolean *skipped)
{
    struct stat st;
    *skipped = FALSE;
    if (g_lstat(dst, &st) != 0) return g_strdup(dst);  /* 不存在 */
    /* 目录冲突与核心一致：任何策略都并入既有目录，
     * 冲突策略对目录内同名文件逐个生效 */
    if (is_dir && S_ISDIR(st.st_mode))
        return g_strdup(dst);
    if (o->policy == HB_SFTP_SKIP) { *skipped = TRUE; return NULL; }
    if (o->policy == HB_SFTP_RENAME) {
        char *np = lc_rename_free(dst, is_dir);
        if (!np) o->err = g_strdup("无法生成重命名目标");
        return np;
    }
    /* OVERWRITE：文件截断；目标是目录则失败 */
    if (S_ISDIR(st.st_mode) && !is_dir) {
        o->err = g_strdup("目标是目录");
        return NULL;
    }
    return g_strdup(dst);
}

/* 累加 regular-file 字节数供进度条用（lstat，不跟随链接） */
static void lc_total(LCOp *o, const char *path, gboolean top)
{
    struct stat st;
    if (top ? stat(path, &st) != 0 : lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(path, 0, NULL);
        const gchar *nm;
        if (!d) return;
        while ((nm = g_dir_read_name(d))) {
            char *full = g_build_filename(path, nm, NULL);
            lc_total(o, full, FALSE);
            g_free(full);
        }
        g_dir_close(d);
    } else if (S_ISREG(st.st_mode))
        o->total += (uint64_t)st.st_size;
    /* 链接与非普通文件不计入（递归会跳过） */
}

static gboolean lc_copy_file(LCOp *o, const char *src, const char *dst,
                             GCancellable *c, mode_t mode)
{
    int in = -1, out = -1;
    char buf[262144];
    ssize_t n;
    gboolean ok = FALSE;
    {
        struct stat ex;
        if (g_lstat(dst, &ex) != 0) {
            g_free(o->partial);
            o->partial = g_strdup(dst);   /* 新建文件，取消可清 */
        }
    }
    in = open(src, O_RDONLY);
    if (in < 0) { o->err = g_strdup(g_strerror(errno)); return FALSE; }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (out < 0) { o->err = g_strdup(g_strerror(errno)); goto e; }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t w = 0;
        if (g_cancellable_is_cancelled(c)) { o->cancelled = TRUE; goto e; }
        while (w < n) {
            ssize_t k = write(out, buf + w, (size_t)(n - w));
            if (k < 0) { o->err = g_strdup(g_strerror(errno)); goto e; }
            w += k;
        }
        o->done += (uint64_t)n;
    }
    if (n < 0) o->err = g_strdup(g_strerror(errno));
    else { fsync(out); ok = TRUE; }
e:
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return ok;
}

/* 递归拷贝：top 层跟随符号链接（显式选择项），
 * 递归内部跳过符号链接与特殊文件（不跟随、不复制）。 */
static gboolean lc_walk(LCOp *o, const char *src, const char *dst,
                        gboolean top, GCancellable *c)
{
    struct stat st;
    gboolean skipped = FALSE;
    char *rd;
    if (g_cancellable_is_cancelled(c)) { o->cancelled = TRUE; return FALSE; }
    if (top ? stat(src, &st) != 0 : lstat(src, &st) != 0) {
        o->err = g_strdup(g_strerror(errno));
        return FALSE;
    }
    if (!top && S_ISLNK(st.st_mode)) { o->nskip++; return TRUE; }
    if (S_ISDIR(st.st_mode)) {
        GDir *d;
        const gchar *nm;
        gboolean ok = TRUE;
        rd = lc_dst(o, dst, TRUE, &skipped);
        if (!rd) {
            if (skipped) { o->nskip++; return TRUE; }
            if (!o->err) o->err = g_strdup("无法解析目标路径");
            return FALSE;
        }
        {
            struct stat ex;
            if (g_lstat(rd, &ex) != 0) {
                if (g_mkdir(rd, st.st_mode & 0777) != 0) {
                    o->err = g_strdup(g_strerror(errno));
                    g_free(rd);
                    return FALSE;
                }
                /* 记录最外层新建目录：取消时只清自己创建的 */
                if (!o->created) o->created = g_strdup(rd);
            }
        }
        d = g_dir_open(src, 0, NULL);
        if (!d) {
            o->err = g_strdup(g_strerror(errno));
            g_free(rd);
            return FALSE;
        }
        while (ok && (nm = g_dir_read_name(d))) {
            char *cs = g_build_filename(src, nm, NULL);
            char *cd = g_build_filename(rd, nm, NULL);
            ok = lc_walk(o, cs, cd, FALSE, c);
            g_free(cs); g_free(cd);
        }
        g_dir_close(d);
        g_free(rd);
        return ok;
    }
    if (!S_ISREG(st.st_mode)) { o->nskip++; return TRUE; }
    rd = lc_dst(o, dst, FALSE, &skipped);
    if (!rd) {
        if (skipped) { o->nskip++; return TRUE; }
        return FALSE;
    }
    {
        gboolean ok = lc_copy_file(o, src, rd, c, st.st_mode);
        g_free(rd);
        return ok;
    }
}

static void lc_work(GTask *t, gpointer src, gpointer data,
                    GCancellable *c)
{
    LCOp *o = data;
    gboolean top_skipped = FALSE;
    char *rd;
    struct stat st;
    (void)src;
    /* 顶层存在性 + 冲突策略先行：SKIP → 整体跳过，
     * RENAME → 顶层落 "name (1)"，OVERWRITE → 覆盖/合并 */
    if (stat(o->src, &st) != 0) {
        o->err = g_strdup(g_strerror(errno));
        g_task_return_boolean(t, FALSE);
        return;
    }
    rd = lc_dst(o, o->dst, S_ISDIR(st.st_mode), &top_skipped);
    if (!rd) {
        if (top_skipped) {
            o->skipped = TRUE;
            g_task_return_boolean(t, TRUE);
            return;
        }
        if (!o->err) o->err = g_strdup("无法解析目标路径");
        g_task_return_boolean(t, FALSE);
        return;
    }
    lc_total(o, o->src, TRUE);
    if (lc_walk(o, o->src, rd, TRUE, c) && !o->cancelled && !o->err)
        o->ok = TRUE;
    g_free(rd);
    g_task_return_boolean(t, o->ok);
}

static gboolean lc_tick(gpointer ud);
static void lc_rm_rf(const char *path);

static void lc_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    LCOp *o = ud;
    Xfer *x = o->x;
    (void)src;
    x->lcop = NULL;
    g_task_propagate_boolean(G_TASK(res), NULL);
    if (x->state != XS_RUN || o->cancelled) {  /* 已被取消/断连 */
        if (o->created) lc_rm_rf(o->created);
        else if (o->partial) g_remove(o->partial);
    }
    if (x->state != XS_RUN) {
        /* 状态已被外部置位（取消/断开），不再 xfer_finish */
    } else if (o->err)
        xfer_finish(x, XS_FAIL, o->err);
    else if (o->cancelled) {
        xfer_finish(x, XS_CANCEL, NULL);
    } else if (o->skipped) {
        x->skipped = TRUE;
        xfer_finish(x, XS_DONE, NULL);
    } else if (o->ok)
        xfer_finish(x, XS_DONE, NULL);
    else
        xfer_finish(x, XS_CANCEL, NULL);
    g_free(o->err);
    g_free(o->src);
    g_free(o->dst);
    g_free(o->created);
    g_free(o->partial);
    g_free(o);
}

static gboolean lc_tick(gpointer ud)
{
    Xfer *x = ud;
    LCOp *o = x->lcop;
    gint64 now;
    if (x->state != XS_RUN) { x->tick = 0; return G_SOURCE_REMOVE; }
    if (o) {
        now = g_get_monotonic_time();
        x->acc += o->done >= x->done ? o->done - x->done
                                   : o->done;
        x->done = o->done;
        if (!x->ema_t) { x->ema_t = now; x->ema_done = o->done; }
        else if (o->done > x->ema_done) {
            double dt = (now - x->ema_t) / 1e6;
            if (dt >= 0.25) {
                double inst = (o->done - x->ema_done) / dt;
                x->ema = x->ema ? x->ema * 0.6 + inst * 0.4
                                : inst;
                x->ema_t = now; x->ema_done = o->done;
            }
        }
        x->total = o->total;
        xfer_update_row(x);
    }
    return G_SOURCE_CONTINUE;
}

/* 取消时清掉半拷贝的目标（lstat，不跟随链接） */
static void lc_rm_rf(const char *path)
{
    struct stat st;
    if (g_lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(path, 0, NULL);
        const gchar *nm;
        if (d) {
            while ((nm = g_dir_read_name(d))) {
                char *full = g_build_filename(path, nm, NULL);
                lc_rm_rf(full);
                g_free(full);
            }
            g_dir_close(d);
        }
        g_rmdir(path);
    } else
        g_remove(path);
}

static void xfer_start_local(Xfer *x)
{
    LCOp *o = g_new0(LCOp, 1);
    GTask *t;
    o->x = x;
    o->src = g_strdup(x->src_path);
    o->dst = path_join(x->dst_dir, x->name);
    o->policy = x->policy;
    x->lcop = o;
    x->canc = g_cancellable_new();
    if (!x->tick) x->tick = g_timeout_add(120, lc_tick, x);
    t = g_task_new(NULL, x->canc, lc_done, o);
    g_task_set_task_data(t, o, NULL);  /* lc_work 的 data 参数 */
    g_task_run_in_thread(t, lc_work);
    g_object_unref(t);
}

/* ---- 冲突对话框 ---- */

typedef struct {
    HbSftpView *v;
    Pane    *sp, *dp;
    GList   *todo;          /* HbFileItem*（待处理） */
    int      policy_all;    /* -1 无，0/1/2 = hb_sftp_conflict */
} ConfCtx;

static void conf_next(ConfCtx *cc);
static void conf_free(ConfCtx *cc)
{
    sel_items_free(cc->todo);
    g_free(cc);
}

static void enq_one(ConfCtx *cc, HbFileItem *i,
                    hb_sftp_conflict pol)
{
    HbSftpView *v = cc->v;
    Xfer *x = g_new0(Xfer, 1);
    x->v = v;
    x->id = ++v->next_xid;
    x->state = XS_RUN;
    x->t0 = g_get_monotonic_time();
    x->sp = cc->sp; x->dp = cc->dp;
    x->name = g_strdup(i->name);
    x->src_path = g_strdup(i->path);
    x->dst_dir = g_strdup(cc->dp->cwd);
    x->src_label = g_strdup(!cc->sp || cc->sp->is_local
                            ? "Local" : cc->sp->hname);
    x->dst_label = g_strdup(cc->dp->is_local ? "Local" : cc->dp->hname);
    x->policy = pol;
    if (cc->sp && !cc->sp->is_local && !cc->dp->is_local)
        x->kind = X_RELAY;
    else if ((!cc->sp || cc->sp->is_local) && !cc->dp->is_local)
        x->kind = X_UP;
    else if (cc->sp && !cc->sp->is_local && cc->dp->is_local)
        x->kind = X_DOWN;
    else
        x->kind = X_LOCAL;

    /* row */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *ico = hb_icon_new(
            x->kind == X_UP ? "up" : x->kind == X_DOWN ? "down"
            : "fwd", 13);
        GtkWidget *nm = gtk_label_new(x->name);
        GtkWidget *dl = gtk_label_new(NULL);
        GtkWidget *track = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        char *d;
        gtk_widget_add_css_class(row, "xfer");
        gtk_widget_add_css_class(nm, "x-name");
        gtk_label_set_ellipsize(GTK_LABEL(nm),
                                PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_size_request(nm, 130, -1);
        gtk_label_set_xalign(GTK_LABEL(nm), 0);
        d = g_strdup_printf("%s → %s", x->src_label, x->dst_label);
        gtk_label_set_label(GTK_LABEL(dl), d);
        g_free(d);
        gtk_widget_add_css_class(dl, "x-dir");
        gtk_widget_set_size_request(dl, 130, -1);
        gtk_label_set_xalign(GTK_LABEL(dl), 0);
        gtk_widget_add_css_class(track, "x-track");
        gtk_widget_set_hexpand(track, TRUE);
        x->fill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(x->fill, "x-fill");
        gtk_box_append(GTK_BOX(track), x->fill);
        x->pct = gtk_label_new("0%");
        gtk_widget_add_css_class(x->pct, "x-pct");
        gtk_widget_set_size_request(x->pct, 150, -1);
        gtk_label_set_xalign(GTK_LABEL(x->pct), 0);
        x->xbtn = gtk_button_new_with_label("×");
        gtk_widget_add_css_class(x->xbtn, "hb-btn");
        gtk_widget_add_css_class(x->xbtn, "sm");
        gtk_box_append(GTK_BOX(row), ico);
        gtk_box_append(GTK_BOX(row), nm);
        gtk_box_append(GTK_BOX(row), dl);
        gtk_box_append(GTK_BOX(row), track);
        gtk_box_append(GTK_BOX(row), x->pct);
        gtk_box_append(GTK_BOX(row), x->xbtn);
        x->row = row;
        x->dir_lbl = dl;
        g_object_set_data(G_OBJECT(x->xbtn), "xfer", x);
        g_signal_connect(x->xbtn, "clicked",
                         G_CALLBACK(on_xfer_btn), NULL);
        gtk_box_append(GTK_BOX(v->xlist), row);
    }
    v->xers = g_list_append(v->xers, x);
    xhead_sync(v);
    xfer_update_row(x);

    /* 启动 */
    switch (x->kind) {
    case X_UP: {
        char *t = path_join(x->dst_dir, x->name);
        x->req = hb_sftp_upload(x->dp->sess, x->src_path, t,
                                x->policy);
        g_free(t);
        x->sess = x->dp->sess;
        break; }
    case X_DOWN: {
        char *t = path_join(x->dst_dir, x->name);
        x->req = hb_sftp_download(x->sp->sess, x->src_path, t,
                                  x->policy);
        g_free(t);
        x->sess = x->sp->sess;
        break; }
    case X_RELAY:
        x->tmp_dir = g_strdup_printf("%s/harbor/relay-%d-XXXXXX",
            g_get_user_cache_dir(), x->id);
        {
            char *bd = g_strdup_printf("%s/harbor",
                                       g_get_user_cache_dir());
            g_mkdir_with_parents(bd, 0700);
            g_free(bd);
        }
        {
            char *t = g_mkdtemp(x->tmp_dir);
            if (!t) { xfer_finish(x, XS_FAIL, "无法创建临时目录"); return; }
        }
        x->tmp_path = path_join(x->tmp_dir, x->name);
        x->phase = 0;
        x->req = hb_sftp_download(x->sp->sess, x->src_path,
                                  x->tmp_path, HB_SFTP_OVERWRITE);
        x->sess = x->sp->sess;
        break;
    case X_LOCAL:
        xfer_start_local(x);
        break;
    }
}

/* conflict 对话框 */
typedef struct {
    ConfCtx *cc;
    HbFileItem *item;
    AdwDialog *dlg;
    GtkWidget *apply_all;
    int       pol;          /* 已选策略；-1 取消 */
    gboolean  all;
    gboolean  handled;      /* 已走 response */
    gboolean  pending;      /* decide idle 已挂 */
} ConfDlg;

/* 本 libadwaita 先 "closed" 后 "response"（按钮激活时两个信号同一
 * 次派发里先后发出），任何一边处理都不可靠 → 决策放到 idle：
 * 回到主循环时本次所有信号已发完，状态齐全。 */
static gboolean conf_decide(gpointer ud)
{
    ConfDlg *cd = ud;
    if (!cd->handled || cd->pol < 0) {
        /* Esc/×/取消传输：丢弃当前项与整个待处理批次 */
        g_object_unref(cd->item);
        conf_free(cd->cc);
    } else {
        if (cd->all)
            cd->cc->policy_all = cd->pol;
        enq_one(cd->cc, cd->item, (hb_sftp_conflict)cd->pol);
        conf_next(cd->cc);
    }
    g_free(cd);
    return G_SOURCE_REMOVE;
}

static void conf_dlg_closed(AdwDialog *d, gpointer ud)
{
    ConfDlg *cd = g_object_get_data(G_OBJECT(d), "cd");
    (void)ud;
    if (!cd->pending) {
        cd->pending = TRUE;
        g_idle_add(conf_decide, cd);
    }
}

static void on_conf_response(AdwAlertDialog *d, char *resp,
                             gpointer ud)
{
    ConfDlg *cd = g_object_get_data(G_OBJECT(d), "cd");
    (void)ud;
    if (!resp) return;
    if (!strcmp(resp, "ov")) cd->pol = HB_SFTP_OVERWRITE;
    else if (!strcmp(resp, "skip")) cd->pol = HB_SFTP_SKIP;
    else if (!strcmp(resp, "rename")) cd->pol = HB_SFTP_RENAME;
    else cd->pol = -1;
    cd->all = gtk_check_button_get_active(
        GTK_CHECK_BUTTON(cd->apply_all));
    cd->handled = TRUE;
}

static void conf_ask(ConfCtx *cc, HbFileItem *it)
{
    ConfDlg *cd = g_new0(ConfDlg, 1);
    AdwDialog *d;
    GtkWidget *box;
    char *t;
    cd->cc = cc;
    cd->item = it;
    d = adw_alert_dialog_new("目标已存在", NULL);
    if (it->is_dir)
        t = g_strdup_printf(
            "「%s」已存在 — 将合并目录，按所选方式处理同名文件",
            it->name);
    else
        t = g_strdup_printf("「%s」在对侧目录已存在。选择处理方式：",
                            it->name);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d), t);
    g_free(t);
    cd->dlg = d;
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "cancel",
                                  "取消传输");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "skip", "跳过");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "rename",
                                  "重命名");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ov", "覆盖");
    adw_alert_dialog_set_response_appearance(
        ADW_ALERT_DIALOG(d), "ov", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "ov");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
    cd->apply_all = gtk_check_button_new_with_label(
        "对本次其余项目都适用");
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(box), cd->apply_all);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), box);
    g_object_set_data(G_OBJECT(d), "cd", cd);
    g_signal_connect(d, "response",
                     G_CALLBACK(on_conf_response), NULL);
    g_signal_connect(d, "closed",
                     G_CALLBACK(conf_dlg_closed), NULL);
    adw_dialog_present(d, GTK_WIDGET(cc->v));
}

/* 逐 stat 目标 → 冲突→问，否则入队 */
typedef struct { ConfCtx *cc; HbFileItem *it; } StatCtx;

static void stat_cb(const hb_fs_result *r, gpointer ud)
{
    StatCtx *sc = ud;
    ConfCtx *cc = sc->cc;
    if (r->ok && r->n > 0) {
        if (cc->policy_all >= 0) {
            enq_one(cc, sc->it, (hb_sftp_conflict)cc->policy_all);
            conf_next(cc);
        } else
            conf_ask(cc, sc->it);
    } else {
        enq_one(cc, sc->it, HB_SFTP_OVERWRITE /*无冲突*/);
        conf_next(cc);
    }
    g_free(sc);
}

static void conf_next(ConfCtx *cc)
{
    HbFileItem *it;
    char *dst;
    StatCtx *sc;
    if (!cc->todo) { conf_free(cc); return; }
    it = cc->todo->data;
    cc->todo = g_list_delete_link(cc->todo, cc->todo);
    dst = path_join(cc->dp->cwd, it->name);
    sc = g_new0(StatCtx, 1);
    sc->cc = cc;
    sc->it = it;
    hb_fs_stat(cc->dp->fs, dst, stat_cb, sc, NULL);
    g_free(dst);
}

/* 入队一批选中项到对侧 pane */
static void xfer_enqueue(HbSftpView *v, Pane *sp, GList *items,
                         Pane *dp)
{
    ConfCtx *cc;
    if (!items) return;
    if (!dp || dp->state != PS_BROWSE || !dp->fs) {
        toast(v, "请先在对侧栏选择主机");
        sel_items_free(items);
        return;
    }
    cc = g_new0(ConfCtx, 1);
    cc->v = v; cc->sp = sp; cc->dp = dp;
    cc->todo = items;             /* owned items */
    cc->policy_all = -1;
    conf_next(cc);
}

static void on_xfer_btn(GtkButton *b, gpointer ud)
{
    Xfer *x = g_object_get_data(G_OBJECT(b), "xfer");
    HbSftpView *v;
    (void)ud;
    if (!x) return;
    v = x->v;
    if (x->state == XS_RUN) {
        /* cancel */
        if (x->req && x->sess)
            hb_sftp_cancel(x->sess, x->req);
        if (x->canc) g_cancellable_cancel(x->canc);
        if (x->kind != X_LOCAL && !(x->req && x->sess))
            xfer_finish(x, XS_CANCEL, NULL);
        /* sftp cancel → DONE(CANCELLED) 事件到 dispatch 时 finish */
        if (x->kind == X_LOCAL)
            xfer_finish(x, XS_CANCEL, NULL);
        return;
    }
    if (x->state == XS_FAIL) {
        /* 重试：同一 job 重新入队（同 policy） */
        ConfCtx *cc = g_new0(ConfCtx, 1);
        HbFileItem *it = file_item_new(&(hb_sftp_entry){
            .name = x->name }, NULL);
        g_free(it->path);
        it->path = g_strdup(x->src_path);
        cc->v = v;
        cc->sp = x->sp; cc->dp = x->dp;
        cc->todo = g_list_append(NULL, it);
        cc->policy_all = x->policy;
        /* 移除旧行 */
        v->xers = g_list_remove(v->xers, x);
        xfer_remove_row(x);
        xfer_free(x);
        xhead_sync(v);
        conf_next(cc);
        return;
    }
}

static void on_clear_done(GtkButton *b, gpointer ud)
{
    HbSftpView *v = ud;
    GList *l = v->xers, *nx;
    (void)b;
    while (l) {
        Xfer *x = l->data;
        nx = l->next;
        if (x->state == XS_DONE || x->state == XS_CANCEL) {
            v->xers = g_list_remove(v->xers, x);
            xfer_remove_row(x);
            xfer_free(x);
        }
        l = nx;
    }
    xhead_sync(v);
}

/* ================= 文件操作（rename/mkdir/chmod/open/delete） ================= */

static void op_cb_refresh(const hb_fs_result *r, gpointer ud)
{
    Pane *p = ud;
    if (!r->ok) {
        char *t = g_strdup_printf("操作失败: %s",
            r->message ? r->message : "");
        toast(p->v, t); g_free(t);
    }
    pane_refresh(p);
}

static void act_refresh(Pane *p) { pane_refresh(p); }

static void act_select_all(Pane *p)
{
    gtk_selection_model_select_all(GTK_SELECTION_MODEL(p->sel));
}

static void act_toggle_hidden(Pane *p)
{
    p->show_hidden = !p->show_hidden;
    filter_changed(p);
}

typedef struct { Pane *p; } MkCtx;
static void on_mkdir_name(const char *text, gpointer ud)
{
    Pane *p = ud;
    char *full;
    if (!text || !*text) return;
    full = path_join(p->cwd, text);
    hb_fs_mkdir(p->fs, full, 0755, op_cb_refresh, p, NULL);
    g_free(full);
}
static void act_new_folder(Pane *p)
{
    hb_dialog_prompt(GTK_WIDGET(p->v), "新建文件夹", "名称",
                     "new-folder", on_mkdir_name, p);
}

/* 删除（undo toast 9s）：先把项从 model 藏起来；
 * toast 超时/关闭 → 真删（递归）；撤销 → 重新插回。 */
static void pend_exec(PendDel *pd)
{
    GList *l;
    Pane *p = pd->p;
    p->pend_dels = g_list_remove(p->pend_dels, pd);
    if (!pd->undone && p->gen == pd->gen) {
        for (l = pd->paths; l; l = l->next)
            hb_fs_remove(p->fs, l->data, TRUE,
                         op_cb_refresh, p, NULL);
    }
    g_list_free_full(pd->paths, g_free);
    g_ptr_array_free(pd->items, TRUE);
    g_free(pd);
}

static void pend_undo(gpointer ud)
{
    PendDel *pd = ud;
    Pane *p = pd->p;
    guint k;
    pd->undone = TRUE;
    if (p->gen == pd->gen) {
        for (k = 0; k < pd->items->len; k++)
            g_list_store_append(p->model, pd->items->pdata[k]);
    }
    /* 撤销后不再真删 */
    g_list_free_full(pd->paths, g_free);
    pd->paths = NULL;
}

static void delete_items(Pane *p, GList *items)
{
    PendDel *pd = g_new0(PendDel, 1);
    GList *l;
    guint n = 0;
    pd->p = p;
    pd->gen = p->gen;
    pd->items = g_ptr_array_new_with_free_func(g_object_unref);
    for (l = items; l; l = l->next) {
        HbFileItem *i = l->data;
        guint k, m;
        n++;
        pd->paths = g_list_append(pd->paths, g_strdup(i->path));
        g_ptr_array_add(pd->items, g_object_ref(i));
        m = g_list_model_get_n_items(G_LIST_MODEL(p->model));
        for (k = 0; k < m; k++) {
            gpointer it2 = g_list_model_get_item(
                G_LIST_MODEL(p->model), k);
            gboolean hit = it2 == i;
            g_object_unref(it2);
            if (hit) { g_list_store_remove(p->model, k); break; }
        }
    }
    p->pend_dels = g_list_append(p->pend_dels, pd);
    {
        char *t = g_strdup_printf("已删除 %u 个项目", n);
        hb_toast_undo_destroy(GTK_WIDGET(p->v), t,
            pend_undo, (GDestroyNotify)pend_exec, pd);
        g_free(t);
    }
    sel_items_free(items);
}

static void act_delete(Pane *p)
{
    GList *items = sel_items(p);
    GList *l;
    guint ndir = 0, n = 0;
    if (!items) return;
    for (l = items; l; l = l->next) {
        HbFileItem *i = l->data;
        n++;
        if (i->is_dir) ndir++;
    }
    if (ndir) {
        char *t = g_strdup_printf(
            "将递归删除 %u 个项目（含 %u 个目录）。删除后 9 秒内可撤销。",
            n, ndir);
        DelConf *dc = g_new0(DelConf, 1);
        dc->p = p;
        dc->items = items;
        hb_dialog_confirm(GTK_WIDGET(p->v), "删除项目", t, "删除",
                          TRUE, on_deldir_conf, dc);
        g_free(t);
        return;
    }
    delete_items(p, items);
}

/* ================= Actions 菜单 ================= */

static void menu_cb(GSimpleAction *a, GVariant *p_, gpointer ud);

static const char *menu_names[] = {
    "open", "copy", "rename", "delete", "mkdir",
    "chmod", "refresh", "selectall", "hidden"
};

static GMenu *build_pane_menu(Pane *p)
{
    GMenu *m = g_menu_new();
    g_menu_append(m, "打开", "pane.open");
    g_menu_append(m, "复制到对侧栏", "pane.copy");
    g_menu_append(m, "重命名…", "pane.rename");
    g_menu_append(m, "删除", "pane.delete");
    g_menu_append(m, "新建文件夹…", "pane.mkdir");
    g_menu_append(m, "修改权限…", "pane.chmod");
    g_menu_append(m, "刷新", "pane.refresh");
    g_menu_append(m, "全选", "pane.selectall");
    g_menu_append(m, "显示隐藏文件", "pane.hidden");
    (void)p;
    return m;
}

static void menu_cb(GSimpleAction *a, GVariant *p_, gpointer ud)
{
    Pane *p = ud;
    const char *n = g_action_get_name(G_ACTION(a));
    GList *items;
    (void)p_;
    if (!strcmp(n, "refresh")) { act_refresh(p); return; }
    if (!strcmp(n, "selectall")) { act_select_all(p); return; }
    if (!strcmp(n, "hidden")) { act_toggle_hidden(p); return; }
    items = sel_items(p);
    if (!strcmp(n, "copy")) { xfer_enqueue(p->v, p, items,
        &p->v->pane[p->side ^ 1]); return; }
    if (!strcmp(n, "delete")) { sel_items_free(items);
        act_delete(p); return; }
    if (!strcmp(n, "mkdir")) { sel_items_free(items);
        act_new_folder(p); return; }
    if (!strcmp(n, "rename")) { act_rename(p, items); return; }
    if (!strcmp(n, "chmod")) { act_chmod(p, items); return; }
    if (!strcmp(n, "open")) { act_open(p, items); return; }
    sel_items_free(items);
}


/* ================= 行交互：右键菜单 / 拖放 / 激活 / 键盘 ================= */

static void pop_menu(Pane *p, GtkWidget *w, double x, double y)
{
    GtkWidget *pop = gtk_popover_menu_new_from_model(
        G_MENU_MODEL(build_pane_menu(p)));
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_widget_set_parent(pop, w);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent),
                     NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

/* colview 坐标 → 命中的行（向上找带 "li" 的单元格 widget） */
static GtkListItem *cell_at(Pane *p, double x, double y)
{
    GtkWidget *t = gtk_widget_pick(p->colview, x, y,
                                   GTK_PICK_DEFAULT);
    while (t && t != p->colview &&
           !g_object_get_data(G_OBJECT(t), "li"))
        t = gtk_widget_get_parent(t);
    if (!t || t == p->colview)
        return NULL;
    return g_object_get_data(G_OBJECT(t), "li");
}

static void on_row_menu(GtkGestureClick *g, int n, double x,
                        double y, gpointer ud)
{
    Pane *p = g_object_get_data(G_OBJECT(g), "pane");
    GtkListItem *li;
    guint pos;
    (void)n; (void)ud;
    li = cell_at(p, x, y);
    if (!li) return;
    pos = gtk_list_item_get_position(li);
    if (pos == GTK_INVALID_LIST_POSITION) return;
    /* 不 claim：claim 会握住指针 grab，popover 拿不到 grab 就弹不出来 */
    /* 右键点击的项若未选中则单选它 */
    if (!gtk_selection_model_is_selected(
            GTK_SELECTION_MODEL(p->sel), pos)) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(p->sel));
        gtk_selection_model_select_item(
            GTK_SELECTION_MODEL(p->sel), pos, TRUE);
    }
    pop_menu(p, p->colview, x, y);
}

static GdkContentProvider *drag_prepare(GtkDragSource *ds,
                                        double x, double y,
                                        gpointer ud)
{
    Pane *p = g_object_get_data(G_OBJECT(ds), "pane");
    GtkListItem *li = cell_at(p, x, y);
    guint pos = li ? gtk_list_item_get_position(li)
                   : GTK_INVALID_LIST_POSITION;
    GList *items;
    GString *s;
    GList *l;
    GdkContentProvider *prov;
    (void)x; (void)y; (void)ud;
    if (pos == GTK_INVALID_LIST_POSITION)
        return NULL;                /* 空白处不发起拖拽 */
    if (!gtk_selection_model_is_selected(
            GTK_SELECTION_MODEL(p->sel), pos)) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(p->sel));
        gtk_selection_model_select_item(
            GTK_SELECTION_MODEL(p->sel), pos, TRUE);
    }
    items = sel_items(p);
    s = g_string_new("hbsftp");
    g_string_append_printf(s, "|%d", p->side);
    for (l = items; l; l = l->next) {
        HbFileItem *i = l->data;
        g_string_append_c(s, '\n');
        g_string_append(s, i->name);
    }
    prov = gdk_content_provider_new_for_bytes("text/plain",
        g_bytes_new(s->str, s->len));
    /* 本地 pane 同时提供 GdkFileList（可拖给外部文件管理器） */
    if (p->is_local) {
        GSList *files = NULL;
        GdkFileList *fl;
        GdkContentProvider *fp, *arr[2], *uni;
        for (l = items; l; l = l->next) {
            HbFileItem *i = l->data;
            files = g_slist_prepend(files, g_file_new_for_path(i->path));
        }
        fl = gdk_file_list_new_from_list(g_slist_reverse(files));
        fp = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl);
        arr[0] = prov; arr[1] = fp;
        uni = gdk_content_provider_new_union(arr, 2);
        g_object_unref(fp);
        g_boxed_free(GDK_TYPE_FILE_LIST, fl);
        sel_items_free(items);
        g_string_free(s, TRUE);
        return uni;
    }
    sel_items_free(items);
    g_string_free(s, TRUE);
    return prov;
}

static gboolean on_drop(GtkDropTarget *dt, const GValue *val,
                        double x, double y, gpointer ud)
{
    Pane *dp = g_object_get_data(G_OBJECT(dt), "pane");
    HbSftpView *v = dp->v;
    (void)x; (void)y; (void)ud;
    if (dp->state != PS_BROWSE || !dp->fs) {
        toast(v, "请先在对侧栏选择主机");
        return FALSE;
    }
    if (G_VALUE_HOLDS(val, GDK_TYPE_FILE_LIST)) {
        /* 外部文件管理器拖入 → 传输到本 pane cwd */
        GdkFileList *fl = g_value_get_boxed(val);
        GSList *files = gdk_file_list_get_files(fl);
        GList *items = NULL;
        /* 文件来自外部文件管理器：源按 local 处理（sp=NULL 也行） */
        Pane *sp = v->pane[0].is_local ? &v->pane[0]
                 : v->pane[1].is_local ? &v->pane[1] : NULL;
        for (GSList *sl = files; sl; sl = sl->next) {
            GFile *gf = sl->data;
            char *path = g_file_get_path(gf), *base;
            HbFileItem *it;
            if (!path) continue;
            base = g_path_get_basename(path);
            it = file_item_new(&(hb_sftp_entry){
                .name = base }, NULL);
            g_free(it->name); it->name = base;
            g_free(it->path); it->path = path;
            items = g_list_append(items, it);
        }
        xfer_enqueue(v, sp, items, dp);
        return TRUE;
    }
    if (G_VALUE_HOLDS(val, G_TYPE_STRING)) {
        const char *s = g_value_get_string(val);
        Pane *sp;
        GList *items = NULL;
        char **lines, **l;
        int sside;
        if (!s || !g_str_has_prefix(s, "hbsftp|")) return FALSE;
        sside = atoi(s + 7);
        if (sside < 0 || sside > 1) return FALSE;
        sp = &v->pane[sside];
        lines = g_strsplit(s, "\n", -1);
        for (l = lines + 1; *l; l++) {
            HbFileItem *it;
            char *path;
            if (!**l) continue;
            path = path_join(sp->cwd, *l);
            it = file_item_new(&(hb_sftp_entry){ .name = *l }, NULL);
            g_free(it->name); it->name = g_strdup(*l);
            g_free(it->path); it->path = path;
            items = g_list_append(items, it);
        }
        g_strfreev(lines);
        xfer_enqueue(v, sp, items, dp);
        return TRUE;
    }
    return FALSE;
}

static void on_row_activate(GtkColumnView *cv, guint pos, gpointer ud)
{
    Pane *p = ud;
    HbFileItem *i = g_list_model_get_item(G_LIST_MODEL(p->sel), pos);
    (void)cv;
    if (!i) return;
    if (i->is_dir) {
        pane_browse_to(p, i->path, TRUE);
    } else {
        /* owned item 入队（activate 项只是借用） */
        HbFileItem *cp = file_item_new(&(hb_sftp_entry){
            .name = i->name }, NULL);
        g_free(cp->path); cp->path = g_strdup(i->path);
        xfer_enqueue(p->v, p, g_list_append(NULL, cp),
                     &p->v->pane[p->side ^ 1]);
    }
    g_object_unref(i);
}

static gboolean on_pane_key(GtkEventControllerKey *kc, guint kv,
                            guint code, GdkModifierType st,
                            gpointer ud)
{
    Pane *p = g_object_get_data(G_OBJECT(kc), "pane");
    GtkWidget *foc;
    (void)code; (void)ud;
    /* 快捷键挂在 pane 容器上（bubble 阶段）：列视图单元格不可聚焦，
     * 焦点常落在行内部或窗口上，控制器绑 colview 会收不到键。
     * 过滤输入框持有焦点时只放行 F5，其余键留给文本编辑。 */
    foc = gtk_root_get_focus(gtk_widget_get_root(p->colview));
    if (foc && GTK_IS_EDITABLE(foc))
        return kv == GDK_KEY_F5 ? (pane_refresh(p), TRUE) : FALSE;
    if (kv == GDK_KEY_F5) { pane_refresh(p); return TRUE; }
    if (kv == GDK_KEY_F2) { GList *l = sel_items(p);
        act_rename(p, l); return TRUE; }
    if (kv == GDK_KEY_Delete) { act_delete(p); return TRUE; }
    if (kv == GDK_KEY_BackSpace ||
        (kv == GDK_KEY_Up && (st & GDK_ALT_MASK))) {
        on_up(NULL, p); return TRUE;
    }
    if (kv == GDK_KEY_a && (st & GDK_CONTROL_MASK)) {
        act_select_all(p); return TRUE;
    }
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
        /* activate 已由 ColumnView 处理 Enter —— 防重复 */
        return FALSE;
    }
    return FALSE;
}

/* ================= 重命名 / chmod / 打开 ================= */

static void on_rename_name(const char *text, gpointer ud)
{
    RenCtx *rc = ud;
    char *dir, *to;
    if (text && *text) {
        dir = parent_dir(rc->old);
        to = path_join(dir, text);
        hb_fs_rename(rc->p->fs, rc->old, to, FALSE,
                     op_cb_refresh, rc->p, NULL);
        g_free(dir); g_free(to);
    }
    g_free(rc->old);
    g_free(rc);
}

/* ---- chmod 对话框：3×3 rwx 矩阵 + 八进制输入，双向同步 ---- */

typedef struct {
    Pane     *p;
    GList    *paths;        /* char* */
    GtkWidget *cb[9];       /* r w x × u g o */
    GtkWidget *oct;
    gboolean  updating;
} ChmodCtx;

static unsigned chmod_from_matrix(ChmodCtx *c)
{
    unsigned m = 0;
    int row, col;
    /* cb[row*3+col]：行 r/w/x，列 属主/组/其他；
     * 权限位序是 属主rwx,组rwx,其他rwx —— 列优先 */
    for (col = 0; col < 3; col++)
        for (row = 0; row < 3; row++)
            if (gtk_check_button_get_active(
                    GTK_CHECK_BUTTON(c->cb[row * 3 + col])))
                m |= 1u << (8 - (col * 3 + row));
    return m;
}

static void chmod_to_matrix(ChmodCtx *c, unsigned m)
{
    int row, col;
    c->updating = TRUE;
    for (col = 0; col < 3; col++)
        for (row = 0; row < 3; row++)
            gtk_check_button_set_active(
                GTK_CHECK_BUTTON(c->cb[row * 3 + col]),
                !!(m & (1u << (8 - (col * 3 + row)))));
    c->updating = FALSE;
}

static void chmod_sync_oct(ChmodCtx *c)
{
    char t[8];
    g_snprintf(t, sizeof t, "%o", chmod_from_matrix(c));
    c->updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(c->oct), t);
    c->updating = FALSE;
}

static void on_chmod_cb_toggled(GtkCheckButton *b, gpointer ud)
{
    ChmodCtx *c = ud;
    (void)b;
    if (!c->updating) chmod_sync_oct(c);
}
static void on_chmod_oct(GtkEditable *e, gpointer ud)
{
    ChmodCtx *c = ud;
    const char *t = gtk_editable_get_text(e);
    char *end = NULL;
    unsigned long m;
    (void)ud;
    if (c->updating) return;
    m = strtoul(t, &end, 8);
    if (end && *end == 0 && m <= 0777)
        chmod_to_matrix(c, (unsigned)m);
}

static void chmod_apply(ChmodCtx *c)
{
    unsigned m = chmod_from_matrix(c);
    GList *l;
    for (l = c->paths; l; l = l->next)
        hb_fs_chmod(c->p->fs, l->data, m, op_cb_refresh, c->p, NULL);
    g_list_free_full(c->paths, g_free);
    g_free(c);
}
static void chmod_cancel(ChmodCtx *c)
{
    g_list_free_full(c->paths, g_free);
    g_free(c);
}
static void on_chmod_apply(GtkButton *b, gpointer ud)
{
    ChmodCtx *c = g_object_get_data(G_OBJECT(b), "cc");
    AdwDialog *d = g_object_get_data(G_OBJECT(b), "dlg");
    (void)ud;
    chmod_apply(c);
    adw_dialog_close(d);
}
static void on_chmod_cancel_btn(GtkButton *b, gpointer ud)
{
    ChmodCtx *c = g_object_get_data(G_OBJECT(b), "cc");
    AdwDialog *d = g_object_get_data(G_OBJECT(b), "dlg");
    (void)ud;
    chmod_cancel(c);
    adw_dialog_close(d);
}

static void act_chmod(Pane *p, GList *items)
{
    ChmodCtx *c;
    GList *l;
    HbFileItem *first;
    AdwDialog *dlg;
    GtkWidget *box, *grid, *foot, *btn;
    int k;
    static const char *heads[] = { "所有者", "组", "其他" };
    if (!items) return;
    c = g_new0(ChmodCtx, 1);
    c->p = p;
    first = items->data;
    for (l = items; l; l = l->next) {
        HbFileItem *i = l->data;
        c->paths = g_list_append(c->paths, g_strdup(i->path));
    }
    sel_items_free(items);

    dlg = adw_dialog_new();
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    {
        GtkWidget *t = gtk_label_new("修改权限");
        gtk_widget_add_css_class(t, "h-name");
        gtk_box_append(GTK_BOX(box), t);
    }
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    for (k = 0; k < 3; k++) {
        GtkWidget *h = gtk_label_new(heads[k]);
        gtk_widget_add_css_class(h, "hb-dim");
        gtk_grid_attach(GTK_GRID(grid), h, k + 1, 0, 1, 1);
    }
    {
        const char *cols[] = { "r", "w", "x" };
        int row;
        for (row = 0; row < 3; row++) {
            int col;
            GtkWidget *h = gtk_label_new(cols[row]);
            gtk_widget_add_css_class(h, "hb-dim");
            gtk_grid_attach(GTK_GRID(grid), h, 0, row + 1, 1, 1);
            for (col = 0; col < 3; col++) {
                c->cb[row * 3 + col] = gtk_check_button_new();
                g_signal_connect(c->cb[row * 3 + col], "toggled",
                    G_CALLBACK(on_chmod_cb_toggled), c);
                gtk_grid_attach(GTK_GRID(grid), c->cb[row * 3 + col],
                                col + 1, row + 1, 1, 1);
            }
        }
    }
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), grid);
    c->oct = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(c->oct), "八进制，如 755");
    gtk_editable_set_width_chars(GTK_EDITABLE(c->oct), 6);
    gtk_widget_set_halign(c->oct, GTK_ALIGN_CENTER);
    g_signal_connect(c->oct, "changed",
                     G_CALLBACK(on_chmod_oct), c);
    gtk_box_append(GTK_BOX(box), c->oct);
    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(foot, GTK_ALIGN_END);
    btn = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(btn, "hb-btn");
    g_object_set_data(G_OBJECT(btn), "cc", c);
    g_object_set_data(G_OBJECT(btn), "dlg", dlg);
    g_signal_connect(btn, "clicked",
                     G_CALLBACK(on_chmod_cancel_btn), NULL);
    gtk_box_append(GTK_BOX(foot), btn);
    btn = gtk_button_new_with_label("应用");
    gtk_widget_add_css_class(btn, "hb-btn");
    gtk_widget_add_css_class(btn, "primary");
    g_object_set_data(G_OBJECT(btn), "cc", c);
    g_object_set_data(G_OBJECT(btn), "dlg", dlg);
    g_signal_connect(btn, "clicked",
                     G_CALLBACK(on_chmod_apply), NULL);
    gtk_box_append(GTK_BOX(foot), btn);
    gtk_box_append(GTK_BOX(box), foot);
    adw_dialog_set_child(dlg, box);
    gtk_widget_set_size_request(GTK_WIDGET(dlg), 320, -1);
    chmod_to_matrix(c, first->mode & 0777);
    chmod_sync_oct(c);
    adw_dialog_present(dlg, GTK_WIDGET(p->v));
}

/* ---- 打开（本地直接起 launcher；远端下载缓存 → launcher →
 *        GFileMonitor 修改 → 提示回传上传） ---- */

static void open_ctx_free(OpenCtx *oc)
{
    if (oc->mon) g_object_unref(oc->mon);
    g_free(oc->name);
    g_free(oc->remote_path);
    g_free(oc->local_path);
    g_free(oc->dir);
    g_free(oc);
}

typedef struct {
    Pane *p;                        /* 嵌入视图，生命周期 ≥ toast */
    char *local_path;
    char *remote_path;
} UpBack;

static void upback_free(UpBack *u)
{
    g_free(u->local_path);
    g_free(u->remote_path);
    g_free(u);
}

static void upload_back_cb(gpointer ud)
{
    /* swapped 连接：第一个实参即 user_data */
    UpBack *u = ud;
    if (u->p->sess)
        hb_sftp_upload(u->p->sess, u->local_path,
                       u->remote_path, HB_SFTP_OVERWRITE);
}

static void on_file_changed(GFileMonitor *m, GFile *f, GFile *o,
                            GFileMonitorEvent ev, gpointer ud)
{
    OpenCtx *oc = ud;
    HbWindow *w = view_win(oc->p->v);
    char *t;
    UpBack *u;
    (void)m; (void)f; (void)o;
    if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        ev != G_FILE_MONITOR_EVENT_CREATED)
        return;
    t = g_strdup_printf("已修改 %s · 上传回远端", oc->name);
    /* toast 拥有 UpBack（dismiss 时释放），不引用 OpenCtx：
     * pane teardown 先于 toast 消失也不会 UAF */
    u = g_new0(UpBack, 1);
    u->p = oc->p;
    u->local_path = g_strdup(oc->local_path);
    u->remote_path = g_strdup(oc->remote_path);
    if (w)
        hb_window_toast_action2(w, t, "上传", 9,
            (GCallback)upload_back_cb,
            (GDestroyNotify)upback_free, u);
    else
        upback_free(u);
    g_free(t);
}

/* remote open 下载完成 → 启动 launcher + monitor */
static gboolean open_done(Pane *p, const hb_sftp_result *r)
{
    OpenCtx *oc;
    if (!p->open_reqs) return FALSE;
    oc = g_hash_table_lookup(p->open_reqs,
                             GUINT_TO_POINTER(r->req));
    if (!oc) return FALSE;
    if (r->kind == HB_SFTP_R_PROGRESS) return TRUE;
    g_hash_table_steal(p->open_reqs, GUINT_TO_POINTER(r->req));
    if (r->status != HB_SFTP_OK) {
        char *t = g_strdup_printf("下载失败: %s",
            r->message ? r->message : "");
        toast(p->v, t); g_free(t);
        open_ctx_free(oc);
        return TRUE;
    }
    {
        GFile *f = g_file_new_for_path(oc->local_path);
        GtkFileLauncher *l = gtk_file_launcher_new(f);
        gtk_file_launcher_launch(l, GTK_WINDOW(view_win(p->v)),
                                 NULL, NULL, NULL);
        g_object_unref(l);
        oc->mon = g_file_monitor_file(f, G_FILE_MONITOR_NONE,
                                      NULL, NULL);
        if (oc->mon)
            g_signal_connect(oc->mon, "changed",
                             G_CALLBACK(on_file_changed), oc);
        g_object_unref(f);
    }
    /* oc 存活至 pane teardown 统一释放 */
    p->open_ctxs = g_list_append(p->open_ctxs, oc);
    return TRUE;
}

static void act_open(Pane *p, GList *items)
{
    HbFileItem *i;
    if (!items) { sel_items_free(items); return; }
    i = items->data;
    if (p->is_local) {
        GFile *f = g_file_new_for_path(i->path);
        GtkFileLauncher *l = gtk_file_launcher_new(f);
        gtk_file_launcher_launch(l, GTK_WINDOW(view_win(p->v)),
                                 NULL, NULL, NULL);
        g_object_unref(l);
        g_object_unref(f);
    } else {
        /* 下载到 $XDG_CACHE_HOME/harbor/open/<rand>/ */
        OpenCtx *oc = g_new0(OpenCtx, 1);
        char *d;
        d = g_strdup_printf("%s/harbor/open", g_get_user_cache_dir());
        g_mkdir_with_parents(d, 0700);
        g_free(d);
        oc->p = p;
        oc->name = g_strdup(i->name);
        oc->remote_path = g_strdup(i->path);
        oc->dir = g_strdup_printf("%s/harbor/open/%u-XXXXXX",
            g_get_user_cache_dir(), g_random_int());
        {
            char *t = g_mkdtemp(oc->dir);
            if (!t) {
                toast(p->v, "无法创建缓存目录");
                open_ctx_free(oc);
                sel_items_free(items);
                return;
            }
            g_chmod(oc->dir, 0700);
        }
        oc->local_path = path_join(oc->dir, i->name);
        if (!p->open_reqs)
            p->open_reqs = g_hash_table_new_full(g_direct_hash,
                g_direct_equal, NULL,
                (GDestroyNotify)open_ctx_free);
        oc->req = hb_sftp_download(p->sess, oc->remote_path,
                                   oc->local_path,
                                   HB_SFTP_OVERWRITE);
        g_hash_table_insert(p->open_reqs,
                            GUINT_TO_POINTER(oc->req), oc);
    }
    sel_items_free(items);
}

static void act_rename(Pane *p, GList *items)
{
    HbFileItem *i;
    RenCtx *rc;
    if (!items) return;
    i = items->data;
    rc = g_new0(RenCtx, 1);
    rc->p = p;
    rc->old = g_strdup(i->path);
    hb_dialog_prompt(GTK_WIDGET(p->v), "重命名", "新文件名",
                     i->name, on_rename_name, rc);
    sel_items_free(items);
}

/* ---- 目录删除确认 ---- */

static void on_deldir_conf(gboolean ok, gpointer ud)
{
    DelConf *dc = ud;
    if (ok)
        delete_items(dc->p, dc->items);  /* 吃掉 items 所有权 */
    else
        sel_items_free(dc->items);
    g_free(dc);
}

/* ================= Pane 构建 ================= */


static GtkWidget *build_browser(Pane *p)
{
    GtkWidget *box, *head, *nav;
    GtkListItemFactory *f;
    GtkColumnViewColumn *col;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "sftp-pane");
    p->browse_box = box;

    /* head */
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(head, "sftp-head");
    p->head_av = hb_os_avatar_new("generic", 30);
    gtk_box_append(GTK_BOX(head), p->head_av);
    {
        GtkWidget *t = gtk_label_new("");
        gtk_widget_add_css_class(t, "h-name");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        g_object_set_data(G_OBJECT(box), "title", t);
        gtk_box_append(GTK_BOX(head), t);
    }
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(head), sp);
    }
    {
        GtkWidget *fb = gtk_button_new();
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_widget_add_css_class(fb, "hb-btn");
        gtk_widget_add_css_class(fb, "ghost");
        gtk_widget_add_css_class(fb, "sm");
        gtk_box_append(GTK_BOX(r), hb_icon_new("search", 13));
        gtk_box_append(GTK_BOX(r), gtk_label_new("Filter"));
        gtk_button_set_child(GTK_BUTTON(fb), r);
        g_signal_connect(fb, "clicked",
                         G_CALLBACK(on_filter_toggle), p);
        gtk_box_append(GTK_BOX(head), fb);
    }
    {
        GtkWidget *mb = gtk_menu_button_new();
        gtk_widget_add_css_class(mb, "hb-btn");
        gtk_widget_add_css_class(mb, "ghost");
        gtk_widget_add_css_class(mb, "sm");
        gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "Actions ▾");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb),
                                     G_MENU_MODEL(build_pane_menu(p)));
        gtk_box_append(GTK_BOX(head), mb);
    }
    gtk_box_append(GTK_BOX(box), head);

    /* filter row */
    p->filter_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(p->filter_row, "sftp-head");
    p->filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->filter_entry),
                                   "过滤文件名…");
    gtk_widget_set_hexpand(p->filter_entry, TRUE);
    g_signal_connect(p->filter_entry, "changed",
                     G_CALLBACK(on_filter_text), p);
    gtk_box_append(GTK_BOX(p->filter_row), p->filter_entry);
    gtk_widget_set_visible(p->filter_row, FALSE);
    gtk_box_append(GTK_BOX(box), p->filter_row);

    /* nav row */
    nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(nav, "crumbs-row");
    p->back_b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(p->back_b),
                         hb_icon_new("back", 13));
    gtk_widget_add_css_class(p->back_b, "navbtn");
    g_signal_connect(p->back_b, "clicked", G_CALLBACK(on_back), p);
    gtk_box_append(GTK_BOX(nav), p->back_b);
    p->fwd_b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(p->fwd_b),
                         hb_icon_new("fwd", 13));
    gtk_widget_add_css_class(p->fwd_b, "navbtn");
    g_signal_connect(p->fwd_b, "clicked", G_CALLBACK(on_fwd), p);
    gtk_box_append(GTK_BOX(nav), p->fwd_b);
    p->up_b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(p->up_b),
                         hb_icon_new("up", 13));
    gtk_widget_add_css_class(p->up_b, "navbtn");
    gtk_widget_set_tooltip_text(p->up_b, "上级目录");
    g_signal_connect(p->up_b, "clicked", G_CALLBACK(on_up), p);
    gtk_box_append(GTK_BOX(nav), p->up_b);
    p->crumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(nav), p->crumbs);
    gtk_box_append(GTK_BOX(box), nav);

    /* model */
    p->model = g_list_store_new(HB_TYPE_FILE_ITEM);
    p->filter = gtk_custom_filter_new(filter_fn, p, NULL);
    p->filtered = gtk_filter_list_model_new(
        G_LIST_MODEL(p->model), GTK_FILTER(p->filter));
    {
        GtkSorter *cols;
        GtkMultiSorter *ms = gtk_multi_sorter_new();
        gtk_multi_sorter_append(ms,
            GTK_SORTER(gtk_custom_sorter_new(dirfirst_cmp,
                                             NULL, NULL)));
        p->sorted = gtk_sort_list_model_new(
            G_LIST_MODEL(p->filtered), NULL);
        p->sel = gtk_multi_selection_new(G_LIST_MODEL(p->sorted));
        p->colview = gtk_column_view_new(
            GTK_SELECTION_MODEL(p->sel));
        cols = gtk_column_view_get_sorter(
            GTK_COLUMN_VIEW(p->colview));
        gtk_multi_sorter_append(ms, g_object_ref(cols));
        gtk_sort_list_model_set_sorter(p->sorted,
                                       GTK_SORTER(ms));
        g_object_unref(ms);
    }
    gtk_widget_set_vexpand(p->colview, TRUE);
    gtk_widget_add_css_class(p->colview, "filetable");

    /* columns */
#define ADDCOL(title_, setup_, bind_, sorter_) do { \
        f = gtk_signal_list_item_factory_new();     \
        g_signal_connect(f, "setup", G_CALLBACK(fac_setup), p); \
        g_signal_connect(f, "bind", G_CALLBACK(bind_), p);      \
        col = gtk_column_view_column_new(title_, f);            \
        gtk_column_view_column_set_expand(col, TRUE);           \
        gtk_column_view_column_set_sorter(                      \
            col, GTK_SORTER(gtk_custom_sorter_new(              \
                sorter_, NULL, NULL)));                         \
        gtk_column_view_append_column(                          \
            GTK_COLUMN_VIEW(p->colview), col);                  \
        g_object_unref(col);                                    \
    } while (0)
    ADDCOL("Name", fac_setup, fac_bind_name, name_cmp);
    ADDCOL("修改时间", fac_setup, fac_bind_mtime, mtime_cmp);
    ADDCOL("大小", fac_setup, fac_bind_size, size_cmp);
    ADDCOL("类型", fac_setup, fac_bind_kind, name_cmp);
    ADDCOL("权限", fac_setup, fac_bind_mode, mode_cmp);
#undef ADDCOL

    {
        GtkWidget *scr = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scr, TRUE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr),
                                      p->colview);
        gtk_box_append(GTK_BOX(box), scr);
    }

    /* activate（双击/Enter）：目录进入，文件传输到对侧 */
    g_signal_connect(p->colview, "activate",
                     G_CALLBACK(on_row_activate), p);
    /* 点击行把焦点拉进 pane：单元格不可聚焦，焦点不会自动进来，
     * 快捷键（挂在 box 上）依赖 pane 内焦点 */
    {
        GtkGesture *g = gtk_gesture_click_new();
        /* button 1 即可：设 0 会在冒泡/捕获阶段把右键序列
         * 也认领掉，单元格上的右键菜单手势收不到事件 */
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 1);
        g_object_set_data(G_OBJECT(g), "pane", p);
        g_signal_connect(g, "pressed",
            G_CALLBACK(on_col_press), NULL);
        gtk_widget_add_controller(p->colview,
                                  GTK_EVENT_CONTROLLER(g));
    }
    /* 右键菜单 / 拖拽挂在 colview 且用 capture 阶段：
     * GTK 内部行 widget 会先在捕获阶段认领按键序列，
     * 挂在单元格里的手势收不到 button-3 / 拖拽事件。 */
    {
        GtkGesture *g = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 3);
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(g), GTK_PHASE_CAPTURE);
        g_object_set_data(G_OBJECT(g), "pane", p);
        g_signal_connect(g, "pressed",
            G_CALLBACK(on_row_menu), NULL);
        gtk_widget_add_controller(p->colview,
                                  GTK_EVENT_CONTROLLER(g));
    }
    {
        GtkDragSource *ds = gtk_drag_source_new();
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(ds), GTK_PHASE_CAPTURE);
        g_object_set_data(G_OBJECT(ds), "pane", p);
        g_signal_connect(ds, "prepare",
                         G_CALLBACK(drag_prepare), NULL);
        gtk_widget_add_controller(p->colview,
                                  GTK_EVENT_CONTROLLER(ds));
    }

    /* action group */
    {
        GSimpleActionGroup *g = g_simple_action_group_new();
        int k;
        for (k = 0; k < 9; k++) {
            GSimpleAction *a = g_simple_action_new(menu_names[k],
                                                   NULL);
            g_signal_connect(a, "activate",
                             G_CALLBACK(menu_cb), p);
            g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
            g_object_unref(a);
        }
        gtk_widget_insert_action_group(box, "pane",
                                       G_ACTION_GROUP(g));
        g_object_unref(g);
    }

    /* keyboard shortcuts */
    {
        GtkEventController *kc = gtk_event_controller_key_new();
        g_object_set_data(G_OBJECT(kc), "pane", p);
        g_signal_connect(kc, "key-pressed",
                         G_CALLBACK(on_pane_key), NULL);
        gtk_widget_add_controller(box, kc);
    }
    /* drop：外部文件管理器(GdkFileList)→上传；内联文本→对侧传输 */
    {
        GType tps[2] = { G_TYPE_STRING, GDK_TYPE_FILE_LIST };
        GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
        gtk_drop_target_set_gtypes(dt, tps, 2);
        g_object_set_data(G_OBJECT(dt), "pane", p);
        g_signal_connect(dt, "drop", G_CALLBACK(on_drop), NULL);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(dt));
    }
    return box;
}

/* ================= 视图 ================= */

static void clean_open_cache(void);
static void hb_sftp_view_dispose(GObject *o)
{
    HbSftpView *v = HB_SFTP_VIEW(o);
    int k;
    GList *l;
    for (k = 0; k < 2; k++) {
        Pane *p = &v->pane[k];
        /* 未完成删除立即执行 */
        p->gen++;
        pane_teardown_conn(p);
    }
    clean_open_cache();
    for (l = v->xers; l; l = l->next) {
        Xfer *x = l->data;
        if (x->state == XS_RUN) {
            if (x->req && x->sess) hb_sftp_cancel(x->sess, x->req);
            if (x->canc) g_cancellable_cancel(x->canc);
        }
        xfer_free(x);
    }
    g_list_free(v->xers);
    v->xers = NULL;
    G_OBJECT_CLASS(hb_sftp_view_parent_class)->dispose(o);
}

static void hb_sftp_view_finalize(GObject *o)
{
    HbSftpView *v = HB_SFTP_VIEW(o);
    int k;
    for (k = 0; k < 2; k++) {
        Pane *p = &v->pane[k];
        g_free(p->conn_host_id);
        g_free(p->host_id);
        g_free(p->hname);
        g_free(p->os_key);
        g_free(p->cwd);
        g_clear_pointer(&p->back, g_ptr_array_unref);
        g_clear_pointer(&p->fwd, g_ptr_array_unref);
    }
    g_free(v->tab_id);
    G_OBJECT_CLASS(hb_sftp_view_parent_class)->finalize(o);
}

static void hb_sftp_view_class_init(HbSftpViewClass *k)
{
    GObjectClass *oc = G_OBJECT_CLASS(k);
    oc->dispose = hb_sftp_view_dispose;
    oc->finalize = hb_sftp_view_finalize;
}

static void hb_sftp_view_init(HbSftpView *v)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(GTK_WIDGET(v), "sftp-col");
    v->next_xid = 0;
}

/* app 启动/退出时清理 open 缓存目录 */
static void clean_open_cache(void)
{
    char *d = g_strdup_printf("%s/harbor/open",
                              g_get_user_cache_dir());
    GDir *g = g_dir_open(d, 0, NULL);
    const gchar *nm;
    if (g) {
        while ((nm = g_dir_read_name(g))) {
            char *sub = g_build_filename(d, nm, NULL);
            GDir *s2 = g_dir_open(sub, 0, NULL);
            const gchar *f;
            if (s2) {
                while ((f = g_dir_read_name(s2))) {
                    char *p = g_build_filename(sub, f, NULL);
                    g_remove(p);
                    g_free(p);
                }
                g_dir_close(s2);
            }
            g_rmdir(sub);
            g_free(sub);
        }
        g_dir_close(g);
    }
    g_free(d);
}

GtkWidget *hb_sftp_view_new(HbStore *st)
{
    HbSftpView *v = g_object_new(HB_TYPE_SFTP_VIEW, NULL);
    clean_open_cache();
    GtkWidget *panes, *xwrap, *scr;
    int k;

    v->store = st;
    v->conc = read_conc(v);

    /* 双栏 */
    panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(panes, "sftp-view");
    gtk_widget_set_vexpand(panes, TRUE);
    gtk_widget_set_hexpand(panes, TRUE);
    for (k = 0; k < 2; k++) {
        Pane *p = &v->pane[k];
        GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        p->v = v;
        p->side = k;
        p->stack = gtk_stack_new();
        gtk_widget_set_hexpand(p->stack, TRUE);
        gtk_widget_set_vexpand(p->stack, TRUE);
        gtk_stack_add_named(GTK_STACK(p->stack),
                            build_select(p), "select");
        p->conn_view = hb_connect_view_new();
        g_signal_connect(p->conn_view, "close",
                         G_CALLBACK(on_cv_close), p);
        g_signal_connect(p->conn_view, "start-over",
                         G_CALLBACK(on_cv_retry), p);
        gtk_stack_add_named(GTK_STACK(p->stack), p->conn_view,
                            "conn");
        gtk_stack_add_named(GTK_STACK(p->stack),
                            build_browser(p), "browse");
        pane_set_state(p, PS_SELECT);
        gtk_widget_set_hexpand(wrap, TRUE);
        gtk_box_append(GTK_BOX(wrap), p->stack);
        if (k) gtk_widget_add_css_class(wrap, "sftp-pane-r");
        gtk_box_append(GTK_BOX(panes), wrap);
    }
    gtk_box_append(GTK_BOX(v), panes);

    /* 传输队列 */
    xwrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(xwrap, "xfer-wrap");
    {
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(h, "xfer-head");
        gtk_box_append(GTK_BOX(h), hb_icon_new("fwd", 13));
        v->xhead = gtk_label_new("传输队列 · 0 进行中");
        gtk_label_set_xalign(GTK_LABEL(v->xhead), 0);
        gtk_box_append(GTK_BOX(h), v->xhead);
        {
            GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_hexpand(sp, TRUE);
            gtk_box_append(GTK_BOX(h), sp);
        }
        v->xclear = gtk_button_new_with_label("清空已完成");
        gtk_widget_add_css_class(v->xclear, "hb-btn");
        gtk_widget_add_css_class(v->xclear, "ghost");
        gtk_widget_add_css_class(v->xclear, "sm");
        gtk_widget_set_visible(v->xclear, FALSE);
        g_signal_connect(v->xclear, "clicked",
                         G_CALLBACK(on_clear_done), v);
        gtk_box_append(GTK_BOX(h), v->xclear);
        gtk_box_append(GTK_BOX(xwrap), h);
    }
    scr = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scr, -1, -1);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(scr), 130);   /* ~3.5 行 */
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scr), TRUE);
    v->xlist = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), v->xlist);
    gtk_box_append(GTK_BOX(xwrap), scr);
    v->xwrap = xwrap;
    gtk_box_append(GTK_BOX(v), xwrap);

    return GTK_WIDGET(v);
}

void hb_sftp_view_attach_tab(HbSftpView *v, GtkWidget *win,
                             const char *tab_id)
{
    v->win = HB_WINDOW(win);
    g_free(v->tab_id);
    v->tab_id = g_strdup(tab_id);
}

void hb_sftp_view_open_host(HbSftpView *v, const char *host_id)
{
    pane_pick(&v->pane[0], NULL);       /* 左 = Local home */
    pane_pick(&v->pane[1], host_id);    /* 右 = host 连接 */
}
