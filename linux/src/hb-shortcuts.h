#ifndef HB_SHORTCUTS_H
#define HB_SHORTCUTS_H

#include <gtk/gtk.h>
#include "hb-window.h"

G_BEGIN_DECLS

/* 快捷键表：单一事实来源，供窗口 controller 执行 + 设置页展示。 */
typedef struct {
    const char      *id;       /* "palette" / "find" / "tab-next" … */
    guint            keyval;
    GdkModifierType  mods;     /* 精确匹配 CTRL|SHIFT|ALT 组合 */
    const char      *accel;    /* "Ctrl+Shift+K"（展示用） */
    const char      *label;    /* "命令面板"（设置页用） */
    unsigned         flags;
} HbShortcut;

/* flags */
#define HB_SC_NOT_IN_TERM  1u   /* 焦点在 HbTerminal 内时不生效（留给 shell） */

const HbShortcut *hb_shortcuts(size_t *n);

/* 给窗口装 capture 阶段快捷键 controller（终端吞不到 Ctrl+Shift+F/K 等） */
void hb_shortcuts_install(HbWindow *win);

G_END_DECLS

#endif
