#ifndef HB_LOGIN_H
#define HB_LOGIN_H

#include <glib.h>
#include <gio/gio.h>

/* 写/删用户级 systemd unit + graphical-session.target.wants 软链
 * （免 root、不依赖 systemctl）。 */
gboolean hb_login_set_enabled(gboolean on, GError **err);

#endif
