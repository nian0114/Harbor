#ifndef HB_PALETTE_H
#define HB_PALETTE_H

#include <gtk/gtk.h>
#include "hb-window.h"

G_BEGIN_DECLS

/* 命令面板（Ctrl+Shift+K / 终端外 Ctrl+K）：hosts / Vault sections /
 * snippets / commands 的模糊搜索执行器。重复调用 = 已开则聚焦。 */
void hb_palette_open(HbWindow *win);

G_END_DECLS

#endif
