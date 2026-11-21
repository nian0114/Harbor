#include "srv.h"

#include <string.h>

#define HB_SRV_OK        0
#define HB_SRV_NOT_FOUND 1

/* ============================ schema ============================ */

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS accounts("
    " id TEXT PRIMARY KEY,"
    " email TEXT UNIQUE NOT NULL,"
    " auth_hash TEXT NOT NULL,"
    " kdf_salt TEXT NOT NULL, kdf_ops INTEGER NOT NULL,"
    " kdf_mem INTEGER NOT NULL,"
    " wrapped_vk_pw TEXT NOT NULL,"
    " rc_hash TEXT NOT NULL, wrapped_vk_rc TEXT NOT NULL,"
    " totp_secret BLOB, totp_enabled INTEGER NOT NULL DEFAULT 0,"
    " totp_last_step INTEGER NOT NULL DEFAULT -1,"
    " fail_count INTEGER NOT NULL DEFAULT 0,"
    " locked_until INTEGER NOT NULL DEFAULT 0,"
    " created INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS devices("
    " id TEXT PRIMARY KEY, account_id TEXT NOT NULL,"
    " name TEXT, platform TEXT,"
    " created INTEGER NOT NULL, last_seen INTEGER NOT NULL DEFAULT 0,"
    " revoked INTEGER NOT NULL DEFAULT 0, wipe INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS tokens("
    " hash TEXT PRIMARY KEY, device_id TEXT NOT NULL,"
    " account_id TEXT NOT NULL, created INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS objects("
    " account_id TEXT NOT NULL, id TEXT NOT NULL, type TEXT NOT NULL,"
    " seq INTEGER NOT NULL, blob BLOB NOT NULL, updated_at INTEGER NOT NULL,"
    " PRIMARY KEY(account_id, id));"
    "CREATE INDEX IF NOT EXISTS objects_seq ON objects(account_id, seq);"
    "CREATE TABLE IF NOT EXISTS account_seq("
    " account_id TEXT PRIMARY KEY, last_seq INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS recovery("
    " hash TEXT PRIMARY KEY, account_id TEXT NOT NULL,"
    " expires INTEGER NOT NULL);"
    "PRAGMA user_version=1;";

sqlite3 *hbd_open(const char *dir, GError **err)
{
    g_autofree char *path = g_build_filename(dir, "harbor-server.db", NULL);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "sqlite open %s: %s", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "schema: %s",
                    sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    return st;
}

/* ============================ accounts ============================ */

int hbd_acct_by_email(sqlite3 *db, const char *email, HbAcct **out)
{
    sqlite3_stmt *st = prep(
        db, "SELECT id,email,auth_hash,kdf_salt,kdf_ops,kdf_mem,wrapped_vk_pw,"
            "rc_hash,wrapped_vk_rc,totp_secret,totp_enabled,totp_last_step,"
            "fail_count,locked_until,created FROM accounts WHERE email=?");
    HbAcct *a;
    int rc;
    sqlite3_bind_text(st, 1, email, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return HB_SRV_NOT_FOUND;
    }
    a = g_new0(HbAcct, 1);
    a->id = g_strdup((const char *)sqlite3_column_text(st, 0));
    a->email = g_strdup((const char *)sqlite3_column_text(st, 1));
    a->auth_hash = g_strdup((const char *)sqlite3_column_text(st, 2));
    a->kdf_salt = g_strdup((const char *)sqlite3_column_text(st, 3));
    a->kdf_ops = sqlite3_column_int64(st, 4);
    a->kdf_mem = sqlite3_column_int64(st, 5);
    a->wrapped_vk_pw = g_strdup((const char *)sqlite3_column_text(st, 6));
    a->rc_hash = g_strdup((const char *)sqlite3_column_text(st, 7));
    a->wrapped_vk_rc = g_strdup((const char *)sqlite3_column_text(st, 8));
    if (sqlite3_column_bytes(st, 9) == 20) {
        memcpy(a->totp_secret, sqlite3_column_blob(st, 9), 20);
        a->totp_pending = true;
    }
    a->totp_enabled = sqlite3_column_int(st, 10) != 0;
    a->totp_last_step = sqlite3_column_int64(st, 11);
    a->fail_count = sqlite3_column_int(st, 12);
    a->locked_until = sqlite3_column_int64(st, 13);
    a->created = sqlite3_column_int64(st, 14);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    *out = a;
    (void)rc;
    return 0;
}

int hbd_acct_by_id(sqlite3 *db, const char *id, HbAcct **out)
{
    sqlite3_stmt *st = prep(
        db, "SELECT id,email,auth_hash,kdf_salt,kdf_ops,kdf_mem,wrapped_vk_pw,"
            "rc_hash,wrapped_vk_rc,totp_secret,totp_enabled,totp_last_step,"
            "fail_count,locked_until,created FROM accounts WHERE id=?");
    HbAcct *a;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return HB_SRV_NOT_FOUND;
    }
    a = g_new0(HbAcct, 1);
    a->id = g_strdup((const char *)sqlite3_column_text(st, 0));
    a->email = g_strdup((const char *)sqlite3_column_text(st, 1));
    a->auth_hash = g_strdup((const char *)sqlite3_column_text(st, 2));
    a->kdf_salt = g_strdup((const char *)sqlite3_column_text(st, 3));
    a->kdf_ops = sqlite3_column_int64(st, 4);
    a->kdf_mem = sqlite3_column_int64(st, 5);
    a->wrapped_vk_pw = g_strdup((const char *)sqlite3_column_text(st, 6));
    a->rc_hash = g_strdup((const char *)sqlite3_column_text(st, 7));
    a->wrapped_vk_rc = g_strdup((const char *)sqlite3_column_text(st, 8));
    if (sqlite3_column_bytes(st, 9) == 20) {
        memcpy(a->totp_secret, sqlite3_column_blob(st, 9), 20);
        a->totp_pending = true;
    }
    a->totp_enabled = sqlite3_column_int(st, 10) != 0;
    a->totp_last_step = sqlite3_column_int64(st, 11);
    a->fail_count = sqlite3_column_int(st, 12);
    a->locked_until = sqlite3_column_int64(st, 13);
    a->created = sqlite3_column_int64(st, 14);
    sqlite3_finalize(st);
    *out = a;
    return 0;
}

void hbd_acct_free(HbAcct *a)
{
    if (!a) return;
    g_free(a->id); g_free(a->email); g_free(a->auth_hash);
    g_free(a->kdf_salt); g_free(a->wrapped_vk_pw);
    g_free(a->rc_hash); g_free(a->wrapped_vk_rc);
    sodium_memzero(a->totp_secret, sizeof a->totp_secret);
    g_free(a);
}

int hbd_acct_insert(sqlite3 *db, const HbAcct *a)
{
    sqlite3_stmt *st = prep(
        db, "INSERT INTO accounts(id,email,auth_hash,kdf_salt,kdf_ops,kdf_mem,"
            "wrapped_vk_pw,rc_hash,wrapped_vk_rc,created)"
            " VALUES(?,?,?,?,?,?,?,?,?,?)");
    int rc;
    sqlite3_bind_text(st, 1, a->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, a->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, a->auth_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, a->kdf_salt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, a->kdf_ops);
    sqlite3_bind_int64(st, 6, a->kdf_mem);
    sqlite3_bind_text(st, 7, a->wrapped_vk_pw, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, a->rc_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, a->wrapped_vk_rc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 10, a->created);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int hbd_acct_set_lock(sqlite3 *db, const char *id, int fail, int64_t until)
{
    sqlite3_stmt *st = prep(
        db, "UPDATE accounts SET fail_count=?,locked_until=? WHERE id=?");
    int rc;
    sqlite3_bind_int(st, 1, fail);
    sqlite3_bind_int64(st, 2, until);
    sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int hbd_acct_set_auth(sqlite3 *db, const char *id, const char *auth_hash,
                    const char *salt, int64_t ops, int64_t mem,
                    const char *wrapped_vk_pw)
{
    sqlite3_stmt *st = prep(
        db, "UPDATE accounts SET auth_hash=?,kdf_salt=?,kdf_ops=?,kdf_mem=?,"
            "wrapped_vk_pw=? WHERE id=?");
    int rc;
    sqlite3_bind_text(st, 1, auth_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, salt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, ops);
    sqlite3_bind_int64(st, 4, mem);
    sqlite3_bind_text(st, 5, wrapped_vk_pw, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int hbd_acct_set_totp(sqlite3 *db, const char *id, const uint8_t secret[20],
                    bool pending, bool enabled, int64_t last_step)
{
    sqlite3_stmt *st = prep(
        db, "UPDATE accounts SET totp_secret=?,totp_enabled=?,"
            "totp_last_step=? WHERE id=?");
    int rc;
    if (pending)
        sqlite3_bind_blob(st, 1, secret, 20, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 1);
    sqlite3_bind_int(st, 2, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 3, last_step);
    sqlite3_bind_text(st, 4, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ============================ devices ============================ */

int hbd_device_add(sqlite3 *db, const char *id, const char *acct,
                   const char *name, const char *platform)
{
    sqlite3_stmt *st = prep(
        db, "INSERT INTO devices(id,account_id,name,platform,created,last_seen)"
            " VALUES(?,?,?,?,?,?)");
    int64_t now = hbu_now_ms();
    int rc;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, platform, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_int64(st, 6, now);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int hbd_device_by_id(sqlite3 *db, const char *id, char **acct_out,
                     bool *revoked, bool *wipe)
{
    sqlite3_stmt *st = prep(
        db, "SELECT account_id,revoked,wipe FROM devices WHERE id=?");
    int found = HB_SRV_NOT_FOUND;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (acct_out)
            *acct_out = g_strdup((const char *)sqlite3_column_text(st, 0));
        if (revoked) *revoked = sqlite3_column_int(st, 1) != 0;
        if (wipe) *wipe = sqlite3_column_int(st, 2) != 0;
        found = 0;
    }
    sqlite3_finalize(st);
    return found;
}

GArray *hbd_devices_list(sqlite3 *db, const char *acct)
{
    sqlite3_stmt *st = prep(
        db, "SELECT id,name,platform,created,last_seen FROM devices"
            " WHERE account_id=? AND revoked=0 ORDER BY created");
    GArray *arr = g_array_new(FALSE, TRUE, sizeof(HbDevice));
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        HbDevice d = {0};
        d.id = g_strdup((const char *)sqlite3_column_text(st, 0));
        d.name = g_strdup((const char *)sqlite3_column_text(st, 1));
        d.platform = g_strdup((const char *)sqlite3_column_text(st, 2));
        d.created = sqlite3_column_int64(st, 3);
        d.last_seen = sqlite3_column_int64(st, 4);
        g_array_append_val(arr, d);
    }
    sqlite3_finalize(st);
    return arr;
}

void hbd_devices_free(GArray *arr)
{
    if (!arr) return;
    for (guint i = 0; i < arr->len; i++) {
        HbDevice *d = &g_array_index(arr, HbDevice, i);
        g_free(d->id); g_free(d->name); g_free(d->platform);
    }
    g_array_free(arr, TRUE);
}

int hbd_device_revoke(sqlite3 *db, const char *acct, const char *id, bool wipe)
{
    sqlite3_stmt *st = prep(
        db, "UPDATE devices SET revoked=1,wipe=? WHERE account_id=? AND id=?");
    int rc, changed;
    sqlite3_bind_int(st, 1, wipe ? 1 : 0);
    sqlite3_bind_text(st, 2, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    changed = sqlite3_changes(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || changed == 0) return HB_SRV_NOT_FOUND;
    return 0;
}

int hbd_device_touch(sqlite3 *db, const char *id, int64_t now_ms)
{
    sqlite3_stmt *st = prep(
        db, "UPDATE devices SET last_seen=? WHERE id=? AND last_seen<?");
    int rc;
    sqlite3_bind_int64(st, 1, now_ms);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now_ms - 60000); /* 60 s 节流 */
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ============================ tokens ============================ */

int hbd_token_add(sqlite3 *db, const char *hash, const char *device,
                  const char *acct)
{
    sqlite3_stmt *st = prep(
        db, "INSERT INTO tokens(hash,device_id,account_id,created)"
            " VALUES(?,?,?,?)");
    int rc;
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, device, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, hbu_now_ms());
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int hbd_token_lookup(sqlite3 *db, const char *hash, char **device_out,
                     char **acct_out)
{
    sqlite3_stmt *st = prep(
        db, "SELECT device_id,account_id FROM tokens WHERE hash=?");
    int found = HB_SRV_NOT_FOUND;
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *device_out = g_strdup((const char *)sqlite3_column_text(st, 0));
        *acct_out = g_strdup((const char *)sqlite3_column_text(st, 1));
        found = 0;
    }
    sqlite3_finalize(st);
    return found;
}

int hbd_recovery_add(sqlite3 *db, const char *hash, const char *acct,
                     int64_t expires)
{
    sqlite3_stmt *st = prep(
        db, "INSERT INTO recovery(hash,account_id,expires) VALUES(?,?,?)");
    int rc;
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, expires);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int hbd_recovery_lookup(sqlite3 *db, const char *hash, char **acct_out)
{
    sqlite3_stmt *st = prep(
        db, "SELECT account_id,expires FROM recovery WHERE hash=?");
    int found = HB_SRV_NOT_FOUND;
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (sqlite3_column_int64(st, 1) > hbu_now_ms()) {
            *acct_out = g_strdup((const char *)sqlite3_column_text(st, 0));
            found = 0;
        }
    }
    sqlite3_finalize(st);
    return found;
}

int hbd_recovery_del(sqlite3 *db, const char *hash)
{
    sqlite3_stmt *st = prep(db, "DELETE FROM recovery WHERE hash=?");
    int rc;
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ============================ objects ============================ */

GArray *hbd_objects_since(sqlite3 *db, const char *acct, int64_t since,
                          int64_t limit)
{
    sqlite3_stmt *st = prep(
        db, "SELECT id,type,seq,blob,updated_at FROM objects"
            " WHERE account_id=? AND seq>? ORDER BY seq LIMIT ?");
    GArray *arr = g_array_new(FALSE, TRUE, sizeof(HbObject));
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, since);
    sqlite3_bind_int64(st, 3, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        HbObject o = {0};
        o.id = g_strdup((const char *)sqlite3_column_text(st, 0));
        o.type = g_strdup((const char *)sqlite3_column_text(st, 1));
        o.seq = sqlite3_column_int64(st, 2);
        o.blob_n = sqlite3_column_bytes(st, 3);
        o.blob = g_memdup2(sqlite3_column_blob(st, 3), o.blob_n);
        o.updated_at = sqlite3_column_int64(st, 4);
        g_array_append_val(arr, o);
    }
    sqlite3_finalize(st);
    return arr;
}

void hbd_objects_free(GArray *arr)
{
    if (!arr) return;
    for (guint i = 0; i < arr->len; i++) {
        HbObject *o = &g_array_index(arr, HbObject, i);
        g_free(o->id); g_free(o->type); g_free(o->blob);
    }
    g_array_free(arr, TRUE);
}

int64_t hbd_object_seq(sqlite3 *db, const char *acct, const char *id)
{
    sqlite3_stmt *st = prep(
        db, "SELECT seq FROM objects WHERE account_id=? AND id=?");
    int64_t seq = -1;
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) seq = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return seq;
}

int hbd_object_get(sqlite3 *db, const char *acct, const char *id,
                   HbObject *out)
{
    sqlite3_stmt *st = prep(
        db, "SELECT id,type,seq,blob,updated_at FROM objects"
            " WHERE account_id=? AND id=?");
    int found = HB_SRV_NOT_FOUND;
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        memset(out, 0, sizeof *out);
        out->id = g_strdup((const char *)sqlite3_column_text(st, 0));
        out->type = g_strdup((const char *)sqlite3_column_text(st, 1));
        out->seq = sqlite3_column_int64(st, 2);
        out->blob_n = sqlite3_column_bytes(st, 3);
        out->blob = g_memdup2(sqlite3_column_blob(st, 3), out->blob_n);
        out->updated_at = sqlite3_column_int64(st, 4);
        found = 0;
    }
    sqlite3_finalize(st);
    return found;
}

int64_t hbd_seq_next(sqlite3 *db, const char *acct)
{
    sqlite3_stmt *st;
    int64_t seq = -1;
    /* 调用方必须在事务内 */
    st = prep(db, "INSERT OR IGNORE INTO account_seq(account_id,last_seq)"
                  " VALUES(?,0)");
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    st = prep(db, "UPDATE account_seq SET last_seq=last_seq+1"
                  " WHERE account_id=?");
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    st = prep(db, "SELECT last_seq FROM account_seq WHERE account_id=?");
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) seq = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return seq;
}

int hbd_object_put(sqlite3 *db, const char *acct, const char *id,
                   const char *type, int64_t seq, const uint8_t *blob,
                   size_t bn, int64_t updated_at)
{
    sqlite3_stmt *st = prep(
        db, "INSERT INTO objects(account_id,id,type,seq,blob,updated_at)"
            " VALUES(?,?,?,?,?,?)"
            " ON CONFLICT(account_id,id) DO UPDATE SET"
            " type=excluded.type,seq=excluded.seq,blob=excluded.blob,"
            " updated_at=excluded.updated_at");
    int rc;
    sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, seq);
    sqlite3_bind_blob(st, 5, blob, (int)bn, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, updated_at);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}
