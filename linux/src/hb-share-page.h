#ifndef HB_SHARE_PAGE_H
#define HB_SHARE_PAGE_H

#include <adwaita.h>
#include "hb-share.h"

G_BEGIN_DECLS

#define HB_TYPE_SHARE_PAGE (hb_share_page_get_type())
G_DECLARE_FINAL_TYPE(HbSharePage, hb_share_page, HB, SHARE_PAGE,
                     GtkBox)

/* viewer tab：绑定已 join 的 peer ctx（不接管所有权；destroy 时
 * hb_share_service_release_peer）。 */
GtkWidget *hb_share_page_new(hb_share_peer_ctx *ctx);

/* 打开 viewer tab（win = HbWindow）。 */
void hb_share_open_viewer(GtkWidget *win, hb_share_peer_ctx *ctx,
                          const char *title);
hb_share_peer_ctx *hb_share_page_ctx(GtkWidget *page);
/* dev 校验：可见屏文本（调用方 g_free）。 */
char *hb_share_page_screen(GtkWidget *page);
gboolean hb_share_page_ended(GtkWidget *page);
const char *hb_share_page_end_label(GtkWidget *page);

G_END_DECLS

#endif
