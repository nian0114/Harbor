#ifndef HB_VAULT_PAGE_H
#define HB_VAULT_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define HB_TYPE_VAULT_PAGE (hb_vault_page_get_type())
G_DECLARE_FINAL_TYPE(HbVaultPage, hb_vault_page, HB, VAULT_PAGE, GtkBox)

GtkWidget *hb_vault_page_new(void);

/* section id: hosts|keys|fwd|snippets|known_hosts|logs|share|appearance|account */
void hb_vault_page_set_section(HbVaultPage *vp, const char *section);
void hb_vault_page_toggle_sidebar(HbVaultPage *vp);

/* hosts view 子控件（dev flag 用） */
GtkWidget *hb_vault_page_hosts_view(HbVaultPage *vp);
GtkWidget *hb_vault_page_settings_view(HbVaultPage *vp);

G_END_DECLS

#endif
