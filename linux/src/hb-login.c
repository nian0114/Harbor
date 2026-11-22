/* hb-login.c — start_at_login：用户级 systemd unit。
 * $XDG_CONFIG_HOME/systemd/user/dev.harbor.Harbor.service +
 * graphical-session.target.wants/ 下的软链，手动创建（无 root、
 * 不依赖 systemctl）。ExecStart 用 /proc/self/exe 或 $APPIMAGE。 */
#include "hb-login.h"

#include <stdlib.h>
#include <unistd.h>

gboolean hb_login_set_enabled(gboolean on, GError **err)
{
    const char *cfg = g_get_user_config_dir();
    char *udir, *unit, *wdir, *link, *exe = NULL;
    gboolean ok;

    udir = g_build_filename(cfg, "systemd", "user", NULL);
    unit = g_build_filename(udir, "dev.harbor.Harbor.service", NULL);
    wdir = g_build_filename(udir, "graphical-session.target.wants",
                            NULL);
    link = g_build_filename(wdir, "dev.harbor.Harbor.service", NULL);

    if (!on) {
        unlink(link);
        unlink(unit);
        g_free(udir); g_free(unit); g_free(wdir); g_free(link);
        return TRUE;
    }
    if (g_getenv("APPIMAGE"))
        exe = g_strdup(g_getenv("APPIMAGE"));
    else {
        char *rp = realpath("/proc/self/exe", NULL);
        exe = rp ? g_strdup(rp)
                 : g_strdup(g_get_prgname() ? g_get_prgname()
                                          : "harbor");
        free(rp);
    }
    {
        char *content = g_strdup_printf(
            "[Unit]\n"
            "Description=Harbor remote workbench\n"
            "PartOf=graphical-session.target\n"
            "After=graphical-session.target\n"
            "\n"
            "[Service]\n"
            "Type=simple\n"
            "ExecStart=%s --background\n"
            "Restart=no\n"
            "\n"
            "[Install]\n"
            "WantedBy=graphical-session.target\n", exe);
        ok = g_mkdir_with_parents(udir, 0755) == 0 &&
             g_mkdir_with_parents(wdir, 0755) == 0 &&
             g_file_set_contents(unit, content, -1, err) &&
             (unlink(link), symlink("../dev.harbor.Harbor.service",
                                    link) == 0);
        g_free(content);
    }
    if (!ok && err && !*err)
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot write login unit");
    g_free(exe); g_free(udir); g_free(unit); g_free(wdir);
    g_free(link);
    return ok;
}
