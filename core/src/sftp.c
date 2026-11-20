/* sftp.c — SFTP 请求引擎（FR-05 + FR-19 Phase 1）
 *
 * 线程模型：所有 libssh2 调用只跑在 session 线程（tr.run）。
 * 公共 API 任意线程可调：mu 下入队 + cmd 管道唤醒；结果经
 * HB_SESS_EV_SFTP 事件回 UI。
 *
 * libssh2 约束（重要）：SFTP 控制面操作（open/close/stat/readdir/
 * rename/unlink/mkdir/rmdir/setstat/symlink…）的状态机字段共享在同一
 * LIBSSH2_SFTP 对象上（sftp->open_state / readdir_state / …），同类
 * 同时只能一个在飞；不同 HANDLE 的 sftp_read/sftp_write 状态在句柄
 * 上，可以并发。因此引擎设一个全局 ctl 槽串行化所有控制面调用，文件
 * 数据读写直接做 —— 传输并发由后者提供。EAGAIN 后必须用相同参数重试
 * 同一调用（否则已发出的请求串会丢）。
 *
 * 断线清理：本地临时文件总能删；远端 .harbor-part-* 在断线时清不掉会
 * 留残件，但绝不留下半截最终文件（先写临时名，收尾才 rename 进位）。
 */

#include "harbor/sftp.h"

#include "session_priv.h"
#include "sftp_priv.h"
#include "ssh_priv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <poll.h>
#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHUNK      (4 * 1024 * 1024) /* 传输块：libssh2 ≥1.10 内部按块
                                      大小做请求流水，大块吞吐高 */
#define PROG_MS    100            /* 进度事件最小间隔 */
#define DEF_CONC   3
#define MAX_CONC   8

enum {
    RQ_REALPATH, RQ_LIST, RQ_STAT, RQ_MKDIR, RQ_RENAME, RQ_REMOVE,
    RQ_CHMOD, RQ_UPLOAD, RQ_DOWNLOAD, RQ_DRAIN
};

/* ctl 槽操作码：所有共享状态机的调用都经这里串行 */
enum {
    CTL_LSTAT, CTL_STAT, CTL_SETSTAT, CTL_MKDIR, CTL_RMDIR, CTL_UNLINK,
    CTL_RENAME, CTL_PRENAME,     /* PRENAME = posix-rename@openssh.com */
    CTL_OPENDIR, CTL_CLOSE, CTL_READDIR,
    CTL_OPEN_R, CTL_OPEN_W,
    CTL_REALPATH, CTL_READLINK
};

/* step 返回值 */
enum { ST_DONE = 1, ST_BLK, ST_WORK };
/* 通用僵尸态：发终态前排干远端句柄 */
#define ST_ZOM 900

/* 传输子状态 */
enum {
    X_SFTP_INIT = 0,            /* 建本传输的独立 sftp channel */
    X_LSTAT_SRC,
    X_WALK,
    X_MKDIRS,
    X_INIT, X_PROBE,
    X_OPEN_L, X_OPEN_R, X_RW,
    X_CLOSE_R, X_META, X_REN_RM, X_REN_UNLINK, X_REN,
    X_Z_CLOSE, X_Z_UNLINK, X_Z_SFTP, X_Z_DONE
};

typedef struct sfreq sfreq;

typedef struct dirstk {              /* 远端目录递归栈帧 */
    LIBSSH2_SFTP_HANDLE *dh;
    char   *path;                    /* 该层远端路径 */
    char   *lpath;                   /* 该层本地路径（下载遍历用） */
    struct dirstk *up;
} dirstk;

typedef struct {                     /* 传输清单条目（文件或目录） */
    char    *lp, *rp;
    uint64_t size;
    int64_t  mtime;
    uint32_t mode;
    int      is_dir;
} xent;

typedef struct xfr {                 /* 传输运行态 */
    LIBSSH2_SFTP *sf;                /* 本传输独占的 sftp channel：
                                        共享会话上并发流水线调用会互踩
                                        （session 级 write_state/partial），
                                        独立 channel 才真正并发 */
    xent    *files;  size_t nf, fi, fcap;
    xent    *dirs;   size_t nd, di, dcap;
    bool     walking;                /* 遍历阶段 → total 报 0 */
    /* 当前文件 */
    FILE    *lf;                     /* upload：本地读 */
    int      lfd;                    /* download：本地写 */
    LIBSSH2_SFTP_HANDLE *rh;
    uint8_t   buf[CHUNK];
    size_t    blen, boff;
    char     *part;                  /* 临时路径（远端或本地） */
    char     *fin;                   /* 当前探测中的最终目标 */
    char     *base;                  /* 期望目标（RENAME 变体基） */
    int       ren_n;
    bool      fin_exists;
    bool      no_prename;            /* 已知无 posix-rename → 跳过 */
    uint64_t  cur_size;
    int64_t   cur_mtime, cur_atime;
    uint32_t  cur_mode;
    uint64_t  total, done;
    uint32_t  files_total, files_done;
    int64_t   last_prog;
} xfr;

struct sfreq {
    uint32_t     id;
    int          op;
    bool         is_xfer;
    char        *a, *b;
    uint32_t     mode;
    bool         flag;
    int          conf;
    sfreq       *next;
    /* worker 态 */
    int          st;
    int          wsub;               /* 遍历/杂项子状态 */
    bool         ctl_pending;        /* 正持有 ctl 槽 */
    hb_sftp_entry *ents;
    size_t        nents, ecap;
    dirstk       *stk;
    char         *pend_child;        /* 待下钻子目录远端路径 */
    char         *pend_lpath;        /* …对应本地路径 */
    unsigned      nskip_bad;         /* 跳过的非法文件名（服务器侧） */
    unsigned      nskip_link;        /* 跳过的符号链接（递归传输不跟随） */
    xfr          *x;
    /* 终态暂存 */
    hb_sftp_status dst;
    char         *dmsg;              /* owned；NULL → status_msg(dst) */
    char         *dpath;             /* owned */
    bool          dskip;
};

typedef struct {                     /* ctl 槽：一次一个在飞 */
    sfreq   *owner;
    LIBSSH2_SFTP *sf;                /* 本 op 用的 sftp channel */
    int      fn;
    const char *p1; size_t p1l;
    const char *p2; size_t p2l;
    long     flags;
    LIBSSH2_SFTP_HANDLE *h;
    LIBSSH2_SFTP_ATTRIBUTES att;
    bool     att_set;
    char     nbuf[8192];
    char     lbuf[2048];
    int      rc;
} ctlop;

typedef struct {
    hb_session    *s;
    hb_ssh        *h;
    hop           *hp;
    LIBSSH2_SFTP  *sf;
    pthread_mutex_t mu;
    sfreq         *pend_h, *pend_t;
    uint32_t       next_id;
    int            conc;
    bool           dead;
    uint32_t      *cancel_ids;       /* UI 线程取消集合（mu 内） */
    size_t         nci, ccicap;
    /* worker 态 */
    sfreq         *act_h, *act_t;
    int            n_xfer;
    ctlop          ctl;
    sfreq          drain;            /* 空闲 POLLIN 消耗用伪请求 */
    bool           draining;
    /* 独占调用属主：sftp_init/sftp_shutdown 内含 channel_open/close，
     * 用 session 级 open_* 中间态 —— EAGAIN 续打期间不得有另一个同类
     * 调用（否则 open 态被踩）。e->sf 上的 ctl 调用由 ctl 槽天然串行，
     * 传输各有独立 sftp channel —— 均无需登记。 */
    sfreq          *io_owner;
    bool           conn_dead;
    bool           shutting;
} eng;

/* ============================ 小工具 ============================ */

/* 服务器报来的目录项名校验：拒绝 "."/".."、'/'、内嵌 NUL、<0x20 控制符，
 * 防恶意服务器经递归下载/remove 目录穿越（CVE-2019-6111 类）。 */
int hb__sftp_name_ok(const char *n, size_t nlen)
{
    size_t i;
    if (!n || !nlen || memchr(n, '\0', nlen))
        return 0;
    if ((nlen == 1 && n[0] == '.') ||
        (nlen == 2 && n[0] == '.' && n[1] == '.'))
        return 0;
    for (i = 0; i < nlen; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c == '/' || c < 0x20)
            return 0;
    }
    return 1;
}

char *hb__sftp_normpath(const char *p)
{
    /* 词法归一：多余 '/'、'.' 丢弃，'..' 弹一层（越界 '.. ' 也吞掉） */
    char *out, *w;
    const char *q;
    int abs_p;
    if (!p)
        return NULL;
    out = malloc(strlen(p) + 2);
    if (!out)
        return NULL;
    abs_p = (p[0] == '/');
    w = out;
    if (abs_p)
        *w++ = '/';
    q = p;
    while (*q) {
        const char *seg, *e;
        size_t sl;
        while (*q == '/')
            q++;
        seg = q;
        while (*q && *q != '/')
            q++;
        e = q;
        sl = (size_t)(e - seg);
        if (!sl || (sl == 1 && seg[0] == '.'))
            continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            /* 弹一层：回到上一个 '/'（保留根） */
            if (w > out + abs_p) {
                if (*(w - 1) == '/')
                    w--;
                while (w > out + abs_p && *(w - 1) != '/')
                    w--;
            }
            continue;
        }
        if (w > out + abs_p && *(w - 1) != '/')
            *w++ = '/';
        memcpy(w, seg, sl);
        w += sl;
    }
    if (w > out + abs_p && *(w - 1) == '/')
        w--;                        /* 去尾 '/'（根本身除外） */
    *w = 0;
    if (!*out)
        strcpy(out, ".");           /* 空/相对全弹光 → "." */
    return out;
}

int hb__sftp_path_under(const char *root, const char *path)
{
    char *nr = hb__sftp_normpath(root);
    char *np = hb__sftp_normpath(path);
    size_t rl;
    int ok = 0;
    if (!nr || !np) {
        free(nr);
        free(np);
        return 0;
    }
    rl = strlen(nr);
    if (!strcmp(nr, "/"))
        ok = np[0] == '/';              /* 根覆盖一切绝对路径 */
    else
        ok = !strcmp(nr, np) ||
             (!strncmp(nr, np, rl) && np[rl] == '/');
    free(nr);
    free(np);
    return ok;
}

static char *path_join(const char *d, const char *n)
{
    size_t dl = strlen(d), nl = strlen(n);
    char *p = malloc(dl + nl + 2);
    if (!p)
        return NULL;
    memcpy(p, d, dl);
    if (dl && d[dl - 1] != '/')
        p[dl++] = '/';
    memcpy(p + dl, n, nl + 1);
    return p;
}

static char *dir_name(const char *p)
{
    const char *sl = strrchr(p, '/');
    if (!sl)
        return strdup(".");
    if (sl == p)
        return strdup("/");
    return strndup(p, (size_t)(sl - p));
}

static const char *base_name(const char *p)
{
    const char *sl = strrchr(p, '/');
    return sl ? sl + 1 : p;
}

/* "dir/name.ext" × n → "dir/name (n).ext" */
static char *ren_variant(const char *base, int n)
{
    const char *bn = base_name(base);
    size_t dl = (size_t)(bn - base);
    const char *dot = strrchr(bn, '.');
    size_t stem = dot ? (size_t)(dot - bn) : strlen(bn);
    char tmp[64], *p;
    size_t need;
    if (!n)
        return strdup(base);
    snprintf(tmp, sizeof tmp, " (%d)", n);
    need = dl + stem + strlen(tmp) + (dot ? strlen(dot) : 0) + 1;
    p = malloc(need);
    if (!p)
        return NULL;
    memcpy(p, base, dl + stem);
    strcpy(p + dl + stem, tmp);
    if (dot)
        strcat(p, dot);
    return p;
}

/* <dir>/.<name>.harbor-part-<rand6>（远端临时名） */
static char *part_name(const char *fin)
{
    static const char t[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    uint8_t b[6];
    char *dn = dir_name(fin), suf[7], *p;
    size_t n;
    int i;
    if (!dn)
        return NULL;
    randombytes_buf(b, sizeof b);
    for (i = 0; i < 6; i++)
        suf[i] = t[b[i] % 36];
    suf[6] = 0;
    n = strlen(dn) + strlen(base_name(fin)) + 24;
    p = malloc(n);
    if (p)
        snprintf(p, n, "%s/.%s.harbor-part-%s", dn, base_name(fin), suf);
    free(dn);
    return p;
}

/* 本地临时：<dir>/.<name>.harbor-part-XXXXXX → mkstemp */
static int lpart_open(const char *fin, char **path_out, uint32_t mode)
{
    char *dn = dir_name(fin), *p;
    size_t n;
    int fd;
    if (!dn)
        return -1;
    n = strlen(dn) + strlen(base_name(fin)) + 24;
    p = malloc(n);
    if (!p) {
        free(dn);
        return -1;
    }
    snprintf(p, n, "%s/.%s.harbor-part-XXXXXX", dn, base_name(fin));
    free(dn);
    fd = mkstemp(p);
    if (fd >= 0) {
        fchmod(fd, (mode_t)(mode & 0777));
        *path_out = p;
    } else {
        free(p);
    }
    return fd;
}

static void fill_entry(hb_sftp_entry *en, const char *name, size_t nlen,
                       const LIBSSH2_SFTP_ATTRIBUTES *a, const char *ln)
{
    memset(en, 0, sizeof *en);
    en->name = strndup(name, nlen);
    if (a->flags & LIBSSH2_SFTP_ATTR_SIZE)
        en->size = a->filesize;
    if (a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
        en->mtime = (int64_t)a->mtime;
    if (a->flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        en->uid = (uint32_t)a->uid;
        en->gid = (uint32_t)a->gid;
    }
    if (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
        en->mode = (uint32_t)a->permissions;
        en->is_dir = LIBSSH2_SFTP_S_ISDIR(a->permissions) != 0;
        en->is_link = LIBSSH2_SFTP_S_ISLNK(a->permissions) != 0;
    }
    if (en->is_link && ln) {
        const char *p = NULL, *q = ln;
        while ((q = strstr(q, " -> ")))
            p = q, q += 4;          /* 取最后一次 → 避免名字含 " -> " */
        if (p)
            en->link_target = strdup(p + 4);
    }
}

static void entry_clear(hb_sftp_entry *en, size_t n)
{
    size_t i;
    if (!en)
        return;
    for (i = 0; i < n; i++) {
        free(en[i].name);
        free(en[i].link_target);
    }
    free(en);
}

void hb_sftp_result_free(hb_sftp_result *r)
{
    if (!r)
        return;
    free(r->message);
    free(r->path);
    entry_clear(r->entries, r->n);
    free(r);
}

/* ============================ 状态映射 ============================ */

static const char *status_msg(hb_sftp_status st)
{
    switch (st) {
    case HB_SFTP_ERR_NOENT:     return "没有那个文件或目录";
    case HB_SFTP_ERR_PERM:      return "权限不足";
    case HB_SFTP_ERR_EXISTS:    return "目标已存在";
    case HB_SFTP_ERR_NOTEMPTY:  return "目录非空";
    case HB_SFTP_ERR_CONN:      return "连接已断开";
    case HB_SFTP_ERR_CANCELLED: return "已取消";
    case HB_SFTP_ERR_IO:        return "读写错误";
    default:                    return "操作失败";
    }
}

static hb_sftp_status map_fx(unsigned long fx)
{
    switch (fx) {
    case LIBSSH2_FX_NO_SUCH_FILE:
    case LIBSSH2_FX_NO_SUCH_PATH:        return HB_SFTP_ERR_NOENT;
    case LIBSSH2_FX_PERMISSION_DENIED:
    case LIBSSH2_FX_WRITE_PROTECT:       return HB_SFTP_ERR_PERM;
    case LIBSSH2_FX_FILE_ALREADY_EXISTS: return HB_SFTP_ERR_EXISTS;
    case LIBSSH2_FX_DIR_NOT_EMPTY:       return HB_SFTP_ERR_NOTEMPTY;
    case LIBSSH2_FX_NO_CONNECTION:
    case LIBSSH2_FX_CONNECTION_LOST:     return HB_SFTP_ERR_CONN;
    default:                             return HB_SFTP_ERR_IO;
    }
}

/* sftp 调用出错后归类：传输层错 → CONN；服务器状态 → FX 映射 */
static hb_sftp_status last_err(eng *e, LIBSSH2_SFTP *sf)
{
    int se = libssh2_session_last_errno(e->hp->sess);
    if (se == LIBSSH2_ERROR_SFTP_PROTOCOL)
        return map_fx(libssh2_sftp_last_error(sf ? sf : e->sf));
    switch (se) {
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
    case LIBSSH2_ERROR_SOCKET_NONE:
    case LIBSSH2_ERROR_BANNER_RECV:
    case LIBSSH2_ERROR_BANNER_SEND:
    case LIBSSH2_ERROR_CHANNEL_CLOSED:
    case LIBSSH2_ERROR_CHANNEL_FAILURE:
        return HB_SFTP_ERR_CONN;
    default:
        return HB_SFTP_ERR_OTHER;
    }
}

/* ============================ 结果发射 ============================ */

static void emit_res(eng *e, sfreq *r, hb_sftp_rkind kind,
                     hb_sftp_status st, const char *msg)
{
    hb_sftp_result *res = calloc(1, sizeof *res);
    if (!res)
        return;
    res->req = r->id;
    res->kind = kind;
    res->status = st;
    if (!msg && kind == HB_SFTP_R_DONE &&
        (r->nskip_bad || r->nskip_link)) {
        /* 跳过计数随结果告知（非法名 / 不跟随的符号链接） */
        char sb[160];
        if (r->nskip_bad && r->nskip_link)
            snprintf(sb, sizeof sb,
                     "已跳过 %u 个非法文件名、%u 个符号链接",
                     r->nskip_bad, r->nskip_link);
        else if (r->nskip_bad)
            snprintf(sb, sizeof sb, "已跳过 %u 个非法文件名",
                     r->nskip_bad);
        else
            snprintf(sb, sizeof sb, "已跳过 %u 个符号链接",
                     r->nskip_link);
        res->message = strdup(sb);
    } else {
        res->message = msg ? strdup(msg) : NULL;
    }
    if (kind == HB_SFTP_R_DONE) {
        res->entries = r->ents;
        res->n = r->nents;
        r->ents = NULL;
        r->nents = 0;
        res->path = r->dpath;
        r->dpath = NULL;
        res->skipped = r->dskip;
    }
    if (r->x) {
        res->done = r->x->done;
        res->total = r->x->walking ? 0 : r->x->total;
        res->files_done = r->x->files_done;
        res->files_total = r->x->files_total;
    }
    hb__session_emit_sftp(e->s, res);
}

static void emit_prog(eng *e, sfreq *r, int force)
{
    int64_t now;
    if (!r->x)
        return;
    now = hb_now_ms();
    if (!force && now - r->x->last_prog < PROG_MS)
        return;
    r->x->last_prog = now;
    emit_res(e, r, HB_SFTP_R_PROGRESS, HB_SFTP_OK, NULL);
}

/* 终态登记 → 统一进 ST_ZOM：先排干远端句柄再发射 */
static int req_done(eng *e, sfreq *r, hb_sftp_status st, const char *msg)
{
    (void)e;
    r->dst = st;
    free(r->dmsg);
    r->dmsg = msg ? strdup(msg)
                  : (st == HB_SFTP_OK ? NULL : strdup(status_msg(st)));
    if (st != HB_SFTP_OK)
        entry_clear(r->ents, r->nents), r->ents = NULL, r->nents = 0;
    r->st = ST_ZOM;
    return ST_WORK;
}

static int req_fail(eng *e, sfreq *r)
{
    hb_sftp_status st = last_err(e, e->ctl.sf);
    if (st == HB_SFTP_ERR_CONN)
        e->conn_dead = true;
    return req_done(e, r, st, NULL);
}

/* ============================ ctl 槽 ============================ */

/* 独占调用登记/释放：仅 sftp_init/sftp_shutdown 用（见 eng.io_owner） */
static void io_take(eng *e, sfreq *r)
{
    e->io_owner = r;
}
static void io_rel(eng *e, sfreq *r)
{
    if (e->io_owner == r)
        e->io_owner = NULL;
}

static int ctl_pump(eng *e)
{
    ctlop *c = &e->ctl;
    LIBSSH2_SFTP *sf = c->sf;
    int rc;

    /* 独占调用在飞（sftp_init/shutdown）→ 让位；其余情形下本 op 所在
     * 的 sftp channel 只可能被槽主自己占用 → 直接泵。 */
    if (e->io_owner && e->io_owner != c->owner)
        return ST_BLK;

    switch (c->fn) {
    case CTL_LSTAT:
        rc = libssh2_sftp_stat_ex(sf, c->p1, (unsigned)c->p1l,
                                  LIBSSH2_SFTP_LSTAT, &c->att);
        break;
    case CTL_STAT:
        rc = libssh2_sftp_stat_ex(sf, c->p1, (unsigned)c->p1l,
                                  LIBSSH2_SFTP_STAT, &c->att);
        break;
    case CTL_SETSTAT:
        rc = libssh2_sftp_stat_ex(sf, c->p1, (unsigned)c->p1l,
                                  LIBSSH2_SFTP_SETSTAT, &c->att);
        break;
    case CTL_MKDIR:
        rc = libssh2_sftp_mkdir_ex(sf, c->p1, (unsigned)c->p1l, c->flags);
        break;
    case CTL_RMDIR:
        rc = libssh2_sftp_rmdir_ex(sf, c->p1, (unsigned)c->p1l);
        break;
    case CTL_UNLINK:
        rc = libssh2_sftp_unlink_ex(sf, c->p1, (unsigned)c->p1l);
        break;
    case CTL_RENAME:
        rc = libssh2_sftp_rename_ex(sf, c->p1, (unsigned)c->p1l,
                                    c->p2, (unsigned)c->p2l, 0);
        break;
    case CTL_PRENAME:
        rc = libssh2_sftp_rename_ex(sf, c->p1, (unsigned)c->p1l,
                                    c->p2, (unsigned)c->p2l,
                                    LIBSSH2_SFTP_RENAME_OVERWRITE |
                                    LIBSSH2_SFTP_RENAME_ATOMIC |
                                    LIBSSH2_SFTP_RENAME_NATIVE);
        break;
    case CTL_OPENDIR:
        c->h = libssh2_sftp_open_ex(sf, c->p1, (unsigned)c->p1l,
                                    0, 0, LIBSSH2_SFTP_OPENDIR);
        if (!c->h && libssh2_session_last_errno(e->hp->sess)
                    == LIBSSH2_ERROR_EAGAIN)
            return ST_BLK;          /* open 返回指针：NULL 也可能是 EAGAIN */
        rc = c->h ? 0 : -1;
        break;
    case CTL_CLOSE:
        rc = libssh2_sftp_close_handle(c->h);
        break;
    case CTL_READDIR:
        rc = libssh2_sftp_readdir_ex(c->h, c->nbuf, sizeof c->nbuf,
                                     c->lbuf, sizeof c->lbuf, &c->att);
        break;
    case CTL_OPEN_R:
        c->h = libssh2_sftp_open_ex(sf, c->p1, (unsigned)c->p1l,
                                    LIBSSH2_FXF_READ, 0,
                                    LIBSSH2_SFTP_OPENFILE);
        if (!c->h && libssh2_session_last_errno(e->hp->sess)
                    == LIBSSH2_ERROR_EAGAIN)
            return ST_BLK;
        rc = c->h ? 0 : -1;
        break;
    case CTL_OPEN_W:
        c->h = libssh2_sftp_open_ex(sf, c->p1, (unsigned)c->p1l,
                                    LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT |
                                    LIBSSH2_FXF_TRUNC,
                                    c->flags, LIBSSH2_SFTP_OPENFILE);
        if (!c->h && libssh2_session_last_errno(e->hp->sess)
                    == LIBSSH2_ERROR_EAGAIN)
            return ST_BLK;
        rc = c->h ? 0 : -1;
        break;
    case CTL_REALPATH:
        rc = libssh2_sftp_symlink_ex(sf, c->p1, (unsigned)c->p1l,
                                     c->nbuf, (unsigned)sizeof c->nbuf,
                                     LIBSSH2_SFTP_REALPATH);
        break;
    case CTL_READLINK:
        rc = libssh2_sftp_symlink_ex(sf, c->p1, (unsigned)c->p1l,
                                     c->nbuf, (unsigned)sizeof c->nbuf,
                                     LIBSSH2_SFTP_READLINK);
        break;
    default:
        rc = -1;
        break;
    }
    if (rc == LIBSSH2_ERROR_EAGAIN)
        return ST_BLK;              /* 槽主不变 → 下次同参续打 */
    c->rc = rc;
    c->owner = NULL;
    return ST_DONE;
}

/* 发起/续泵一个 ctl 操作；ST_BLK=在飞或槽忙，ST_DONE=完成（看 ctl.rc） */
static int ctl_do(eng *e, sfreq *r, int fn, const char *p1, const char *p2,
                  long flags, LIBSSH2_SFTP_HANDLE *h,
                  const LIBSSH2_SFTP_ATTRIBUTES *att)
{
    ctlop *c = &e->ctl;
    int st;
    if (c->owner == r) {
        st = ctl_pump(e);
        if (st == ST_DONE)
            r->ctl_pending = false;
        return st;
    }
    if (c->owner)
        return ST_BLK;              /* 槽忙 → 让位 */
    memset(c, 0, sizeof *c);
    c->owner = r;
    c->sf = (r->x && r->x->sf) ? r->x->sf : e->sf;
    c->fn = fn;
    c->p1 = p1;
    c->p1l = p1 ? strlen(p1) : 0;
    c->p2 = p2;
    c->p2l = p2 ? strlen(p2) : 0;
    c->flags = flags;
    c->h = h;
    if (att) {
        c->att = *att;
        c->att_set = true;
    }
    r->ctl_pending = true;
    st = ctl_pump(e);
    if (st == ST_DONE)
        r->ctl_pending = false;
    return st;
}

static void ctl_drop(eng *e, sfreq *r)
{
    if (e->ctl.owner == r) {
        e->ctl.owner = NULL;
        r->ctl_pending = false;
    }
}

/* ============================ 请求释放 ============================ */

static void stk_pop(sfreq *r)
{
    dirstk *f = r->stk;
    if (!f)
        return;
    r->stk = f->up;
    free(f->path);
    free(f->lpath);
    free(f);
}

static void xent_free(xent *a, size_t n)
{
    size_t i;
    if (!a)
        return;
    for (i = 0; i < n; i++) {
        free(a[i].lp);
        free(a[i].rp);
    }
    free(a);
}

static void req_free(sfreq *r)
{
    xfr *x;
    if (!r)
        return;
    free(r->a);
    free(r->b);
    free(r->dmsg);
    free(r->dpath);
    free(r->pend_child);
    free(r->pend_lpath);
    entry_clear(r->ents, r->nents);
    x = r->x;
    if (x) {
        if (x->lf)
            fclose(x->lf);
        if (x->lfd >= 0)
            close(x->lfd);
        if (x->sf) {
            /* 尽力而为：活通道正常走 X_Z_SFTP 才会到这（如断线 abort），
             * 死亡通道的 close 立刻失败、对象照释放 */
            libssh2_sftp_shutdown(x->sf);
            x->sf = NULL;
        }
        free(x->part);
        free(x->fin);
        free(x->base);
        xent_free(x->files, x->nf);
        xent_free(x->dirs, x->nd);
        free(x);
    }
    while (r->stk)
        stk_pop(r);
    free(r);
}

/* ============================ 元数据 step ============================ */

/* 通用僵尸态：关 stk 上的远端目录柄（每次一个），然后发终态 */
static int step_zombie(eng *e, sfreq *r)
{
    if (r->stk && !e->conn_dead) {
        if (ctl_do(e, r, CTL_CLOSE, NULL, NULL, 0, r->stk->dh, NULL)
            == ST_BLK)
            return ST_BLK;
        stk_pop(r);
        return ST_WORK;
    }
    emit_res(e, r, HB_SFTP_R_DONE, r->dst, r->dmsg);
    return ST_DONE;
}

static int step_realpath(eng *e, sfreq *r)
{
    if (ctl_do(e, r, CTL_REALPATH, r->a, NULL, 0, NULL, NULL) == ST_BLK)
        return ST_BLK;
    if (e->ctl.rc < 0)
        return req_fail(e, r);
    free(r->dpath);
    r->dpath = strndup(e->ctl.nbuf, (size_t)e->ctl.rc);
    return req_done(e, r, HB_SFTP_OK, NULL);
}

static int step_stat(eng *e, sfreq *r)
{
    hb_sftp_entry *en;
    if (ctl_do(e, r, CTL_LSTAT, r->a, NULL, 0, NULL, NULL) == ST_BLK)
        return ST_BLK;
    if (e->ctl.rc < 0)
        return req_fail(e, r);
    en = calloc(1, sizeof *en);
    if (!en)
        return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
    fill_entry(en, base_name(r->a), strlen(base_name(r->a)),
               &e->ctl.att, NULL);
    r->ents = en;
    r->nents = 1;
    return req_done(e, r, HB_SFTP_OK, NULL);
}

static int step_mkdir(eng *e, sfreq *r)
{
    if (ctl_do(e, r, CTL_MKDIR, r->a, NULL, (long)(r->mode & 07777),
               NULL, NULL) == ST_BLK)
        return ST_BLK;
    if (e->ctl.rc < 0)
        return req_fail(e, r);
    return req_done(e, r, HB_SFTP_OK, NULL);
}

static int step_chmod(eng *e, sfreq *r)
{
    LIBSSH2_SFTP_ATTRIBUTES att;
    memset(&att, 0, sizeof att);
    att.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    att.permissions = r->mode & 07777;
    if (ctl_do(e, r, CTL_SETSTAT, r->a, NULL, 0, NULL, &att) == ST_BLK)
        return ST_BLK;
    if (e->ctl.rc < 0)
        return req_fail(e, r);
    return req_done(e, r, HB_SFTP_OK, NULL);
}

static int step_list(eng *e, sfreq *r)
{
    switch (r->st) {
    case 0: {
        dirstk *f;
        if (ctl_do(e, r, CTL_OPENDIR, r->a, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        f = calloc(1, sizeof *f);
        if (!f)
            return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        f->dh = e->ctl.h;
        f->path = strdup(r->a);
        r->stk = f;
        r->st = 1;
        return ST_WORK;
    }
    case 1: {
        hb_sftp_entry *en;
        if (ctl_do(e, r, CTL_READDIR, NULL, NULL, 0, r->stk->dh, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        if (e->ctl.rc == 0) {           /* EOF → 补读符号链接目标 */
            r->st = 3;
            r->wsub = 0;
            return ST_WORK;
        }
        if (!hb__sftp_name_ok(e->ctl.nbuf, (size_t)e->ctl.rc)) {
            if (strcmp(e->ctl.nbuf, ".") && strcmp(e->ctl.nbuf, ".."))
                r->nskip_bad++;     /* 真·非法名（./.. 是正常噪音） */
            return ST_WORK;
        }
        if (r->nents == r->ecap) {
            size_t nc = r->ecap ? r->ecap * 2 : 32;
            hb_sftp_entry *ne = realloc(r->ents, nc * sizeof *ne);
            if (!ne)
                return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            r->ents = ne;
            r->ecap = nc;
        }
        en = &r->ents[r->nents];
        fill_entry(en, e->ctl.nbuf, (size_t)e->ctl.rc, &e->ctl.att,
                   e->ctl.lbuf);
        if (!en->name)
            return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        r->nents++;
        return ST_WORK;
    }
    case 2:
        if (ctl_do(e, r, CTL_CLOSE, NULL, NULL, 0, r->stk->dh, NULL)
            == ST_BLK)
            return ST_BLK;
        stk_pop(r);
        return req_done(e, r, HB_SFTP_OK, NULL);
    case 3: {                   /* internal-sftp longname 不含 "-> 目标"，
                                   符号链接逐个 READLINK 补 target */
        size_t i = (size_t)r->wsub;
        while (i < r->nents &&
               !(r->ents[i].is_link && !r->ents[i].link_target))
            i++;
        if (i >= r->nents) {
            r->st = 2;
            return ST_WORK;
        }
        if (!r->pend_child) {
            r->pend_child = path_join(r->a, r->ents[i].name);
            if (!r->pend_child)
                return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        }
        if (ctl_do(e, r, CTL_READLINK, r->pend_child, NULL, 0, NULL,
                   NULL) == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc > 0) {
            free(r->ents[i].link_target);
            r->ents[i].link_target =
                strndup(e->ctl.nbuf, (size_t)e->ctl.rc);
        }
        free(r->pend_child);
        r->pend_child = NULL;
        r->wsub = (int)i + 1;
        return ST_WORK;
    }
    }
    return req_done(e, r, HB_SFTP_ERR_OTHER, NULL);
}

static int step_rename(eng *e, sfreq *r)
{
    switch (r->st) {
    case 0:                     /* 先查目标存在性 */
        if (ctl_do(e, r, CTL_LSTAT, r->b, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc == 0) {
            if (!r->flag)
                return req_done(e, r, HB_SFTP_ERR_EXISTS, NULL);
            r->st = 1;
            return ST_WORK;
        }
        if (last_err(e, e->ctl.sf) == HB_SFTP_ERR_NOENT) {
            r->st = 2;
            return ST_WORK;
        }
        return req_fail(e, r);
    case 1:                     /* posix-rename 原子覆盖先试 */
        if (ctl_do(e, r, CTL_PRENAME, r->a, r->b, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc == 0)
            return req_done(e, r, HB_SFTP_OK, NULL);
        {
            hb_sftp_status st = last_err(e, e->ctl.sf);
            if (st == HB_SFTP_ERR_CONN || st == HB_SFTP_ERR_PERM ||
                st == HB_SFTP_ERR_NOENT)
                return req_done(e, r, st, NULL);
        }
        r->st = 3;              /* 扩展不受支持 → unlink + 普通 rename */
        return ST_WORK;
    case 3:
        if (ctl_do(e, r, CTL_UNLINK, r->b, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        /* 忽略 unlink 结果（竞态下目标可能已消失） */
        r->st = 2;
        return ST_WORK;
    case 2:
        if (ctl_do(e, r, CTL_RENAME, r->a, r->b, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        return req_done(e, r, HB_SFTP_OK, NULL);
    }
    return req_done(e, r, HB_SFTP_ERR_OTHER, NULL);
}

static int step_remove(eng *e, sfreq *r)
{
    dirstk *f;
    switch (r->st) {
    case 0:
        if (ctl_do(e, r, CTL_LSTAT, r->a, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        if ((e->ctl.att.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
            LIBSSH2_SFTP_S_ISDIR(e->ctl.att.permissions)) {
            r->st = r->flag ? 1 : 4;
        } else {
            r->st = 5;
        }
        return ST_WORK;
    case 1: {                   /* opendir 一层并入栈 */
        const char *p = r->pend_child ? r->pend_child : r->a;
        if (ctl_do(e, r, CTL_OPENDIR, p, NULL, 0, NULL, NULL) == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        f = calloc(1, sizeof *f);
        if (!f)
            return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        f->dh = e->ctl.h;
        f->path = strdup(p);
        free(r->pend_child);
        r->pend_child = NULL;
        if (!f->path) {
            free(f);
            return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        }
        f->up = r->stk;
        r->stk = f;
        r->st = 2;
        return ST_WORK;
    }
    case 2:                     /* readdir 栈顶 */
        if (ctl_do(e, r, CTL_READDIR, NULL, NULL, 0, r->stk->dh, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        if (e->ctl.rc == 0) {
            r->st = 6;
            return ST_WORK;
        }
        if (!hb__sftp_name_ok(e->ctl.nbuf, (size_t)e->ctl.rc)) {
            if (strcmp(e->ctl.nbuf, ".") && strcmp(e->ctl.nbuf, ".."))
                r->nskip_bad++;     /* 非法名不进 remove 遍历 */
            return ST_WORK;
        }
        free(r->pend_child);
        r->pend_child = path_join(r->stk->path, e->ctl.nbuf);
        if (!r->pend_child)
            return req_done(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        r->st = ((e->ctl.att.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                 LIBSSH2_SFTP_S_ISDIR(e->ctl.att.permissions))
                ? 1 : 3;
        return ST_WORK;
    case 3:
        if (ctl_do(e, r, CTL_UNLINK, r->pend_child, NULL, 0, NULL,
                   NULL) == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        free(r->pend_child);
        r->pend_child = NULL;
        r->st = 2;
        return ST_WORK;
    case 6:                     /* 栈顶 EOF：closedir + rmdir */
        if (ctl_do(e, r, CTL_CLOSE, NULL, NULL, 0, r->stk->dh, NULL)
            == ST_BLK)
            return ST_BLK;
        free(r->pend_child);
        r->pend_child = r->stk->path;   /* rmdir 目标 */
        r->stk->path = NULL;
        stk_pop(r);
        r->st = 7;
        return ST_WORK;
    case 7:
        if (ctl_do(e, r, CTL_RMDIR, r->pend_child, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        free(r->pend_child);
        r->pend_child = NULL;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        if (r->stk) {
            r->st = 2;
            return ST_WORK;
        }
        return req_done(e, r, HB_SFTP_OK, NULL);
    case 4:                     /* 非递归删目录 */
        if (ctl_do(e, r, CTL_RMDIR, r->a, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0) {
            hb_sftp_status st = last_err(e, e->ctl.sf);
            if (st == HB_SFTP_ERR_IO)   /* 老服务器 ENOTEMPTY 报 FAILURE */
                st = HB_SFTP_ERR_NOTEMPTY;
            return req_done(e, r, st, NULL);
        }
        return req_done(e, r, HB_SFTP_OK, NULL);
    case 5:
        if (ctl_do(e, r, CTL_UNLINK, r->a, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return req_fail(e, r);
        return req_done(e, r, HB_SFTP_OK, NULL);
    }
    return req_done(e, r, HB_SFTP_ERR_OTHER, NULL);
}

/* ============================ 传输 ============================ */

static int xent_push(xent **a, size_t *n, size_t *cap, const char *lp,
                     const char *rp, const struct stat *lst,
                     const LIBSSH2_SFTP_ATTRIBUTES *att)
{
    xent *ne;
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        ne = realloc(*a, nc * sizeof *ne);
        if (!ne)
            return -1;
        *a = ne;
        *cap = nc;
    }
    ne = &(*a)[*n];
    memset(ne, 0, sizeof *ne);
    ne->lp = strdup(lp);
    ne->rp = strdup(rp);
    if (!ne->lp || !ne->rp) {
        free(ne->lp);
        free(ne->rp);
        return -1;
    }
    if (lst) {
        ne->size = (uint64_t)lst->st_size;
        ne->mtime = (int64_t)lst->st_mtime;
        ne->mode = (uint32_t)lst->st_mode;
        ne->is_dir = S_ISDIR(lst->st_mode);
    } else if (att) {
        if (att->flags & LIBSSH2_SFTP_ATTR_SIZE)
            ne->size = att->filesize;
        if (att->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
            ne->mtime = (int64_t)att->mtime;
        if (att->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            ne->mode = (uint32_t)att->permissions;
            ne->is_dir = LIBSSH2_SFTP_S_ISDIR(att->permissions);
        }
    }
    (*n)++;
    return 0;
}

/* upload：本地目录递归收集（同步；本地 IO 快）。递归不跟随符号链接：
 * 跳过并计数（防逃出树、防环）。 */
static int walk_local(xfr *x, const char *ldir, const char *rdir,
                      unsigned *nskip_link)
{
    DIR *d = opendir(ldir);
    struct dirent *de;
    if (!d)
        return -1;
    while ((de = readdir(d))) {
        char *lp, *rp;
        struct stat st;
        int bad = 0;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        lp = path_join(ldir, de->d_name);
        rp = path_join(rdir, de->d_name);
        if (!lp || !rp || lstat(lp, &st) < 0)
            bad = 1;
        if (!bad && S_ISLNK(st.st_mode)) {
            (*nskip_link)++;
            free(lp);
            free(rp);
            continue;
        }
        if (!bad && S_ISDIR(st.st_mode))
            bad = xent_push(&x->dirs, &x->nd, &x->dcap, lp, rp, &st,
                            NULL) || walk_local(x, lp, rp, nskip_link);
        else if (!bad)
            bad = xent_push(&x->files, &x->nf, &x->fcap, lp, rp, &st,
                            NULL);
        free(lp);
        free(rp);
        if (bad) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int xfer_fail(eng *e, sfreq *r, hb_sftp_status st, const char *msg)
{
    xfr *x = r->x;
    r->dst = st;
    free(r->dmsg);
    r->dmsg = msg ? strdup(msg) : strdup(status_msg(st));
    if (x) {
        entry_clear(r->ents, r->nents);
        r->ents = NULL;
        r->nents = 0;
    }
    if (st == HB_SFTP_ERR_CONN)
        e->conn_dead = true;
    r->st = X_Z_CLOSE;
    return ST_WORK;
}

/* 单文件收尾后登记结果路径（RENAME 后是实际名） */
static void xfer_set_path(sfreq *r)
{
    xfr *x = r->x;
    if (x->nf == 1) {
        free(r->dpath);
        r->dpath = strdup(x->fin ? x->fin : "");
    }
}

static int xfer_next(sfreq *r)
{
    xfr *x = r->x;
    x->fi++;
    if (x->fi < x->nf) {
        r->st = X_INIT;
        return ST_WORK;
    }
    if (x->nf > 1) {                    /* 目录传输：结果路径=目标根 */
        free(r->dpath);
        r->dpath = strdup(x->dirs[0].lp && r->op == RQ_DOWNLOAD
                          ? x->dirs[0].lp : x->dirs[0].rp);
    }
    r->dst = HB_SFTP_OK;
    r->st = X_Z_DONE;
    return ST_WORK;
}

/* 有元数据请求（LIST/STAT/…）在跑或排队？ */
static int meta_pending(eng *e)
{
    sfreq *r;
    for (r = e->act_h; r; r = r->next)
        if (!r->is_xfer)
            return 1;
    pthread_mutex_lock(&e->mu);
    for (r = e->pend_h; r; r = r->next)
        if (!r->is_xfer)
            break;
    pthread_mutex_unlock(&e->mu);
    return r != NULL;
}

/* 有元数据请求时，单次 sftp_read/write 切片收窄到 256KiB：
 * 一个 4MiB 调用在 CPU 争抢下可占线程数百 ms，会把元数据请求的
 * 插空延迟顶爆（实测 >200ms）。无竞争时保持 CHUNK 全流水线。 */
static size_t xfer_call_cap(eng *e)
{
    return meta_pending(e) ? 256 * 1024 : (size_t)CHUNK;
}

static int xfer_step(eng *e, sfreq *r)
{
    xfr *x = r->x;
    int up = (r->op == RQ_UPLOAD);
    ssize_t n;

    switch (r->st) {
    case X_SFTP_INIT:
        /* 本传输独占一个 sftp channel：共享 SFTP 会话上多个在飞流水线
         * 调用会互踩（实测并发写必然死锁），独立 channel 才真并发。
         * sftp_init 内含 channel_open（session 级 open_* 态）→ 独占。 */
        if (e->io_owner && e->io_owner != r)
            return ST_BLK;
        x->sf = libssh2_sftp_init(e->hp->sess);
        if (!x->sf) {
            if (libssh2_session_last_errno(e->hp->sess)
                == LIBSSH2_ERROR_EAGAIN) {
                io_take(e, r);
                return ST_BLK;
            }
            return xfer_fail(e, r, last_err(e, e->sf), NULL);
        }
        io_rel(e, r);
        r->st = X_LSTAT_SRC;
        return ST_WORK;

    case X_LSTAT_SRC:
        if (up) {
            struct stat st;
            if (lstat(r->a, &st) < 0)
                return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                 strerror(errno));
            if (S_ISDIR(st.st_mode)) {
                x->walking = true;
                emit_prog(e, r, 1);
                r->wsub = 0;            /* 0=探远端根 1=本地遍历完 */
                r->st = X_WALK;
                return ST_WORK;
            }
            if (xent_push(&x->files, &x->nf, &x->fcap, r->a, r->b,
                          &st, NULL))
                return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            x->total = x->files[0].size;
            x->files_total = 1;
            r->st = X_WALK;
            r->wsub = 10;               /* 单文件：探远端根是否目录 */
            return ST_WORK;
        }
        /* 下载：根用 STAT（跟随符号链接） */
        if (ctl_do(e, r, CTL_STAT, r->a, NULL, 0, NULL, NULL) == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc < 0)
            return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
        if ((e->ctl.att.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
            LIBSSH2_SFTP_S_ISDIR(e->ctl.att.permissions)) {
            x->walking = true;
            emit_prog(e, r, 1);
            r->st = X_WALK;
            r->wsub = 20;               /* 远端遍历：20=opendir 21=readdir
                                           22=closedir-pop */
            return ST_WORK;
        }
        if (xent_push(&x->files, &x->nf, &x->fcap, r->b, r->a, NULL,
                      &e->ctl.att))
            return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        x->total = x->files[0].size;
        x->files_total = 1;
        r->st = X_WALK;
        r->wsub = 30;                   /* 单文件：本地根是否目录 */
        return ST_WORK;

    case X_WALK:
        /* ---- upload 子阶段 ---- */
        if (r->wsub == 0) {             /* 探远端根 */
            char *nr;
            if (ctl_do(e, r, CTL_LSTAT, r->b, NULL, 0, NULL, NULL)
                == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc == 0) {
                if ((e->ctl.att.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                    && !LIBSSH2_SFTP_S_ISDIR(e->ctl.att.permissions))
                    return xfer_fail(e, r, HB_SFTP_ERR_EXISTS,
                                     "目标已存在且不是目录");
                nr = path_join(r->b, base_name(r->a));
            } else if (last_err(e, e->ctl.sf) == HB_SFTP_ERR_NOENT) {
                nr = strdup(r->b);
            } else {
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
            }
            if (!nr || xent_push(&x->dirs, &x->nd, &x->dcap, r->a, nr,
                                 NULL, NULL)) {
                free(nr);
                return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            }
            free(nr);
            if (walk_local(x, r->a, x->dirs[0].rp, &r->nskip_link) < 0)
                return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                 strerror(errno));
            x->files_total = (uint32_t)x->nf;
            {
                size_t i;
                x->total = 0;
                for (i = 0; i < x->nf; i++)
                    x->total += x->files[i].size;
            }
            x->walking = false;
            emit_prog(e, r, 1);
            r->st = X_MKDIRS;
            x->di = 0;
            return ST_WORK;
        }
        if (r->wsub == 10) {            /* upload 单文件：远端根目录并入 */
            if (ctl_do(e, r, CTL_LSTAT, r->b, NULL, 0, NULL, NULL)
                == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc == 0 &&
                (e->ctl.att.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                LIBSSH2_SFTP_S_ISDIR(e->ctl.att.permissions)) {
                char *p = path_join(r->b, base_name(r->a));
                if (!p)
                    return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
                free(x->files[0].rp);
                x->files[0].rp = p;
            } else if (e->ctl.rc < 0 &&
                       last_err(e, e->ctl.sf) != HB_SFTP_ERR_NOENT) {
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
            }
            r->st = X_INIT;
            x->fi = 0;
            return ST_WORK;
        }
        /* ---- download 子阶段 ---- */
        if (r->wsub == 20) {            /* opendir 入栈 */
            const char *p = r->pend_child ? r->pend_child : r->a;
            dirstk *f;
            if (ctl_do(e, r, CTL_OPENDIR, p, NULL, 0, NULL, NULL)
                == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc < 0)
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
            f = calloc(1, sizeof *f);
            if (!f)
                return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            f->dh = e->ctl.h;
            f->path = strdup(p);
            if (!r->stk) {
                /* 根：本地根 = b 已是目录 → 并入 basename */
                struct stat st;
                char *lr;
                if (lstat(r->b, &st) == 0 && S_ISDIR(st.st_mode))
                    lr = path_join(r->b, base_name(r->a));
                else
                    lr = strdup(r->b);
                if (!lr || xent_push(&x->dirs, &x->nd, &x->dcap, lr, p,
                                     NULL, NULL)) {
                    free(lr);
                    free(f->path);
                    free(f);
                    return xfer_fail(e, r, HB_SFTP_ERR_OTHER,
                                     "内存不足");
                }
                f->lpath = lr;          /* dirs[0].lp 持有同一字符串副本 */
            } else {
                f->lpath = r->pend_lpath;
                r->pend_lpath = NULL;
            }
            free(r->pend_child);
            r->pend_child = NULL;
            if (!f->path || !f->lpath) {
                free(f->path);
                free(f->lpath);
                free(f);
                return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            }
            f->up = r->stk;
            r->stk = f;
            r->wsub = 21;
            return ST_WORK;
        }
        if (r->wsub == 21) {            /* readdir 栈顶 */
            char *rp, *lp;
            if (ctl_do(e, r, CTL_READDIR, NULL, NULL, 0, r->stk->dh,
                       NULL) == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc < 0)
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
            if (e->ctl.rc == 0) {
                r->wsub = 22;
                return ST_WORK;
            }
            /* 恶意服务器目录穿越防护：非法名（'/'、'..'、内嵌 NUL、
             * 控制符）跳过计数；符号链接不跟随（防逃出目标树/防环）。 */
            if (!hb__sftp_name_ok(e->ctl.nbuf, (size_t)e->ctl.rc)) {
                if (strcmp(e->ctl.nbuf, ".") && strcmp(e->ctl.nbuf, ".."))
                    r->nskip_bad++;
                return ST_WORK;
            }
            if ((e->ctl.att.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                LIBSSH2_SFTP_S_ISLNK(e->ctl.att.permissions)) {
                r->nskip_link++;
                return ST_WORK;
            }
            rp = path_join(r->stk->path, e->ctl.nbuf);
            lp = path_join(r->stk->lpath, e->ctl.nbuf);
            if (!rp || !lp) {
                free(rp);
                free(lp);
                return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            }
            if ((e->ctl.att.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                LIBSSH2_SFTP_S_ISDIR(e->ctl.att.permissions)) {
                if (xent_push(&x->dirs, &x->nd, &x->dcap, lp, rp, NULL,
                              &e->ctl.att)) {
                    free(rp);
                    free(lp);
                    return xfer_fail(e, r, HB_SFTP_ERR_OTHER,
                                     "内存不足");
                }
                free(r->pend_child);
                r->pend_child = rp;
                free(r->pend_lpath);
                r->pend_lpath = lp;
                r->wsub = 20;           /* 下钻 */
                return ST_WORK;
            }
            if (xent_push(&x->files, &x->nf, &x->fcap, lp, rp, NULL,
                          &e->ctl.att)) {
                free(rp);
                free(lp);
                return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            }
            x->total += x->files[x->nf - 1].size;
            x->files_total = (uint32_t)x->nf;
            free(rp);
            free(lp);
            return ST_WORK;
        }
        if (r->wsub == 22) {            /* closedir + pop */
            if (ctl_do(e, r, CTL_CLOSE, NULL, NULL, 0, r->stk->dh,
                       NULL) == ST_BLK)
                return ST_BLK;
            stk_pop(r);
            if (r->stk) {
                r->wsub = 21;
                return ST_WORK;
            }
            x->files_total = (uint32_t)x->nf;
            x->walking = false;
            emit_prog(e, r, 1);
            r->st = X_MKDIRS;
            x->di = 0;
            return ST_WORK;
        }
        if (r->wsub == 30) {            /* download 单文件：本地目录并入 */
            struct stat st;
            if (lstat(r->b, &st) == 0 && S_ISDIR(st.st_mode)) {
                char *p = path_join(r->b, base_name(r->a));
                if (!p)
                    return xfer_fail(e, r, HB_SFTP_ERR_OTHER,
                                     "内存不足");
                free(x->files[0].lp);
                x->files[0].lp = p;
            }
            r->st = X_INIT;
            x->fi = 0;
            return ST_WORK;
        }
        return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内部状态错误");

    case X_MKDIRS:              /* 先建完所有目录再传文件 */
        if (x->di >= x->nd) {
            r->st = X_INIT;
            x->fi = 0;
            return ST_WORK;
        }
        if (up) {
            xent *d = &x->dirs[x->di];
            if (ctl_do(e, r, CTL_MKDIR, d->rp, NULL,
                       (long)(d->mode ? (d->mode & 0777) : 0755),
                       NULL, NULL) == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc < 0 && last_err(e, e->ctl.sf) != HB_SFTP_ERR_EXISTS)
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
        } else {
            xent *d = &x->dirs[x->di];
            /* 纵深防御：目标必须仍在下载根内（名字已校验，双保险） */
            if (!hb__sftp_path_under(x->dirs[0].lp, d->lp)) {
                r->nskip_bad++;
                x->di++;
                return ST_WORK;
            }
            if (mkdir(d->lp, d->mode ? (mode_t)(d->mode & 0777)
                                     : (mode_t)0755) < 0 &&
                errno != EEXIST)
                return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                 strerror(errno));
            if (d->mode)
                chmod(d->lp, (mode_t)(d->mode & 0777));
        }
        x->di++;
        return ST_WORK;

    case X_INIT: {
        xent *f = &x->files[x->fi];
        x->ren_n = 0;
        x->fin_exists = false;
        free(x->base);
        free(x->fin);
        x->base = strdup(up ? f->rp : f->lp);
        x->fin = strdup(up ? f->rp : f->lp);
        x->cur_size = f->size;
        x->cur_mtime = f->mtime;
        x->cur_mode = f->mode;
        if (!x->base || !x->fin)
            return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
        /* 纵深防御：递归下载时落盘目标必须在下载根内（名字早已校验，
         * 这里再拦一次防服务器构造的怪路径）。 */
        if (!up && x->nd > 0 &&
            !hb__sftp_path_under(x->dirs[0].lp, x->fin)) {
            r->nskip_bad++;
            x->files_done++;
            return xfer_next(r);
        }
        r->st = X_PROBE;
        return ST_WORK;
    }

    case X_PROBE:
        if (up) {
            if (ctl_do(e, r, CTL_LSTAT, x->fin, NULL, 0, NULL, NULL)
                == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc == 0)
                goto probe_hit;
            if (last_err(e, e->ctl.sf) == HB_SFTP_ERR_NOENT) {
                r->st = X_OPEN_L;
                return ST_WORK;
            }
            return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
        }
        {
            struct stat st;
            if (lstat(x->fin, &st) == 0)
                goto probe_hit;
            if (errno == ENOENT || errno == ENOTDIR) {
                r->st = X_OPEN_L;
                return ST_WORK;
            }
            return xfer_fail(e, r, HB_SFTP_ERR_LOCAL, strerror(errno));
        }

    probe_hit:
        x->fin_exists = true;
        if (r->conf == HB_SFTP_OVERWRITE) {
            r->st = X_OPEN_L;
            return ST_WORK;
        }
        if (r->conf == HB_SFTP_SKIP) {
            x->files_done++;
            if (x->nf == 1) {
                r->dskip = true;
                free(r->dpath);
                r->dpath = strdup(x->base);
                r->dst = HB_SFTP_OK;
                r->st = X_Z_DONE;
                return ST_WORK;
            }
            return xfer_next(r);
        }
        /* RENAME：下一变体 */
        x->ren_n++;
        {
            char *v = ren_variant(x->base, x->ren_n);
            if (!v)
                return xfer_fail(e, r, HB_SFTP_ERR_OTHER, "内存不足");
            free(x->fin);
            x->fin = v;
        }
        r->st = X_PROBE;
        return ST_WORK;

    case X_OPEN_L:
        if (up) {
            x->lf = fopen(x->files[x->fi].lp, "rb");
            if (!x->lf)
                return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                 strerror(errno));
        } else {
            free(x->part);
            x->part = NULL;
            x->lfd = lpart_open(x->fin, &x->part, x->cur_mode);
            if (x->lfd < 0)
                return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                 strerror(errno));
        }
        r->st = X_OPEN_R;
        return ST_WORK;

    case X_OPEN_R:
        if (up) {
            /* part 名只生成一次：OPEN_W EAGAIN 重入会重跑本状态，
             * 重生成会让在飞 open 与收尾 rename 用不同名字。 */
            if (!x->part) {
                x->part = part_name(x->fin);
                if (!x->part)
                    return xfer_fail(e, r, HB_SFTP_ERR_OTHER,
                                     "内存不足");
            }
            if (ctl_do(e, r, CTL_OPEN_W, x->part, NULL, 0600, NULL,
                       NULL) == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc < 0)
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
            x->rh = e->ctl.h;
        } else {
            if (ctl_do(e, r, CTL_OPEN_R, x->files[x->fi].rp, NULL, 0,
                       NULL, NULL) == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc < 0)
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
            x->rh = e->ctl.h;
        }
        x->blen = x->boff = 0;
        r->st = X_RW;
        return ST_WORK;

    case X_RW:
        if (up) {
            if (x->boff >= x->blen) {
                size_t nrd = fread(x->buf, 1, CHUNK, x->lf);
                if (nrd == 0) {
                    if (ferror(x->lf))
                        return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                         strerror(errno));
                    fclose(x->lf);
                    x->lf = NULL;
                    r->st = X_CLOSE_R;
                    return ST_WORK;
                }
                x->blen = nrd;
                x->boff = 0;
            }
            /* EAGAIN 后必须以相同参数重试：libssh2 内部记着已发出的
             * 子请求串，换参数会乱。x->sf 是本传输独占 channel，别的
             * 传输的调用在各自 channel 上互不干扰。 */
            {
            size_t cap = xfer_call_cap(e);
            n = libssh2_sftp_write(x->rh, (const char *)x->buf + x->boff,
                                   x->blen - x->boff > cap
                                       ? cap : x->blen - x->boff);
            }
            if (n == LIBSSH2_ERROR_EAGAIN)
                return ST_BLK;
            if (n < 0)
                return xfer_fail(e, r, last_err(e, x->sf), NULL);
            x->boff += (size_t)n;
            x->done += (uint64_t)n;
            e->h->ka_last_in = hb_now_ms();
            emit_prog(e, r, 0);
            return ST_WORK;
        }
        n = libssh2_sftp_read(x->rh, (char *)x->buf,
                              xfer_call_cap(e));
        if (n == LIBSSH2_ERROR_EAGAIN)
            return ST_BLK;
        if (n < 0)
            return xfer_fail(e, r, last_err(e, x->sf), NULL);
        if (n == 0) {
            r->st = X_CLOSE_R;
            return ST_WORK;
        }
        {
            size_t off = 0;
            while (off < (size_t)n) {
                ssize_t w = write(x->lfd, x->buf + off,
                                  (size_t)n - off);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                     strerror(errno));
                }
                off += (size_t)w;
            }
        }
        x->done += (uint64_t)n;
        e->h->ka_last_in = hb_now_ms();
        emit_prog(e, r, 0);
        return ST_WORK;

    case X_CLOSE_R:
        if (ctl_do(e, r, CTL_CLOSE, NULL, NULL, 0, x->rh, NULL)
            == ST_BLK)
            return ST_BLK;
        x->rh = NULL;
        r->st = X_META;
        return ST_WORK;

    case X_META:
        if (up) {               /* setstat part：权限 + mtime */
            LIBSSH2_SFTP_ATTRIBUTES att;
            memset(&att, 0, sizeof att);
            att.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS |
                        LIBSSH2_SFTP_ATTR_ACMODTIME;
            att.permissions = x->cur_mode & 07777;
            att.mtime = (unsigned long)x->cur_mtime;
            att.atime = (unsigned long)(x->cur_atime ? x->cur_atime
                                                    : x->cur_mtime);
            if (ctl_do(e, r, CTL_SETSTAT, x->part, NULL, 0, NULL, &att)
                == ST_BLK)
                return ST_BLK;
            /* setstat 失败不致命：照常见进位 */
            r->st = x->fin_exists ? X_REN_RM : X_REN;
            return ST_WORK;
        }
        /* 下载：fsync + futimens + close（本地同步） */
        if (fsync(x->lfd) < 0)
            return xfer_fail(e, r, HB_SFTP_ERR_LOCAL, strerror(errno));
        {
            struct timespec ts[2] = {
                { .tv_sec = x->cur_atime ? x->cur_atime : x->cur_mtime,
                  .tv_nsec = 0 },
                { .tv_sec = x->cur_mtime, .tv_nsec = 0 },
            };
            futimens(x->lfd, ts);
        }
        close(x->lfd);
        x->lfd = -1;
        r->st = X_REN;
        return ST_WORK;

    case X_REN_RM:              /* overwrite：先 posix-rename */
        if (x->no_prename) {
            r->st = X_REN_UNLINK;
            return ST_WORK;
        }
        if (ctl_do(e, r, CTL_PRENAME, x->part, x->fin, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        if (e->ctl.rc == 0)
            goto ren_ok;
        {
            hb_sftp_status st = last_err(e, e->ctl.sf);
            if (st == HB_SFTP_ERR_CONN || st == HB_SFTP_ERR_PERM)
                return xfer_fail(e, r, st, NULL);
        }
        x->no_prename = true;         /* 扩展不受支持 → unlink 路线 */
        r->st = X_REN_UNLINK;
        return ST_WORK;

    case X_REN_UNLINK:          /* 删目标后普通 rename */
        if (ctl_do(e, r, CTL_UNLINK, x->fin, NULL, 0, NULL, NULL)
            == ST_BLK)
            return ST_BLK;
        r->st = X_REN;
        return ST_WORK;

    case X_REN:
        if (up) {
            if (ctl_do(e, r, CTL_RENAME, x->part, x->fin, 0, NULL,
                       NULL) == ST_BLK)
                return ST_BLK;
            if (e->ctl.rc < 0)
                return xfer_fail(e, r, last_err(e, e->ctl.sf), NULL);
        } else {
            if (rename(x->part, x->fin) < 0)
                return xfer_fail(e, r, HB_SFTP_ERR_LOCAL,
                                 strerror(errno));
        }
ren_ok:
        free(x->part);
        x->part = NULL;
        x->files_done++;
        xfer_set_path(r);
        emit_prog(e, r, 0);
        return xfer_next(r);

    /* ---------- 取消/出错清理序列 ---------- */
    case X_Z_CLOSE:
        if (r->stk && !e->conn_dead) {
            if (ctl_do(e, r, CTL_CLOSE, NULL, NULL, 0, r->stk->dh,
                       NULL) == ST_BLK)
                return ST_BLK;
            stk_pop(r);
            return ST_WORK;
        }
        if (x->rh && !e->conn_dead) {
            if (ctl_do(e, r, CTL_CLOSE, NULL, NULL, 0, x->rh, NULL)
                == ST_BLK)
                return ST_BLK;
        }
        x->rh = NULL;
        r->st = X_Z_UNLINK;
        return ST_WORK;
    case X_Z_UNLINK:
        if (x->part) {
            if (up) {
                if (!e->conn_dead &&
                    ctl_do(e, r, CTL_UNLINK, x->part, NULL, 0, NULL,
                           NULL) == ST_BLK)
                    return ST_BLK;
            } else {
                unlink(x->part);        /* 本地临时件总能删 */
            }
            free(x->part);
            x->part = NULL;
        }
        if (x->lf) {
            fclose(x->lf);
            x->lf = NULL;
        }
        if (x->lfd >= 0) {
            close(x->lfd);
            x->lfd = -1;
        }
        r->st = X_Z_SFTP;
        return ST_WORK;
    case X_Z_SFTP:
        /* 拆本传输的 sftp channel；含 channel_close → 独占 */
        if (x->sf && !e->conn_dead) {
            if (e->io_owner && e->io_owner != r)
                return ST_BLK;
            if (libssh2_sftp_shutdown(x->sf) == LIBSSH2_ERROR_EAGAIN) {
                io_take(e, r);
                return ST_BLK;
            }
            io_rel(e, r);
            x->sf = NULL;
        }
        r->st = X_Z_DONE;
        return ST_WORK;
    case X_Z_DONE:
        emit_res(e, r, HB_SFTP_R_DONE, r->dst,
                 r->dmsg ? r->dmsg
                         : (r->dst == HB_SFTP_OK ? NULL
                                                 : status_msg(r->dst)));
        return ST_DONE;
    }
    return req_done(e, r, HB_SFTP_ERR_OTHER, NULL);
}

/* ============================ 引擎 ============================ */

static int req_step(eng *e, sfreq *r)
{
    if (r->st == ST_ZOM)
        return step_zombie(e, r);
    switch (r->op) {
    case RQ_REALPATH: return step_realpath(e, r);
    case RQ_LIST:     return step_list(e, r);
    case RQ_STAT:     return step_stat(e, r);
    case RQ_MKDIR:    return step_mkdir(e, r);
    case RQ_CHMOD:    return step_chmod(e, r);
    case RQ_RENAME:   return step_rename(e, r);
    case RQ_REMOVE:   return step_remove(e, r);
    case RQ_UPLOAD:
    case RQ_DOWNLOAD: return xfer_step(e, r);
    }
    return ST_DONE;
}

static void req_cancel(eng *e, sfreq *r)
{
    if (r->is_xfer) {
        r->dst = HB_SFTP_ERR_CANCELLED;
        free(r->dmsg);
        r->dmsg = strdup(status_msg(HB_SFTP_ERR_CANCELLED));
        r->st = X_Z_CLOSE;
    } else {
        req_done(e, r, HB_SFTP_ERR_CANCELLED, NULL);
    }
}

static bool id_in(uint32_t *ids, size_t n, uint32_t id)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (ids[i] == id)
            return true;
    return false;
}

static void id_del(eng *e, uint32_t id)
{
    size_t i;
    for (i = 0; i < e->nci; i++)
        if (e->cancel_ids[i] == id) {
            e->cancel_ids[i] = e->cancel_ids[--e->nci];
            return;
        }
}

/* pending → active：元数据总是晋级；传输受并发槽限制（FIFO 顺序保持） */
static void eng_promote(eng *e)
{
    sfreq **pp, *r;
    int conc;
    pthread_mutex_lock(&e->mu);
    conc = e->conc;
    pp = &e->pend_h;
    while ((r = *pp)) {
        if (id_in(e->cancel_ids, e->nci, r->id)) {
            *pp = r->next;
            id_del(e, r->id);
            emit_res(e, r, HB_SFTP_R_DONE, HB_SFTP_ERR_CANCELLED,
                     "已取消");
            req_free(r);
            continue;
        }
        if (r->is_xfer && e->n_xfer >= conc) {
            pp = &r->next;              /* 让位：后面元数据仍可晋级 */
            continue;
        }
        *pp = r->next;
        r->next = NULL;
        if (e->act_t)
            e->act_t->next = r;
        else
            e->act_h = r;
        e->act_t = r;
        if (r->is_xfer)
            e->n_xfer++;
        /* 不推进 pp：当前位置已是下一个 */
    }
    if (!e->pend_h)
        e->pend_t = NULL;
    pthread_mutex_unlock(&e->mu);
}

/* 一轮：扫 active 每个 step 一次（round-robin）。
 * 返回 1=仍有事可做且有请求刚推进过（无需 poll）… 实际语义：
 *   返回 0 = 全部 ST_BLK / 无活跃 → 上层 poll
 *   返回 1 = 有 ST_WORK/ST_DONE 进展 → 立即再来一轮 */
static int eng_sweep(eng *e)
{
    sfreq **pp = &e->act_h, *r;
    int progressed = 0;
    uint32_t cids[64];
    size_t cni;

    /* 取消集合快照（append/realloc 在 mu 内，裸读会 UAF） */
    pthread_mutex_lock(&e->mu);
    cni = e->nci > 64 ? 64 : e->nci;
    memcpy(cids, e->cancel_ids, cni * sizeof *cids);
    pthread_mutex_unlock(&e->mu);

    /* drain 伪请求：吃空闲期杂散入站包（keepalive 回复等）。
     * 阻塞时不得短路整个 sweep —— 活跃请求（尤其传输）还要继续走。 */
    if (e->draining) {
        if (ctl_do(e, &e->drain, CTL_LSTAT, ".", NULL, 0, NULL, NULL)
            != ST_BLK) {
            e->draining = false;
            if (e->ctl.rc < 0 &&
                last_err(e, e->ctl.sf) == HB_SFTP_ERR_CONN)
                e->conn_dead = true;
            progressed = 1;
        }
    }

    /* 两遍序：元数据请求永远先于传输 step。一个 4MiB read/write
     * 调用在 CPU 争抢下可占线程数百 ms —— 若按链序轮转，LIST 等
     * 元数据会饿死在传输后面（实测插空延迟 >500ms）。 */
    for (int pass = 0; pass < 2; pass++) {
      pp = &e->act_h;
      while ((r = *pp)) {
        int st;
        if (!!r->is_xfer == !pass) {
            pp = &r->next;          /* pass0=非传输, pass1=传输 */
            continue;
        }
        if (id_in(cids, cni, r->id)) {
            /* 取消：在飞 ctl 操作必须先泵完才能释放槽 */
            if (r->ctl_pending) {
                if (ctl_pump(e) == ST_BLK) {
                    pp = &r->next;
                    continue;
                }
                r->ctl_pending = false;
            }
            /* 同理：传输直调在飞也要先驱动到完成 —— 直接换状态会让
             * EAGAIN 的 read/write 永远等不到同参续打（io_owner 卡死） */
            if (e->io_owner == r) {
                st = req_step(e, r);
                if (st == ST_DONE)
                    goto reap;
                if (e->io_owner == r) {
                    pp = &r->next;
                    progressed = (st == ST_WORK) ? 1 : progressed;
                    continue;
                }
            }
            pthread_mutex_lock(&e->mu);
            id_del(e, r->id);
            pthread_mutex_unlock(&e->mu);
            req_cancel(e, r);
            st = req_step(e, r);        /* 立即跑清理序列第一步 */
            if (st == ST_DONE)
                goto reap;
            pp = &r->next;
            progressed = (st == ST_WORK) ? 1 : progressed;
            continue;
        }
        st = req_step(e, r);
        if (st == ST_DONE) {
        reap:
            *pp = r->next;
            if (!e->act_h)
                e->act_t = NULL;
            else if (e->act_t == r)
                /* 收掉的是尾部：新尾 = pp 所属的前驱节点
                 * （否则 act_t 悬空，promote 会把新请求挂进已释放内存） */
                e->act_t = (sfreq *)((char *)pp - offsetof(sfreq, next));
            if (r->is_xfer)
                e->n_xfer--;
            if (e->io_owner == r)
                e->io_owner = NULL;
            ctl_drop(e, r);
            req_free(r);
            progressed = 1;
            continue;
        }
        if (st == ST_WORK)
            progressed = 1;
        pp = &r->next;
      }
    }
    return progressed;
}

/* 断线：全部按 CONN 终态；本地临时件总能删 */
static void eng_abort_all(eng *e, hb_sftp_status st)
{
    sfreq *r;
    while (e->pend_h) {
        r = e->pend_h;
        e->pend_h = r->next;
        emit_res(e, r, HB_SFTP_R_DONE, st, status_msg(st));
        req_free(r);
    }
    e->pend_t = NULL;
    while (e->act_h) {
        r = e->act_h;
        e->act_h = r->next;
        ctl_drop(e, r);
        if (r->x) {
            if (r->x->lfd >= 0) {
                close(r->x->lfd);
                r->x->lfd = -1;
            }
            if (r->x->part && r->op == RQ_DOWNLOAD) {
                unlink(r->x->part);
                free(r->x->part);
                r->x->part = NULL;
            }
            /* 远端 part：断线删不掉 → 残件可留（最终文件绝不留半截） */
        }
        emit_res(e, r, HB_SFTP_R_DONE, st, status_msg(st));
        req_free(r);
    }
    e->act_t = NULL;
    e->n_xfer = 0;
    e->ctl.owner = NULL;
    e->io_owner = NULL;
    e->draining = false;
}

int hb__sftp_run(void *ctx, hb_session *s)
{
    hb_ssh *h = ctx;
    eng *e = h->sftp_eng;
    int cmd_fd = hb__session_cmd_fd(s);
    char junk[64];

    if (!e || !e->sf)
        return -1;
    e->s = s;
    e->h = h;
    e->hp = hb__ssh_target(h);

    for (;;) {
        if (hb__session_cancelled(s))
            e->shutting = true;
        if (e->shutting) {
            /* 标记全部请求取消，走各自清理序列。io_owner 的在飞调用
             * 必须先由它自己的 step 打完，等下一轮再标取消。 */
            sfreq *r;
            for (r = e->act_h; r; r = r->next)
                if (r != e->io_owner && r->st != ST_ZOM &&
                    r->dst != HB_SFTP_ERR_CANCELLED)
                    req_cancel(e, r);
        }
        if (e->conn_dead)
            break;

        eng_promote(e);

        if (eng_sweep(e))
            continue;                   /* 有进展 → 立即下一轮 */

        /* 全 ST_BLK / 空闲 → poll 等 socket 或命令管道 */
        {
            int dirs, tmo, n;
            struct pollfd pf[2];

            if (e->conn_dead)
                continue;
            if (e->shutting && !e->act_h && !e->pend_h && !e->draining)
                break;                  /* 清理完 */

            tmo = hb__ssh_ka_tick(h);
            if (h->disconnected) {
                    e->conn_dead = true;
                continue;
            }
            dirs = libssh2_session_block_directions(e->hp->sess);
            pf[0].fd = cmd_fd;
            pf[0].events = POLLIN;
            pf[0].revents = 0;
            pf[1].fd = e->hp->sock;
            /* POLLIN 恒订：keepalive 回复等杂散入站包随时可能到，
             * 漏订会让 ka_last_in 不刷新、半开检测误判断线，
             * 也会让 drain 机制失去触发机会。 */
            pf[1].events = POLLIN | POLLERR | POLLHUP;
            (void)dirs;
            if (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND)
                pf[1].events |= POLLOUT;
            /* block_directions 只反映最后一个 EAGAIN 调用想等的方向；
             * ctl 槽在飞必然等入站 → 总补 POLLIN。 */
            if (e->ctl.owner)
                pf[1].events |= POLLIN;
            /* 有活跃请求时封顶 50ms：防止漏订方向卡死到 keepalive 周期 */
            if ((e->act_h || e->draining) && (tmo < 0 || tmo > 50))
                tmo = 50;
            if (e->shutting && (tmo < 0 || tmo > 2000))
                tmo = 2000;
            n = poll(pf, 2, tmo);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                e->conn_dead = true;
                continue;
            }
            if (pf[0].revents & POLLIN) {
                ssize_t nr;
                while ((nr = read(cmd_fd, junk, sizeof junk)) > 0)
                    ;
            }
            if (pf[1].revents & POLLIN) {
                h->ka_last_in = hb_now_ms();
                /* drain 伪请求吃掉杂散入站包（keepalive 回复等）。
                 * 避开 ctl 槽占用与独占调用（sftp_init/shutdown）；
                 * 元数据在跑/排队时也得让位 —— drain 每轮占一个 ctl
                 * 槽位会把 LIST 的插空延迟放大（重负载下实测顶爆
                 * 500ms 预算）；传输期间的杂散包本就被各自 read 消耗。 */
                if (!e->ctl.owner && !e->draining && !e->conn_dead &&
                    !e->io_owner && !meta_pending(e)) {
                    memset(&e->drain, 0, sizeof e->drain);
                    e->drain.op = RQ_DRAIN;
                    e->draining = true;
                }
            }
            if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                e->conn_dead = true;
                h->disconnected = true;
            }
        }
        (void)hb__ssh_ka_tick(h);
        if (h->disconnected)
            e->conn_dead = true;
    }

    /* ---- 收尾 ---- */
    if (e->conn_dead) {
        if (!h->disconnect_emitted) {
            h->disconnect_emitted = true;
            hb__ssh_set_err(h, "sftp transport lost");
            hb__ssh_emit_error(h, HB_SSH_ERR_DISCONNECTED, "连接已断开");
        }
        eng_abort_all(e, HB_SFTP_ERR_CONN);
    } else {
        eng_abort_all(e, HB_SFTP_ERR_CANCELLED);
    }

    pthread_mutex_lock(&e->mu);
    e->dead = true;
    e->nci = 0;
    pthread_mutex_unlock(&e->mu);

    /* sftp_shutdown：尽力而为（连接可能已死） */
    if (!e->conn_dead && e->sf) {
        int i;
        for (i = 0; i < 50; i++) {
            int rc = libssh2_sftp_shutdown(e->sf);
            if (rc != LIBSSH2_ERROR_EAGAIN)
                break;
            usleep(10000);
        }
    }
    return e->conn_dead ? -1 : 0;
}

/* ============================ 构造 ============================ */

void *hb__sftp_eng_new(void)
{
    eng *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    pthread_mutex_init(&e->mu, NULL);
    e->conc = DEF_CONC;
    e->drain.op = RQ_DRAIN;
    return e;
}

int hb__sftp_eng_bind(void *eng_, hb_session *s, LIBSSH2_SFTP *sf)
{
    eng *e = eng_;
    if (!e || !s || !sf)
        return -1;
    e->s = s;
    e->sf = sf;
    hb__session_set_udata(s, e);
    return 0;
}

void hb__sftp_eng_free(void *eng_)
{
    eng *e = eng_;
    if (!e)
        return;
    pthread_mutex_lock(&e->mu);
    e->dead = true;
    pthread_mutex_unlock(&e->mu);
    eng_abort_all(e, HB_SFTP_ERR_CANCELLED);
    free(e->cancel_ids);
    pthread_mutex_destroy(&e->mu);
    free(e);
}

/* ============================ 公共 API ============================ */

static uint32_t submit(hb_session *s, int op, const char *a, const char *b,
                       uint32_t mode, bool flag, int conf)
{
    eng *e = s ? hb__session_udata(s) : NULL;
    sfreq *r;
    uint32_t id;
    if (!e || !a)
        return 0;
    r = calloc(1, sizeof *r);
    if (!r)
        return 0;
    r->op = op;
    r->is_xfer = (op == RQ_UPLOAD || op == RQ_DOWNLOAD);
    r->mode = mode;
    r->flag = flag;
    r->conf = conf;
    r->a = strdup(a);
    r->b = b ? strdup(b) : NULL;
    if (!r->a || (b && !r->b))
        goto bad;
    if (r->is_xfer) {
        r->x = calloc(1, sizeof *r->x);
        if (!r->x)
            goto bad;
        r->x->lfd = -1;
        r->x->last_prog = hb_now_ms();
    }
    pthread_mutex_lock(&e->mu);
    if (e->dead) {
        pthread_mutex_unlock(&e->mu);
        goto bad;
    }
    r->id = ++e->next_id;
    if (e->pend_t)
        e->pend_t->next = r;
    else
        e->pend_h = r;
    e->pend_t = r;
    id = r->id;
    pthread_mutex_unlock(&e->mu);
    hb__session_wake(s);
    return id;
bad:
    req_free(r);
    return 0;
}

uint32_t hb_sftp_realpath(hb_session *s, const char *path)
{
    return submit(s, RQ_REALPATH, path, NULL, 0, false, 0);
}

uint32_t hb_sftp_list(hb_session *s, const char *dir)
{
    return submit(s, RQ_LIST, dir, NULL, 0, false, 0);
}

uint32_t hb_sftp_stat(hb_session *s, const char *path)
{
    return submit(s, RQ_STAT, path, NULL, 0, false, 0);
}

uint32_t hb_sftp_mkdir(hb_session *s, const char *path, uint32_t mode)
{
    return submit(s, RQ_MKDIR, path, NULL, mode, false, 0);
}

uint32_t hb_sftp_rename(hb_session *s, const char *from, const char *to,
                       bool overwrite)
{
    if (!to)
        return 0;
    return submit(s, RQ_RENAME, from, to, 0, overwrite, 0);
}

uint32_t hb_sftp_remove(hb_session *s, const char *path, bool recursive)
{
    return submit(s, RQ_REMOVE, path, NULL, 0, recursive, 0);
}

uint32_t hb_sftp_chmod(hb_session *s, const char *path, uint32_t mode)
{
    return submit(s, RQ_CHMOD, path, NULL, mode, false, 0);
}

uint32_t hb_sftp_upload(hb_session *s, const char *local,
                       const char *remote, hb_sftp_conflict c)
{
    if (!remote)
        return 0;
    return submit(s, RQ_UPLOAD, local, remote, 0, false, (int)c);
}

uint32_t hb_sftp_download(hb_session *s, const char *remote,
                         const char *local, hb_sftp_conflict c)
{
    if (!local)
        return 0;
    return submit(s, RQ_DOWNLOAD, remote, local, 0, false, (int)c);
}

void hb_sftp_cancel(hb_session *s, uint32_t req)
{
    eng *e = s ? hb__session_udata(s) : NULL;
    uint32_t *ni;
    if (!e || !req)
        return;
    pthread_mutex_lock(&e->mu);
    if (e->nci == e->ccicap) {
        ni = realloc(e->cancel_ids,
                     (e->ccicap ? e->ccicap * 2 : 16) * sizeof *ni);
        if (!ni) {
            pthread_mutex_unlock(&e->mu);
            return;
        }
        e->cancel_ids = ni;
        e->ccicap = e->ccicap ? e->ccicap * 2 : 16;
    }
    e->cancel_ids[e->nci++] = req;
    pthread_mutex_unlock(&e->mu);
    hb__session_wake(s);
}

void hb_sftp_set_concurrency(hb_session *s, int n)
{
    eng *e = s ? hb__session_udata(s) : NULL;
    if (!e)
        return;
    if (n < 1)
        n = 1;
    if (n > MAX_CONC)
        n = MAX_CONC;
    pthread_mutex_lock(&e->mu);
    e->conc = n;
    pthread_mutex_unlock(&e->mu);
}
