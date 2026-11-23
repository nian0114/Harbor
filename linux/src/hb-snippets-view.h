#ifndef HB_SNIPPETS_VIEW_H
#define HB_SNIPPETS_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_SNIPPETS_VIEW (hb_snippets_view_get_type())
G_DECLARE_FINAL_TYPE(HbSnippetsView, hb_snippets_view,
                     HB, SNIPPETS_VIEW, GtkBox)

GtkWidget *hb_snippets_view_new(HbStore *store);

/* dev/调试：切到 Shell History 子页（hist=TRUE）或回到 Snippets。 */
void hb_snippets_view_show_hist(HbSnippetsView *v, gboolean hist);

G_END_DECLS

#endif
