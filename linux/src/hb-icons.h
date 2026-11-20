#ifndef HB_ICONS_H
#define HB_ICONS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define HB_TYPE_ICON (hb_icon_get_type())
G_DECLARE_FINAL_TYPE(HbIcon, hb_icon, HB, ICON, GtkWidget)

/* name = prototype P 表里的键；px = 期望边长像素 */
GtkWidget *hb_icon_new(const char *name, int px);
void       hb_icon_set_name(HbIcon *ico, const char *name);
void       hb_icon_set_pixel_size(HbIcon *ico, int px);

/* 文字字形图标（{ } / >_），带 .mono-ico 类 */
GtkWidget *hb_mono_icon_new(const char *glyph);

/* OS 头像：38px 圆角方块 + 白色品牌图形。
 * os_or_proto: ubuntu/debian/generic/local/windows/rdp/vnc/ssh 等，
 * 未知 → generic。px 一般 38（头像）或更小。 */
GtkWidget *hb_os_avatar_new(const char *os_or_proto, int px);

/* 就地换键：CSS 类与字形在原地更新，不重建 widget（bind 路径用） */
void       hb_os_avatar_set_key(GtkWidget *box, const char *os_or_proto);

/* 归一化 OS/协议 → 头像键（ubuntu/generic/debian/local/vnc/rdp/win） */
const char *hb_os_avatar_key(const char *os_or_proto);

G_END_DECLS

#endif
