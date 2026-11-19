#include "hb-store.h"

#include <sodium.h>
#include <libsecret/secret.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

struct _HbStore {
    GObject parent;
    hb_vault *vault;
    char     *data_dir;
    gboolean  dek_fallback;
};

G_DEFINE_TYPE(HbStore, hb_store, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

/* ------------------------------- DEK -------------------------------- */

static const SecretSchema VAULT_SCHEMA = {
    "dev.harbor.Harbor.VaultKey", SECRET_SCHEMA_NONE,
    {
        { "purpose", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "NULL", 0 },
    },
    0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

/* 生成新 DEK；尝试 Secret Service，失败/不可用时退到 vault.key 文件。 */
static gboolean load_dek(HbStore *st, uint8_t dek[32])
{
    GError *err = NULL;
    gchar *b64;

    /* 1. Secret Service 读取 */
    b64 = secret_password_lookup_sync(&VAULT_SCHEMA, NULL, &err,
                                      "purpose", "vault-dek", NULL);
    if (b64) {
        gsize len = 0;
        guchar *raw = g_base64_decode(b64, &len);
        secret_password_free(b64);
        if (len == 32) {
            memcpy(dek, raw, 32);
            sodium_memzero(raw, len);
            g_free(raw);
            return TRUE;
        }
        sodium_memzero(raw, len);
        g_free(raw);
    }
    if (err) {          /* 服务不可用 → 走文件 */
        g_error_free(err);
        err = NULL;
    }

    /* 2. vault.key 文件 */
    {
        char *path = g_build_filename(st->data_dir, "vault.key", NULL);
        gchar *raw = NULL;
        gsize len = 0;
        gboolean ok = g_file_get_contents(path, &raw, &len, NULL);
        if (ok && len == 32) {
            memcpy(dek, raw, 32);
            sodium_memzero(raw, len);
            g_free(raw);
            g_free(path);
            st->dek_fallback = TRUE;
            return TRUE;
        }
        if (raw) { sodium_memzero(raw, len); g_free(raw); }
        g_free(path);
    }

    /* 3. 首次运行：生成新 DEK，优先存 Secret Service */
    randombytes_buf(dek, 32);
    b64 = g_base64_encode(dek, 32);
    if (secret_password_store_sync(&VAULT_SCHEMA, SECRET_COLLECTION_DEFAULT,
                                   "Harbor vault key", b64,
                                   NULL, &err,
                                   "purpose", "vault-dek", NULL)) {
        secret_password_free(b64);
        return TRUE;
    }
    if (err) { g_error_free(err); err = NULL; }
    secret_password_free(b64);

    /* 文件兜底：raw 32 字节，0600 */
    {
        char *path = g_build_filename(st->data_dir, "vault.key", NULL);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        gboolean ok = fd >= 0;
        if (ok) {
            ok = write(fd, dek, 32) == 32;
            close(fd);
        }
        g_free(path);
        if (ok) {
            st->dek_fallback = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------ 种子 -------------------------------- */

static hb_obj *seed_host(HbStore *st, const char *name, const char *proto,
                         const char *os, const char *user, const char *addr,
                         int64_t port, const char *group, const char *via,
                         int64_t order)
{
    hb_obj *o = hb_obj_new("host");
    hb_obj_set_str(o, "name", name);
    hb_obj_set_str(o, "proto", proto);
    if (os)    hb_obj_set_str(o, "os", os);
    if (user)  hb_obj_set_str(o, "user", user);
    if (addr)  hb_obj_set_str(o, "address", addr);
    if (port)  hb_obj_set_int(o, "port", port);
    if (group) hb_obj_set_str(o, "group", group);
    if (via)   hb_obj_set_str(o, "via", via);
    hb_obj_set_int(o, "order", order);
    hb_vault_put(st->vault, o);
    return o;
}

static void seed_demo(HbStore *st)
{
    hb_obj *h, *h1, *h3;
    /* 原型 demo：h1/h3 为对象 id —— h1=10.10.21.84, h3=10.87.102.179 */
    h1 = seed_host(st, "10.10.21.84", "ssh", "ubuntu", "x",
                   "10.10.21.84", 22, "prod", NULL, 1);
    h = seed_host(st, "10.88.2.11", "ssh", "ubuntu", "nian",
                  "10.88.2.11", 22, "prod", NULL, 2);
    hb_obj_free(h);
    h3 = seed_host(st, "10.87.102.179", "ssh", "generic", "root",
                   "10.87.102.179", 22, "lab", NULL, 3);
    h = seed_host(st, "home-win (RDP)", "rdp", "windows", "nian",
                  "127.0.0.1", 3389, "lab", hb_obj_id(h3), 4);
    hb_obj_set_bool(h, "tunnel", TRUE);
    hb_vault_put(st->vault, h);
    hb_obj_free(h);
    h = seed_host(st, "lab-vnc (VNC)", "vnc", NULL, "",
                  "127.0.0.1", 5901, "lab", hb_obj_id(h1), 5);
    hb_obj_set_bool(h, "tunnel", TRUE);
    hb_vault_put(st->vault, h);
    hb_obj_free(h);
    hb_obj_free(h1);
    hb_obj_free(h3);
}

/* 首次运行：空 vault → Local 主机（+ HARBOR_DEMO=1 时的 demo 种子） */
static void first_run_seed(HbStore *st)
{
    hb_obj **hosts = NULL;
    size_t n = 0;

    if (hb_vault_list(st->vault, "host", &hosts, &n) != HB_OK || n > 0) {
        if (hosts) hb_obj_list_free(hosts, n);
        return;
    }
    hb_obj_list_free(hosts, n);

    {
        hb_obj *o = seed_host(st, "Local", "local", "local",
                              g_get_user_name(), "127.0.0.1", 0, NULL,
                              NULL, 0);
        hb_obj_free(o);
    }
    if (g_strcmp0(g_getenv("HARBOR_DEMO"), "1") == 0)
        seed_demo(st);
}

/* ------------------------------ GObject ------------------------------ */

static void vault_changed(hb_vault *v, const char *type, const char *id,
                          void *ud)
{
    (void)v;
    g_signal_emit(ud, sigs[SIG_CHANGED], 0, type, id);
}

static void hb_store_finalize(GObject *o)
{
    HbStore *st = HB_STORE(o);
    if (st->vault) hb_vault_close(st->vault);
    g_free(st->data_dir);
    G_OBJECT_CLASS(hb_store_parent_class)->finalize(o);
}

static void hb_store_class_init(HbStoreClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->finalize = hb_store_finalize;
    sigs[SIG_CHANGED] = g_signal_new("changed", HB_TYPE_STORE,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void hb_store_init(HbStore *st)
{
    const char *env = g_getenv("HARBOR_DATA_DIR");
    char *vault_path;
    uint8_t dek[32];
    hb_err rc;

    st->data_dir = env && *env
        ? g_strdup(env)
        : g_build_filename(g_get_user_data_dir(), "harbor", NULL);
    if (g_mkdir_with_parents(st->data_dir, 0700) != 0)
        g_warning("hb-store: mkdir %s failed: %m", st->data_dir);
    chmod(st->data_dir, 0700);

    hb_core_init();     /* sodium_init（幂等） */

    vault_path = g_build_filename(st->data_dir, "vault.db", NULL);
    if (!load_dek(st, dek)) {
        g_free(vault_path);
        g_warning("hb-store: cannot provision vault key");
        sodium_memzero(dek, 32);
        return;
    }
    rc = hb_vault_open(vault_path, dek, &st->vault);
    sodium_memzero(dek, sizeof dek);
    g_free(vault_path);
    if (rc != HB_OK) {
        g_warning("hb-store: vault open failed: %d", rc);
        return;
    }
    hb_vault_set_change_cb(st->vault, vault_changed, st);
    first_run_seed(st);
}

HbStore *hb_store_default(void)
{
    static HbStore *singleton;
    if (!singleton)
        singleton = g_object_new(HB_TYPE_STORE, NULL);
    return singleton;
}

hb_vault *hb_store_vault(HbStore *st) { return st->vault; }
gboolean  hb_store_dek_fallback(HbStore *st) { return st->dek_fallback; }
const char *hb_store_data_dir(HbStore *st) { return st->data_dir; }

hb_obj **hb_store_list(HbStore *st, const char *type, size_t *n_out)
{
    hb_obj **out = NULL;
    *n_out = 0;
    if (!st->vault ||
        hb_vault_list(st->vault, type, &out, n_out) != HB_OK)
        return NULL;
    return out;
}
