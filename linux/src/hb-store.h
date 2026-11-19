#ifndef HB_STORE_H
#define HB_STORE_H

#include <glib-object.h>
#include <harbor/vault.h>

G_BEGIN_DECLS

#define HB_TYPE_STORE (hb_store_get_type())
G_DECLARE_FINAL_TYPE(HbStore, hb_store, HB, STORE, GObject)

/* 应用持有的单例（首次调用即创建并初始化，不做线程安全保护 —— UI 线程用） */
HbStore *hb_store_default(void);

hb_vault *hb_store_vault(HbStore *st);      /* borrowed */
gboolean  hb_store_dek_fallback(HbStore *st);
const char *hb_store_data_dir(HbStore *st);

/* 列出某类非 tombstone 对象（order 升序、name NOCASE）；调用方用 hb_obj_list_free 释放 */
hb_obj  **hb_store_list(HbStore *st, const char *type, size_t *n_out);

/* signals: changed(gchar *type, gchar *id) */

G_END_DECLS

#endif
