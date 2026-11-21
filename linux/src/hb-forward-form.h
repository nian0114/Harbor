#ifndef HB_FORWARD_FORM_H
#define HB_FORWARD_FORM_H

#include <gtk/gtk.h>
#include <harbor/vault.h>
#include "hb-store.h"

G_BEGIN_DECLS

/* 端口转发规则 新建/编辑 对话框（FR-06）。
 * obj NULL = 新建；非 NULL = 编辑（对象借用，内部读字段）。
 * 「保存」只写 vault；「保存并启动」写 vault 后经 HbForwardMgr 启动。
 * 保存前对 local/dynamic 做绑定冲突预检（hb_pf_probe_bind +
 * 其它已存规则同端口检查），冲突显示行内错误并阻止保存。 */
void hb_forward_form_open(GtkWidget *parent, HbStore *st, hb_obj *obj);

G_END_DECLS

#endif
