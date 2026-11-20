#include "hb-term-theme.h"

#include <string.h>

#define C(rrggbb) { (((rrggbb) >> 16) & 0xff) / 255.0, \
                    (((rrggbb) >> 8) & 0xff) / 255.0, \
                    ((rrggbb) & 0xff) / 255.0, 1.0 }
#define CA(rrggbb, a) { (((rrggbb) >> 16) & 0xff) / 255.0, \
                        (((rrggbb) >> 8) & 0xff) / 255.0, \
                        ((rrggbb) & 0xff) / 255.0, (a) }

static const HbTermTheme THEMES[] = {
    {
        .id = "harbor",
        .bg = C(0x0a0f1e), .fg = C(0xdbe4f5), .cursor = C(0x3fd97f),
        .bar = C(0x0d1426), .line = C(0x1a2338), .border = C(0x242f4a),
        .green = C(0x3fd97f), .dim = C(0x8b96ad), .muted = C(0x64718c),
        .title = C(0x9fb0cc), .selection = CA(0x3fd97f, 0.30),
        .find = CA(0xe8c66a, 0.35), .find_cur = CA(0xe8c66a, 0.75),
        .ansi = {
            C(0x1a2338), C(0xff7b72), C(0x3fd97f), C(0xe8c66a),
            C(0x6cb8ff), C(0xc792ea), C(0x56c8e8), C(0xdbe4f5),
            C(0x64718c), C(0xff9b94), C(0x6ee6a0), C(0xf2d88c),
            C(0x93ccff), C(0xd6a8f0), C(0x84d8f0), C(0xffffff),
        },
    },
    {
        .id = "ubuntu",
        .bg = C(0x300a24), .fg = C(0xeeeeec), .cursor = C(0x8ae234),
        .bar = C(0x3d1030), .line = C(0x4d1a3f), .border = C(0x632653),
        .green = C(0x8ae234), .dim = C(0xc0a3b8), .muted = C(0xa98ba0),
        .title = C(0xd8c2d2), .selection = CA(0x8ae234, 0.30),
        .find = CA(0xfce94f, 0.30), .find_cur = CA(0xfce94f, 0.70),
        .ansi = {                 /* Tango */
            C(0x2e3436), C(0xcc0000), C(0x4e9a06), C(0xc4a000),
            C(0x3465a4), C(0x75507b), C(0x06989a), C(0xd3d7cf),
            C(0x555753), C(0xef2929), C(0x8ae234), C(0xfce94f),
            C(0x729fcf), C(0xad7fa8), C(0x34e2e2), C(0xeeeeec),
        },
    },
    {
        .id = "solarized",
        .bg = C(0x002b36), .fg = C(0x93a1a1), .cursor = C(0x859900),
        .bar = C(0x073642), .line = C(0x0a4552), .border = C(0x11525f),
        .green = C(0x859900), .dim = C(0x657b83), .muted = C(0x586e75),
        .title = C(0x839496), .selection = CA(0x859900, 0.30),
        .find = CA(0xb58900, 0.45), .find_cur = CA(0xb58900, 0.85),
        .ansi = {
            C(0x073642), C(0xdc322f), C(0x859900), C(0xb58900),
            C(0x268bd2), C(0xd33682), C(0x2aa198), C(0xeee8d5),
            C(0x002b36), C(0xcb4b16), C(0x586e75), C(0x657b83),
            C(0x839496), C(0x6c71c4), C(0x93a1a1), C(0xfdf6e3),
        },
    },
    {
        .id = "light",
        .bg = C(0xf7f9fb), .fg = C(0x1c2333), .cursor = C(0x1a7f4b),
        .bar = C(0xeef1f5), .line = C(0xe2e7ee), .border = C(0xd4dae3),
        .green = C(0x1a7f4b), .dim = C(0x8a93a3), .muted = C(0x9aa3b0),
        .title = C(0x5b6472), .selection = CA(0x2f7cf6, 0.25),
        .find = CA(0xe8a33d, 0.40), .find_cur = CA(0xd97706, 0.80),
        .ansi = {
            C(0x1c2333), C(0xd1242f), C(0x1a7f4b), C(0x9a6700),
            C(0x0969da), C(0x8250df), C(0x1b7c83), C(0x6e7781),
            C(0x57606a), C(0xe5484d), C(0x22a06b), C(0xbf8700),
            C(0x2f7cf6), C(0xa475f9), C(0x3192aa), C(0x8c959f),
        },
    },
};
#define N_THEMES (sizeof THEMES / sizeof THEMES[0])

const HbTermTheme *hb_term_theme_get(const char *id)
{
    size_t i;
    for (i = 0; i < N_THEMES; i++)
        if (id && !strcmp(id, THEMES[i].id))
            return &THEMES[i];
    return &THEMES[0];
}

void hb_term_theme_indexed(const HbTermTheme *t, int idx, GdkRGBA *out)
{
    static const int levels[6] = { 0, 95, 135, 175, 215, 255 };
    int n;

    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    if (idx < 16) {
        *out = t->ansi[idx];
        return;
    }
    if (idx < 232) {                 /* 6×6×6 cube */
        n = idx - 16;
        out->red   = levels[n / 36] / 255.0;
        out->green = levels[(n / 6) % 6] / 255.0;
        out->blue  = levels[n % 6] / 255.0;
        out->alpha = 1.0;
        return;
    }
    n = 8 + (idx - 232) * 10;        /* 232-255 grayscale */
    out->red = out->green = out->blue = n / 255.0;
    out->alpha = 1.0;
}

/* ------------------------- 运行时生成 chrome CSS ------------------------- */

static void hex_rgb(const GdkRGBA *c, char out[8])
{
    g_snprintf(out, 8, "#%02x%02x%02x",
               (int)(c->red * 255.0 + 0.5),
               (int)(c->green * 255.0 + 0.5),
               (int)(c->blue * 255.0 + 0.5));
}

void hb_term_theme_install_css(void)
{
    static gboolean done = FALSE;
    GtkCssProvider *prov;
    GString *s;
    size_t i;
    char bar[8], line[8], border[8], green[8], dim[8], muted[8],
         title[8], bg[8], fg[8];

    if (done)
        return;
    done = TRUE;

    s = g_string_new(NULL);
    for (i = 0; i < N_THEMES; i++) {
        const HbTermTheme *t = &THEMES[i];
        hex_rgb(&t->bar, bar);   hex_rgb(&t->line, line);
        hex_rgb(&t->border, border); hex_rgb(&t->green, green);
        hex_rgb(&t->dim, dim);   hex_rgb(&t->muted, muted);
        hex_rgb(&t->title, title); hex_rgb(&t->bg, bg);
        hex_rgb(&t->fg, fg);
        g_string_append_printf(s,
            ".hb-term-view.theme-%s { background-color: %s; }\n"
            ".hb-term-view.theme-%s .hb-term-toolbar {"
            "  background-color: %s; border-bottom: 1px solid %s; }\n"
            ".hb-term-view.theme-%s .hb-term-status {"
            "  background-color: %s; border-top: 1px solid %s; color: %s; }\n"
            ".hb-term-view.theme-%s .hb-term-tt { color: %s; }\n"
            ".hb-term-view.theme-%s .hb-term-ok { color: %s; }\n"
            ".hb-term-view.theme-%s .hb-term-warn { color: %s; }\n"
            ".hb-term-view.theme-%s .hb-term-dead {"
            "  color: %s; background-color: alpha(%s, .85); }\n"
            ".hb-term-view.theme-%s .hb-term-sb trough {"
            "  background: none; border: none; }\n"
            ".hb-term-view.theme-%s .hb-term-sb slider {"
            "  background-color: %s; border-radius: 5px;"
            "  min-width: 6px; min-height: 30px; }\n"
            /* find bar 按终端主题取色（原型 .find-bar 的 --t-* 变量） */
            ".hb-term-view.theme-%s .find-bar {"
            "  background-color: %s; border-bottom: 1px solid %s;"
            "  color: %s; }\n"
            ".hb-term-view.theme-%s .find-bar entry {"
            "  background-color: %s; border-color: %s;"
            "  color: %s; caret-color: %s; }\n"
            ".hb-term-view.theme-%s .find-bar entry:focus-within {"
            "  border-color: #2f7cf6; }\n"
            ".hb-term-view.theme-%s .find-bar .f-count { color: %s; }\n"
            ".hb-term-view.theme-%s .find-bar button.icon-btn {"
            "  color: %s; }\n"
            ".hb-term-view.theme-%s .find-bar button.icon-btn:hover {"
            "  background-color: %s; }\n"
            /* workspace pane-head / assist 走同一组主题色 */
            ".hb-term-view.theme-%s .pane-head {"
            "  background-color: %s; border-bottom: 1px solid %s;"
            "  color: %s; }\n"
            ".hb-term-view.theme-%s .assist {"
            "  background-color: %s; border-left: 1px solid %s;"
            "  color: %s; }\n"
            ".hb-term-view.theme-%s .pane-head-btn { color: %s; }\n",
            t->id, bg,
            t->id, bar, line,
            t->id, bar, line, muted,
            t->id, title,
            t->id, green,
            t->id, dim,
            t->id, dim, bar,
            t->id,
            t->id, border,
            t->id, bar, line, title,
            t->id, line, border, fg, fg,
            t->id,
            t->id, dim,
            t->id, title,
            t->id, line,
            t->id, bar, line, title,
            t->id, bar, line, dim,
            t->id, title);
        /* light 主题的 "ended" 黄：保持 e8c66a 在所有主题（原型固定色） */
    }
    /* 会话结束的黄统一用原型色 e8c66a（light 主题下仍可读） */
    g_string_append(s,
        ".hb-term-view .hb-term-ended { color: #e8c66a; }\n");

    prov = gtk_css_provider_new();
    gtk_css_provider_load_from_string(prov, s->str);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
    g_string_free(s, TRUE);
}
