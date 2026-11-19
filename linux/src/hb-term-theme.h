#ifndef HB_TERM_THEME_H
#define HB_TERM_THEME_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* 终端配色：cell 渲染（bg/fg/cursor/selection/ansi）+ chrome
 * （bar/line/border/green/dim/muted/title 供 .hb-term-view.theme-<id>
 * 运行时 CSS 使用，值只在此表里定义一次）。 */
typedef struct {
    const char *id;
    GdkRGBA bg, fg, cursor, bar, line, border, green, dim, muted, title,
            selection;
    GdkRGBA ansi[16];
} HbTermTheme;

const HbTermTheme *hb_term_theme_get(const char *id); /* 未知 id → harbor */

/* idx 0-255 → out（0-15 表内，16-231 cube，232-255 灰阶） */
void hb_term_theme_indexed(const HbTermTheme *t, int idx, GdkRGBA *out);

/* 由表生成 .hb-term-view.theme-* CSS 并注册（幂等） */
void hb_term_theme_install_css(void);

G_END_DECLS

#endif
