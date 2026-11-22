#!/usr/bin/env python3
"""Minimal StatusNotifierWatcher for headless tray verification.

Owns org.kde.StatusNotifierWatcher on the session bus, implements
RegisterStatusNotifierItem, then answers stdin commands (line-based):

  wait            block until an item registers, print its bus name
  layout          call com.canonical.dbusmenu GetLayout on the item
  tooltip         read the SNI ToolTip property
  props           dump all SNI properties
  click <id>      send dbusmenu Event(id, "clicked", <@i 0>, 0)
  activate        call SNI Activate(0, 0)
  quit            exit
"""

import sys
import gi
gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib

WATCHER_XML = """
<node>
 <interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem">
   <arg direction="in" type="s"/>
  </method>
  <method name="RegisterStatusNotifierHost">
   <arg direction="in" type="s"/>
  </method>
  <method name="RegisterStatusNotifierItemByObjPath">
   <arg direction="in" type="o"/>
  </method>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
  <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
  <signal name="StatusNotifierHostRegistered"/>
 </interface>
</node>
"""

loop = GLib.MainLoop()
items = []          # registered SNI bus names
bus = None


def on_call(conn, sender, path, iface, method, params, inv):
    if method == "RegisterStatusNotifierItem":
        name = params.unpack()[0]
        items.append(name)
        print("REGISTERED", name, flush=True)
        inv.return_value(None)
        conn.emit_signal(None, path, iface,
                         "StatusNotifierItemRegistered",
                         GLib.Variant("(s)", (name,)))
    elif method == "RegisterStatusNotifierItemByObjPath":
        items.append(sender)
        print("REGISTERED", sender, flush=True)
        inv.return_value(None)
    else:
        inv.return_value(None)


def get_prop(conn, sender, path, iface, prop):
    if prop == "RegisteredStatusNotifierItems":
        return GLib.Variant("as", items)
    if prop == "IsStatusNotifierHostRegistered":
        return GLib.Variant("b", True)
    if prop == "ProtocolVersion":
        return GLib.Variant("i", 0)
    return None


def item_name():
    return items[-1] if items else None


def cmd(line):
    parts = line.split()
    if not parts:
        return
    c = parts[0]
    name = item_name()
    if c == "wait":
        if name:
            print("ITEM", name, flush=True)
        return
    if not name:
        print("ERR no item registered", flush=True)
        return
    if c == "layout":
        r = bus.call_sync(name, "/dev/harbor/Harbor/MenuBar",
                          "com.canonical.dbusmenu", "GetLayout",
                          GLib.Variant("(iias)", (0, -1, [])),
                          GLib.VariantType("(u(ia{sv}av))"),
                          Gio.DBusCallFlags.NONE, 3000, None)
        print("LAYOUT", r.unpack()[1], flush=True)
    elif c == "tooltip":
        r = bus.call_sync(name, "/StatusNotifierItem",
                          "org.freedesktop.DBus.Properties", "Get",
                          GLib.Variant("(ss)",
                                       ("org.kde.StatusNotifierItem",
                                        "ToolTip")),
                          GLib.VariantType("(v)"),
                          Gio.DBusCallFlags.NONE, 3000, None)
        print("TOOLTIP", r.unpack()[0], flush=True)
    elif c == "props":
        r = bus.call_sync(name, "/StatusNotifierItem",
                          "org.freedesktop.DBus.Properties", "GetAll",
                          GLib.Variant("(s)",
                                       ("org.kde.StatusNotifierItem",)),
                          GLib.VariantType("(a{sv})"),
                          Gio.DBusCallFlags.NONE, 3000, None)
        d = r.unpack()[0]
        for k in sorted(d):
            if k in ("IconPixmap", "AttentionIconPixmap"):
                print("PROP %s=<pixmap %d entries>" % (k, len(d[k])),
                      flush=True)
            else:
                print("PROP %s=%r" % (k, d[k]), flush=True)
    elif c == "click":
        bus.call_sync(name, "/dev/harbor/Harbor/MenuBar",
                      "com.canonical.dbusmenu", "Event",
                      GLib.Variant("(isvu)", (int(parts[1]), "clicked",
                                            GLib.Variant("i", 0), 0)),
                      None, Gio.DBusCallFlags.NONE, 3000, None)
        print("CLICKED", parts[1], flush=True)
    elif c == "activate":
        bus.call_sync(name, "/StatusNotifierItem",
                      "org.kde.StatusNotifierItem", "Activate",
                      GLib.Variant("(ii)", (0, 0)), None,
                      Gio.DBusCallFlags.NONE, 3000, None)
        print("ACTIVATED", flush=True)
    elif c == "quit":
        loop.quit()


def stdin_cb(chan, cond):
    if cond & (GLib.IO_HUP | GLib.IO_ERR):
        loop.quit()
        return False
    line = chan.read_line()[1]
    if line is None:
        return True
    cmd(line.strip())
    return True


def on_bus_acquired(conn, name):
    # Register the object *before* the name is owned (like real
    # watchers do), so items that react to NameOwnerChanged never
    # hit a window where the name exists but the object does not.
    global bus
    bus = conn
    info = Gio.DBusNodeInfo.new_for_xml(WATCHER_XML)
    conn.register_object("/StatusNotifierWatcher",
                         info.interfaces[0],
                         on_call, get_prop, None)


def on_name_acquired(conn, name):
    print("WATCHER-UP", flush=True)


def main():
    Gio.bus_own_name(Gio.BusType.SESSION,
                     "org.kde.StatusNotifierWatcher",
                     Gio.BusNameOwnerFlags.NONE,
                     on_bus_acquired, on_name_acquired, None)
    chan = GLib.IOChannel.unix_new(sys.stdin.fileno())
    chan.set_flags(chan.get_flags() | GLib.IOFlags.NONBLOCK)
    chan.add_watch(GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, stdin_cb)
    loop.run()


if __name__ == "__main__":
    main()
