#include "srv.h"

#include <fcntl.h>
#include <glib-unix.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* server.secret：32B 随机，0600；伪造 prelogin salt 的 HMAC key。 */
static int load_secret(hb_srv *srv, GError **err)
{
    g_autofree char *path =
        g_build_filename(srv->data_dir, "server.secret", NULL);
    int fd;
    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "open %s: %s", path, g_strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (st.st_mode & 0777) != 0600)
        fchmod(fd, 0600);
    if (st.st_size == 32) {
        ssize_t n = read(fd, srv->secret, 32);
        close(fd);
        if (n == 32) return 0;
    } else {
        randombytes_buf(srv->secret, 32);
        if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0 ||
            write(fd, srv->secret, 32) != 32) {
            close(fd);
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "write %s failed", path);
            return -1;
        }
        close(fd);
        return 0;
    }
    close(fd);
    g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "bad %s", path);
    return -1;
}

static hb_srv *g_srv;

static gboolean on_sigint(gpointer u)
{
    (void)u;
    g_main_loop_quit(g_srv->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    g_autoptr(GOptionContext) oc = NULL;
    g_autoptr(GError) err = NULL;
    g_autofree char *opt_listen = g_strdup("127.0.0.1:8787");
    g_autofree char *opt_data = NULL;
    g_autofree char *opt_cert = NULL, *opt_key = NULL;
    g_autofree char *opt_level = g_strdup("info");
    gboolean opt_plain = FALSE;
    gint opt_threads = 0, opt_hqueue = 64;
    hb_srv *srv;
    const GOptionEntry entries[] = {
        { "listen", 'l', 0, G_OPTION_ARG_STRING, &opt_listen,
          "listen address host:port (default 127.0.0.1:8787)", "ADDR" },
        { "data", 'd', 0, G_OPTION_ARG_STRING, &opt_data,
          "data dir (sqlite db + server.secret)", "DIR" },
        { "tls-cert", 0, 0, G_OPTION_ARG_STRING, &opt_cert,
          "TLS certificate file", "FILE" },
        { "tls-key", 0, 0, G_OPTION_ARG_STRING, &opt_key,
          "TLS private key file", "FILE" },
        { "insecure-allow-plain", 0, 0, G_OPTION_ARG_NONE, &opt_plain,
          "allow plain HTTP on non-loopback listen", NULL },
        { "log-level", 0, 0, G_OPTION_ARG_STRING, &opt_level,
          "error|warn|info|debug (default info)", "LEVEL" },
        { "hash-threads", 0, 0, G_OPTION_ARG_INT, &opt_threads,
          "Argon2 worker threads (default min(cpu,4))", "N" },
        { "hash-queue", 0, 0, G_OPTION_ARG_INT, &opt_hqueue,
          "max queued+running hash jobs before 429 (default 64)", "N" },
        { NULL }
    };
    GSocketConnectable *conn;
    GSocketAddressEnumerator *en;
    GSocketAddress *addr;
    GInetAddress *ia;
    gboolean loopback, tls;

    if (sodium_init() < 0) {
        g_printerr("sodium_init failed\n");
        return 1;
    }
    oc = g_option_context_new("- Harbor sync server");
    g_option_context_add_main_entries(oc, entries, NULL);
    if (!g_option_context_parse(oc, &argc, &argv, &err)) {
        g_printerr("%s\n", err->message);
        return 1;
    }

    srv = g_new0(hb_srv, 1);
    g_srv = srv;
    srv->lock_base_ms = 60000;
    srv->ip_limit = HB_SRV_IP_LIMIT;
    {
        const char *e = g_getenv("HARBOR_SERVER_TEST_LOCK_MS");
        if (e && *e) srv->lock_base_ms = g_ascii_strtoll(e, NULL, 10);
        e = g_getenv("HARBOR_SERVER_TEST_IP_LIMIT");
        if (e && *e) srv->ip_limit = atoi(e);
    }
    srv->hash_threads = opt_threads > 0
        ? opt_threads
        : (int)MIN((guint)g_get_num_processors(), 4u);
    if (opt_hqueue < 1) opt_hqueue = 1;
    srv->hash_queue = opt_hqueue;
    if (!strcmp(opt_level, "error")) srv->log_level = 0;
    else if (!strcmp(opt_level, "warn")) srv->log_level = 1;
    else if (!strcmp(opt_level, "debug")) srv->log_level = 3;
    else srv->log_level = 2;

    if (!opt_data)
        opt_data = g_build_filename(g_get_user_data_dir(), "harbor-server",
                                    NULL);
    srv->data_dir = g_steal_pointer(&opt_data);
    if (g_mkdir_with_parents(srv->data_dir, 0700) != 0) {
        g_printerr("cannot create %s\n", srv->data_dir);
        return 1;
    }
    if (load_secret(srv, &err) != 0) {
        g_printerr("%s\n", err->message);
        return 1;
    }
    srv->db = hbd_open(srv->data_dir, &err);
    if (!srv->db) {
        g_printerr("%s\n", err->message);
        return 1;
    }
    /* 重启后无 owner 连接：全部进行中共享标记结束（§4）。 */
    hbd_share_end_all(srv->db);
    /* 未知邮箱登录按 dummy hash 等时校验（防账号探测） */
    if (crypto_pwhash_str(srv->dummy_hash, "harbor-dummy", 12,
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        g_printerr("argon2 dummy hash failed\n");
        return 1;
    }
    srv->ip_hits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         g_free);
    srv->sockets =
        g_ptr_array_new_with_free_func((GDestroyNotify)hb_ws_client_free);
    srv->soup = soup_server_new("server-header", "harbor-server/1.0",
                                NULL);
    srv->loop = g_main_loop_new(NULL, FALSE);

    tls = opt_cert && opt_key;
    if (opt_cert || opt_key) {
        if (!tls) {
            g_printerr("need both --tls-cert and --tls-key\n");
            return 1;
        }
        GTlsCertificate *cert =
            g_tls_certificate_new_from_files(opt_cert, opt_key, &err);
        if (!cert) {
            g_printerr("tls: %s\n", err->message);
            return 1;
        }
        soup_server_set_tls_certificate(srv->soup, cert);
        g_object_unref(cert);
    }

    conn = g_network_address_parse(opt_listen, 8787, &err);
    if (!conn) {
        g_printerr("--listen: %s\n", err->message);
        return 1;
    }
    en = g_socket_connectable_enumerate(conn);
    addr = g_socket_address_enumerator_next(en, NULL, &err);
    if (!addr) {
        g_printerr("--listen: %s\n", err->message);
        return 1;
    }
    loopback = G_IS_INET_SOCKET_ADDRESS(addr) &&
               ((ia = g_inet_socket_address_get_address(
                     G_INET_SOCKET_ADDRESS(addr))) &&
                g_inet_address_get_is_loopback(ia));
    if (!tls && !loopback && !opt_plain) {
        g_printerr("refusing plain HTTP on non-loopback %s "
                   "(use --tls-cert/--tls-key or --insecure-allow-plain)\n",
                   opt_listen);
        return 1;
    }
    if (!soup_server_listen(srv->soup, addr,
                            tls ? SOUP_SERVER_LISTEN_HTTPS : 0, &err)) {
        g_printerr("listen: %s\n", err->message);
        return 1;
    }
    g_object_unref(addr);
    g_object_unref(en);
    g_object_unref(conn);

    http_attach(srv);
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigint, NULL);
    hb_log(srv, 2, "listening on %s (tls=%d) data=%s", opt_listen, tls,
           srv->data_dir);
    g_main_loop_run(srv->loop);
    hb_log(srv, 2, "stopped");
    return 0;
}
