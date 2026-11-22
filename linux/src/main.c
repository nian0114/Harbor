#include <adwaita.h>
#include "hb-application.h"

/* dev/截图选项（hb-application.c 读取） */
extern char *hb_opt_screenshot;
extern int   hb_opt_screenshot_delay;
extern char *hb_opt_section;
extern char *hb_opt_filter;
extern char *hb_opt_host_view;
extern char *hb_opt_theme;
extern char *hb_opt_open_local;
extern char *hb_opt_connect;

static gboolean parse_open_local(const gchar *n, const gchar *v,
                                 gpointer d, GError **e)
{
    (void)n; (void)d; (void)e;
    hb_opt_open_local = g_strdup(v ? v : "");
    return TRUE;
}

int main(int argc, char **argv)
{
    HbApplication *app;
    int status;
    static const GOptionEntry entries[] = {
        { "screenshot", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING,
          &hb_opt_screenshot, "save window screenshot to PNG and quit", "PATH" },
        { "screenshot-delay", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT,
          &hb_opt_screenshot_delay, "ms before screenshot (default 800)", "MS" },
        { "section", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING,
          &hb_opt_section, "open vault section", "NAME" },
        { "filter", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING,
          &hb_opt_filter, "preset hosts search text", "TEXT" },
        { "host-view", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING,
          &hb_opt_host_view, "grid|list", "MODE" },
        { "theme", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING,
          &hb_opt_theme, "terminal theme id", "ID" },
        { "open-local", 0,
          G_OPTION_FLAG_HIDDEN | G_OPTION_FLAG_OPTIONAL_ARG,
          G_OPTION_ARG_CALLBACK, parse_open_local,
          "open a Local terminal tab; =CMD injects CMD", "[CMD]" },
        { "connect", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING,
          &hb_opt_connect, "quick-connect user@host[:port]", "TARGET" },
        { "background", 0, 0, G_OPTION_ARG_NONE, &hb_opt_background,
          "start hidden if a system tray is available (login autostart)",
          NULL },
        { NULL },
    };
    GOptionContext *ctx;
    GError *err = NULL;

    adw_init();

    ctx = g_option_context_new(NULL);
    g_option_context_add_main_entries(ctx, entries, NULL);
    g_option_context_set_ignore_unknown_options(ctx, TRUE);
    if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
        g_printerr("harbor: %s\n", err->message);
        g_error_free(err);
        return 1;
    }
    g_option_context_free(ctx);

    app = hb_application_new();
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
