#ifndef HB_TRAY_H
#define HB_TRAY_H

#include <glib.h>
#include <gtk/gtk.h>

/* StatusNotifierItem + com.canonical.dbusmenu over GDBus（无
 * libappindicator 依赖）。在 session bus 上导出 SNI 对象并向
 * org.kde.StatusNotifierWatcher 注册；watcher 出现/消失时自动
 * 重新注册/降级。菜单项动作经回调回到 application。 */

typedef struct _HbTray HbTray;

typedef struct {
    int  (*count_online)(void *ud);   /* 当前在线会话数 */
    void (*activate)(void *ud);       /* 显示/raise 主窗口 */
    void (*disconnect_all)(void *ud); /* 断开全部会话（窗口保留） */
    void (*quit)(void *ud);           /* 真正退出 */
    void *ud;
} HbTrayHooks;

HbTray   *hb_tray_new(const HbTrayHooks *hooks);
void      hb_tray_free(HbTray *t);

/* watcher 已注册（= 桌面真的提供托盘区） */
gboolean  hb_tray_available(HbTray *t);

/* watcher 状态变化时通知（settings 开关的 sensitivity）。 */
void      hb_tray_set_watcher_cb(HbTray *t,
                                 void (*cb)(gboolean avail, void *ud),
                                 void *ud);

/* 会话数可能变了：重新读 count_online，变了就 emit NewToolTip +
 * LayoutUpdated。页面状态变化时由 application 调。 */
void      hb_tray_sessions_changed(HbTray *t);

/* 当前 tooltip 上的会话数（测试/调试可见）。 */
int       hb_tray_online(HbTray *t);

#endif
