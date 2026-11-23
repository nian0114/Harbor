#if DEBUG
import Foundation
import AppKit
import CHarbor
import HarborKit

/// 开发钩子（仅 DEBUG + HARBOR_DEV_ACTIONS=1）：
/// $HARBOR_DATA_DIR/dev.sock 上的行协议。配合 scripts/dev-mac.sh。
final class DevServer {
    private var fd: Int32 = -1
    private var src: DispatchSourceRead?
    private let ioQueue = DispatchQueue(label: "harbor.dev.io")
    /// 可在 IO 线程直接执行的命令（仅触 DialogRegistry 的 sheet 控件）。
    private let offMainSafe: Set<String> = [
        "postkey", "quit-reply",
    ]
    weak var win: MainWindow?
    let path: String

    /// 在主 runloop 上执行。dispatch .main 在 NSApp.terminate 的
    /// _shouldTerminate 嵌套事件循环 / sheet 模态期不会 drain，dev 命令饿死；
    /// 这里把 block 注册到各常用 runloop mode，先到先执行（主线程单发，flag 去重）。
    private func runOnMain(_ b: @escaping () -> Void) {
        final class Box { var done = false }
        let box = Box()
        let wrap: () -> Void = {
            guard !box.done else { return }
            box.done = true
            b()
        }
        DispatchQueue.main.async(execute: wrap)
        for m in [RunLoop.Mode.default.rawValue,
                  RunLoop.Mode.modalPanel.rawValue,
                  RunLoop.Mode.eventTracking.rawValue,
                  "NSConnectionReplyMode"] {
            CFRunLoopPerformBlock(CFRunLoopGetMain(), m as CFString, wrap)
        }
        CFRunLoopWakeUp(CFRunLoopGetMain())
    }

    init?(win: MainWindow?) {
        self.win = win
        path = Vault.shared.dataDir.appendingPathComponent("dev.sock").path
        unlink(path)
        fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        // listen fd 非阻塞：accept 循环靠 EAGAIN 收尾，不能堵主队列
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        var pathBuf = [CChar](repeating: 0, count: 104)
        _ = path.withCString { strncpy(&pathBuf, $0, 103) }
        withUnsafeMutableBytes(of: &addr.sun_path) { dst in
            dst.copyBytes(from: pathBuf.map { UInt8(bitPattern: $0) })
        }
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, len) } } == 0 && listen(fd, 4) == 0
        guard ok else { close(fd); return nil }
        // 读/accept 走后台队列，命令再经 CFRunLoopPerformBlock(AllModes) 上主线程。
        // 不能用 .main 派发队列：NSApp.terminate 的 _shouldTerminate 嵌套事件循环
        // 和 sheet 模态期都不 drain 主派发队列，dev 命令会饿死（quit 确认无法驱动）。
        let s = DispatchSource.makeReadSource(fileDescriptor: fd,
                                              queue: ioQueue)
        s.setEventHandler { [weak self] in
            self?.accept()
        }
        s.resume()
        src = s
    }
    deinit { src?.cancel(); if fd >= 0 { close(fd) }; unlink(path) }

    private func accept() {
        var c: Int32
        repeat {
            c = Darwin.accept(fd, nil, nil)
            if c >= 0 {
                let cfd = c   // 捕获值，不能引用循环变量（结束后为 -1）
                _ = fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK)
                let s = DispatchSource.makeReadSource(fileDescriptor: cfd,
                                                      queue: ioQueue)
                var buf = ""
                s.setEventHandler { [weak self] in
                    var chunk = [UInt8](repeating: 0, count: 8192)
                    let n = read(cfd, &chunk, chunk.count)
                    if n <= 0 { s.cancel(); close(cfd); return }
                    buf += String(decoding: chunk[0..<n], as: UTF8.self)
                    let respond: (String) -> Void = { cmdline in
                        let resp = (self?.handle(cmdline) ?? "ERR") + "\n"
                        resp.withCString { ptr in
                            var off = 0, n = strlen(ptr)
                            while off < n {
                                let w = Darwin.write(cfd, ptr + off,
                                                     n - off)
                                if w > 0 { off += w; continue }
                                // cfd 非阻塞：EAGAIN 时等可写
                                if w < 0 && errno == EAGAIN {
                                    var p = pollfd(fd: cfd,
                                                   events: Int16(POLLOUT),
                                                   revents: 0)
                                    _ = poll(&p, 1, 5000)
                                    continue
                                }
                                break
                            }
                        }
                    }
                    while let eol = buf.firstIndex(of: "\n") {
                        let line = String(buf[..<eol])
                        buf.removeSubrange(...eol)
                        // NSApp.terminate 的 _shouldTerminate 嵌套事件循环会
                        // 饿死主 runloop 的所有 mode；此期间唯一线程安全的驱动
                        // 方式是 postEvent（文档允许任意线程调用）。
                        let first = line.split(separator: " ").first
                            .map(String.init) ?? ""
                        if self?.offMainSafe.contains(first) == true {
                            respond(line)
                        } else {
                            self?.runOnMain { respond(line) }
                        }
                    }
                }
                s.setCancelHandler { }
                s.resume()
                // 防止过早释放
                objc_setAssociatedObject(self, Unmanaged.passUnretained(
                    s as AnyObject).toOpaque(), s, .OBJC_ASSOCIATION_RETAIN)
            }
        } while c >= 0
    }

    private func handle(_ line: String) -> String {
        let parts = line.split(separator: " ", maxSplits: 1)
        let cmd = String(parts.first ?? "")
        let arg = parts.count > 1 ? String(parts[1]) : ""
        // type/ime-* 负载为 base64，wire 单行协议零转义歧义
        switch cmd {
        case "ping": return "OK pong"
        case "postkey": return cmdPostKey(arg)
        case "quit-reply":
            // _shouldTerminate 嵌套循环期间主 runloop 全 mode 饿死，
            // reply() 仅是置旗+唤醒，可安全在 IO 线程调用。
            DialogRegistry.shared.pendingTerminate = false
            NSApp.reply(toApplicationShouldTerminate:
                            arg == "1" || arg == "true" || arg == "quit")
            return "OK"
        case "snapshot": return cmdSnapshot(arg)
        case "type":    return cmdType(arg)
        case "key":     return cmdKey(arg)
        case "ime-mark":   return cmdIme(arg, commit: false)
        case "ime-commit": return cmdIme(arg, commit: true)
        case "resize":  return cmdResize(arg)
        case "newtab":  win?.newLocalTab(); return "OK"
        case "closetab":
            win?.closeTab(win?.selTab ?? 0); return "OK"
        case "select-sidebar":
            if let i = Int(arg) {
                win?.vaultTab.sidebar.select(i)
                win?.showSection(i) }
            return "OK"
        case "action": return cmdAction(arg)
        case "select": return cmdSelect(arg)
        case "stall-reset":
            win?.maxRunloopStall = 0; return "OK"
        case "set":    return cmdSet(arg)
        case "state":  return cmdState()
        case "gridinfo": return cmdGridInfo()
        case "dialogs": return DialogRegistry.shared.json
        case "dialog-fill":
            let kv = arg.split(separator: " ", maxSplits: 1)
            guard kv.count == 2,
                  let d = Data(base64Encoded: String(kv[1])),
                  let txt = String(data: d, encoding: .utf8) else {
                return "ERR usage: dialog-fill <field> <base64>" }
            return (DialogRegistry.shared.fill(field: String(kv[0]),
                                               text: txt)
                    || fillInAllWindows(field: String(kv[0]),
                                        text: txt))
                ? "OK" : "ERR no field"
        case "dialog-press":
            return DialogRegistry.shared.press(arg) ? "OK" : "ERR no button"
        case "click":    return cmdClick(arg)
        case "dismiss-modal": return cmdDismissModal()
        case "hosts":    return cmdHosts()
        case "import-sshcfg": return cmdImportCfg(arg)
        case "connect":  return cmdConnect(arg)
        case "search":   return cmdSearch(arg)
        case "host-set": return cmdHostSet(arg)
        case "kc-gen":   return cmdKcGen(arg)
        case "kc-import": return cmdKcImport(arg)
        case "kc-list":  return cmdKcList()
        case "kc-del":   return cmdKcDel(arg)
        case "kc-sel":   return cmdKcSel(arg)
        case "host-sel": return cmdHostSel(arg)
        case "host-group": return cmdHostGroup(arg)
        case "host-del": return cmdHostDel(arg)
        case "scroll-test": return cmdScrollTest()
        // ---- M2a dev hooks（split/broadcast/focus 走 action 菜单 id） ----
        case "pal-open":
            win?.openPalette(); return "OK"
        case "pal-type":
            Palette.devType(arg); return "OK"
        case "pal-items":
            return Palette.devItems.enumerated().map {
                "\($0)|\($1.kind)|\($1.title)|\($1.id)"
            }.joined(separator: "\n")
        case "pal-enter":
            Palette.devExec(Int(arg) ?? Palette.devSel); return "OK"
        case "pal-close":
            Palette.closeCurrent(); return "OK"
        case "pal-state":
            return "open=\(Palette.isOpen) sel=\(Palette.devSel) " +
                   "n=\(Palette.devItems.count)"
        case "find-open":
            win?.wsFind(); return "OK"
        case "find-type":
            selTab?.devFindSet(arg); return "OK"
        case "find-next":
            selTab?.devFindStep(1); return "OK"
        case "find-prev":
            selTab?.devFindStep(-1); return "OK"
        case "find-close":
            selTab?.closeFind(); return "OK"
        case "find-count":
            return selTab?.findCountText ?? "ERR no-tab"
        case "ws-state":
            return selWs?.devState ?? "ERR no-workspace"
        case "ws-focus-pane":
            return cmdWsFocusPane(arg)
        case "notifs":
            return NotifyCenter.shared.json
        case "sysnotifs":
            // M8 系统通知接缝的实际调用（focus-DND/quiet 验证用）
            if let r = NotifyCenter.shared.system
                as? RecordingSystemNotifier {
                return "OK " + r.calls.joined(separator: " ;; ")
            }
            return "ERR"
        case "notif-jump":
            // 与 NotifRow.onClick 同路：item.tabIndex → select
            guard let i = Int(arg), i >= 0,
                  i < NotifyCenter.shared.items.count,
                  let ti = NotifyCenter.shared.items[i].tabIndex,
                  ti >= 0 else { return "ERR" }
            win?.select(ti); return "OK \(ti)"
        case "watch-badge":
            if let w = selTab as? WsTab,
               let g = w.badgeGlyph { return "badge=\(g.0)" }
            return "badge=\(selTab?.watchBadge ?? "none")"
        case "watch-test": return cmdWatchTest(arg)
        case "watch-install":
            // 本地 OSC133 集成安装；$HOME 由进程环境决定（测试可换临时 HOME）
            let (ok, err) = WatchInstaller.installLocal(arg)
            return ok ? "OK \(WatchInstaller.rcPath(arg))"
                      : "ERR \(err ?? "?")"
        case "snippet-new": return cmdSnippetNew(arg)
        case "mode":   return cmdMode(arg)
        case "activate":
            // 置为 key window（focus-DND 等 isKeyWindow 语义测试用）
            NSApp.activate(ignoringOtherApps: true)
            win?.makeKeyAndOrderFront(nil)
            return "OK key=\(win?.isKeyWindow == true) active=\(NSApp.isActive) sel=\(win?.selTab ?? -1)"
        case "force-focus":
            // SSH 驱动不出真实 key window —— 强制 route 的 focused 判定
            switch arg {
            case "on":  WatchRoute.devForceFocused = true
            case "off": WatchRoute.devForceFocused = false
            default:    WatchRoute.devForceFocused = nil
            }
            return "OK focus=\(WatchRoute.devForceFocused.map { $0 ? "on" : "off" } ?? "auto")"
        case "quit":     NSApp.terminate(nil); return "OK"
        default: return "ERR unknown"
        }
    }

    private func cmdSnapshot(_ arg: String) -> String {
        // 整窗（含标题栏/标签条）：渲染 frame 视图而不是 contentView
        guard let v = win?.contentView?.superview ?? win?.contentView
        else { return "ERR" }
        let path = arg.isEmpty ? "/tmp/harbor-snap.png" : arg
        guard let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)
        else { return "ERR rep" }
        v.cacheDisplay(in: v.bounds, to: rep)
        guard let png = rep.representation(using: .png, properties: [:])
        else { return "ERR png" }
        do { try png.write(to: URL(fileURLWithPath: path)) }
        catch { return "ERR write" }
        // 有 sheet/alert 也渲染出来（<path>.sheet.png）
        var extra = ""
        if let sh = win?.attachedSheet, let cv = sh.contentView {
            if let r2 = cv.bitmapImageRepForCachingDisplay(in: cv.bounds) {
                cv.cacheDisplay(in: cv.bounds, to: r2)
                if let p2 = r2.representation(using: .png,
                                              properties: [:]) {
                    try? p2.write(to: URL(fileURLWithPath:
                                          path + ".sheet.png"))
                    extra = " +sheet"
                }
            }
        }
        return "OK \(path)\(extra)"
    }
    private func cmdAction(_ arg: String) -> String {
        // 按 identifier 查找并触发菜单项/已注册动作
        func find(_ menu: NSMenu, _ id: String) -> NSMenuItem? {
            for it in menu.items {
                if it.identifier?.rawValue == id { return it }
                if let m = find(it.submenu ?? NSMenu(), id) { return m }
            }
            return nil
        }
        guard !arg.isEmpty,
              let it = find(NSApp.mainMenu ?? NSMenu(), arg)
        else { return "ERR unknown action" }
        guard let act = it.action else { return "ERR no-action" }
        // 异步派发：NSApp.terminate → _shouldTerminate 会在主队列开嵌套事件循环
        // 等待 reply(toApplicationShouldTerminate:)；同步执行会死锁本 dev server。
        DispatchQueue.main.async {
            _ = NSApp.sendAction(act, to: it.target, from: it)
        }
        return "OK"
    }
    private func cmdSet(_ arg: String) -> String {
        // set <vault-key> <value> —— 设置库写入 + 立即应用（主题/字号实测用）
        let kv = arg.split(separator: " ", maxSplits: 1)
        guard kv.count == 2 else { return "ERR set-args" }
        Vault.shared.setSetting(String(kv[0]), String(kv[1]))
        win?.applySettings()
        return "OK"
    }
    private func cmdSelect(_ arg: String) -> String {
        // select <r0,c0,r1,c1[,block]> —— r/c 为屏幕行列（scrollback 用负数）
        guard let term = win?.tabs[win?.selTab ?? 0].term else { return "ERR" }
        var block = false
        var a = arg
        if a.hasSuffix(",block") { block = true
            a = String(a.dropLast(6)) }
        let nums = a.split(separator: ",").compactMap { Int($0) }
        guard nums.count == 4 else { return "ERR select-args" }
        term.selAnchor = TerminalView.SelPt(row: nums[0], col: nums[1])
        term.selCaret  = TerminalView.SelPt(row: nums[2], col: nums[3])
        term.blockSel = block
        term.bumpSelHash(); term.poke()
        return "OK"
    }
    private func cmdType(_ arg: String) -> String {
        guard let data = Data(base64Encoded: arg) else { return "ERR b64" }
        if let ws = selWs { ws.routeWrite(data); return "OK" }
        guard let t = win?.tabs[win?.selTab ?? 0].session else { return "ERR" }
        data.withUnsafeBytes { ptr in
            hb_session_write(t.sess, ptr.baseAddress!
                .assumingMemoryBound(to: CChar.self), data.count)
        }
        return "OK"
    }
    private func cmdKey(_ arg: String) -> String {
        // "ctrl+c" | "enter" | "f1" | "up" …
        var mods: Int32 = 0
        var name = arg.lowercased()
        for (k, m) in [("ctrl+", KeyMap.HB_MOD_CTRL),
                       ("shift+", KeyMap.HB_MOD_SHIFT),
                       ("alt+", KeyMap.HB_MOD_ALT)] {
            while name.hasPrefix(k) {
                name.removeFirst(k.count); mods |= m }
        }
        let named: [String: Int32] = [
            "enter": KeyMap.HB_KEY_ENTER, "tab": KeyMap.HB_KEY_TAB,
            "backspace": KeyMap.HB_KEY_BACKSPACE, "esc": KeyMap.HB_KEY_ESCAPE,
            "up": KeyMap.HB_KEY_UP, "down": KeyMap.HB_KEY_DOWN,
            "left": KeyMap.HB_KEY_LEFT, "right": KeyMap.HB_KEY_RIGHT,
            "home": KeyMap.HB_KEY_HOME, "end": KeyMap.HB_KEY_END,
            "pageup": KeyMap.HB_KEY_PAGEUP, "pagedown": KeyMap.HB_KEY_PAGEDOWN,
            "del": KeyMap.HB_KEY_DEL,
        ]
        let ws = selWs
        func write(_ sess: OpaquePointer?, _ data: Data) {
            data.withUnsafeBytes { ptr in
                if let ws { ws.routeWrite(data) }
                else if let sess {
                    hb_session_write(sess, ptr.baseAddress!
                        .assumingMemoryBound(to: CChar.self), data.count)
                }
            }
        }
        let sess = win?.tabs[win?.selTab ?? 0].session?.sess
        if let kk = named[name] {
            if let ws { ws.routeKey(kk, mods) }
            else if let sess {
                hb_session_key(sess, hb_key(rawValue: UInt32(kk)), mods) }
            else { return "ERR" }
            return "OK"
        }
        if name.hasPrefix("f"), let n = Int32(name.dropFirst()),
           n >= 1 && n <= 12 {
            if let ws { ws.routeKey(KeyMap.fkey(n), mods) }
            else if let sess {
                hb_session_key(sess, hb_key(rawValue: UInt32(KeyMap.fkey(n))),
                               mods) }
            else { return "ERR" }
            return "OK"
        }
        if let ch = name.first, name.count == 1 {
            if mods & KeyMap.HB_MOD_CTRL != 0, let a = ch.asciiValue {
                // 真终端发的是裸控制字节，不是 CSI-u
                var b = a & 0x1f
                write(sess, Data(bytes: &b, count: 1))
            } else if let u = ch.unicodeScalars.first {
                if let ws { ws.routeUnichar(u.value, mods) }
                else if let sess {
                    hb_session_unichar(sess, u.value, mods) }
                else { return "ERR" }
            }
            return "OK"
        }
        return "ERR key"
    }
    private func cmdIme(_ arg: String, commit: Bool) -> String {
        guard let term = win?.tabs[win?.selTab ?? 0].term else { return "ERR" }
        guard let data = Data(base64Encoded: arg),
              let str = String(data: data, encoding: .utf8)
        else { return "ERR b64" }
        if commit { term.insertText(str, replacementRange: NSRange()) }
        else { term.setMarkedText(str, selectedRange:
                NSRange(location: str.count, length: 0),
                replacementRange: NSRange()) }
        return "OK"
    }
    private func cmdResize(_ arg: String) -> String {
        let ps = arg.split(separator: " ")
        guard ps.count == 2, let w = Double(ps[0]), let h = Double(ps[1]),
              let win else { return "ERR" }
        var f = win.frame
        f.size = NSSize(width: w, height: h)
        win.setFrame(f, display: true)
        return "OK"
    }
    private func cmdGridInfo() -> String {
        guard let term = win?.tabs[win?.selTab ?? 0].term else { return "ERR" }
        return "OK " + term.gridInfo()
    }
    private func cmdState() -> String {
        guard let win else { return "{}" }
        var tabs: [[String: Any]] = []
        for t in win.tabs {
            var d: [String: Any] = ["title": t.title,
                                    "running": t.running,
                                    "termFrame": NSStringFromRect(
                                        t.term?.frame ?? .zero)]
            if let vt = t.session?.vt {
                hb_vt_lock(vt)
                var r: Int32 = 0, c: Int32 = 0
                hb_vt_size(vt, &r, &c)
                var screen = ""
                for row in 0..<r {
                    if let s = hb_vt_get_text(vt, row, 0, row, c-1, false) {
                        screen += String(cString: s) + "\n"
                        free(s)
                    }
                }
                hb_vt_unlock(vt)
                d["rows"] = r; d["cols"] = c
                d["screen"] = screen
            }
            tabs.append(d)
        }
        let obj: [String: Any] = [
            "tabs": tabs, "sel": win.selTab,
            "maxStallMs": Int(win.maxRunloopStall * 1000),
            "winVisible": win.isVisible,
            "occlusion": win.occlusionState.rawValue,
            "appActive": NSApp.isActive,
        ]
        let d = (try? JSONSerialization.data(withJSONObject: obj)) ?? Data()
        return "OK " + (String(data: d, encoding: .utf8) ?? "{}")
    }

    /// 点击任意带 accessibilityIdentifier 的控件（含对话框按钮）。
    /// postkey <return|escape> —— 从任意线程 post 真实按键事件，用于驱动
    /// _shouldTerminate 嵌套循环里的 quit 确认 sheet（Return=默认钮，Esc=取消）。
    private func cmdPostKey(_ arg: String) -> String {
        let code: UInt16 = arg == "escape" ? 53 : 36
        let chars = arg == "escape" ? "\u{1b}" : "\r"
        for t in [NSEvent.EventType.keyDown, NSEvent.EventType.keyUp] {
            guard let e = NSEvent.keyEvent(
                with: t, location: .zero, modifierFlags: [],
                timestamp: 0, windowNumber: 0, context: nil,
                characters: chars, charactersIgnoringModifiers: chars,
                isARepeat: false, keyCode: code)
            else { return "ERR no-event" }
            NSApp.postEvent(e, atStart: false)
        }
        return "OK"
    }

    private func cmdClick(_ arg: String) -> String {
        if DialogRegistry.shared.press(arg) { return "OK" }
        guard let w = win else { return "ERR no window" }
        if clickIn(w.contentView, arg) { return "OK" }
        // 非 NSAlert 的 sheet（如主机表单）和独立窗口不在 contentView 下
        if let s = w.attachedSheet, clickIn(s.contentView, arg) { return "OK" }
        for ow in NSApp.windows where ow !== w {
            if clickIn(ow.contentView, arg) { return "OK" }
        }
        return "ERR no such control"
    }
    /// 在所有窗口（含非 NSAlert sheet，如主机表单）中按 id 找输入控件。
    private func fillInAllWindows(field id: String, text: String) -> Bool {
        for w in NSApp.windows {
            if fillIn(w.contentView, id, text) { return true }
            if let s = w.attachedSheet, fillIn(s.contentView, id, text) {
                return true
            }
        }
        return false
    }
    private func fillIn(_ v: NSView?, _ id: String, _ text: String) -> Bool {
        guard let v else { return false }
        for s in v.subviews {
            if let c = s as? NSControl, c.accessibilityIdentifier() == id {
                if let b = c as? NSButton {
                    b.state = (text == "1" || text == "true") ? .on : .off
                } else {
                    c.stringValue = text
                    // 像真实输入一样触发 controlTextDidChange（表单校验）
                    NotificationCenter.default.post(
                        name: NSControl.textDidChangeNotification,
                        object: c)
                }
                return true
            }
            if fillIn(s, id, text) { return true }
        }
        return false
    }
    private func clickIn(_ v: NSView?, _ id: String) -> Bool {
        guard let v else { return false }
        for s in v.subviews {
            if let c = s as? NSControl, c.accessibilityIdentifier() == id {
                if let b = c as? NSButton { b.performClick(nil) }
                else { c.sendAction(c.action, to: c.target) }
                return true
            }
            if clickIn(s, id) { return true }
        }
        return false
    }

    /// vault 中全部主机 → JSON
    private func cmdHosts() -> String {
        var arr: [[String: Any]] = []
        for o in Vault.shared.list("host") {
            var tags: [String] = []
            var sv: UnsafeMutablePointer<UnsafePointer<CChar>?>?
            let nt = hb_obj_get_strv(o, "tags", &sv)
            for i in 0..<nt { if let t = sv?[i] { tags.append(t.str) } }
            free(sv)
            var jumps: [String] = []
            let nj = hb_obj_get_strv(o, "jump", &sv)
            for i in 0..<nj { if let j = sv?[i] { jumps.append(j.str) } }
            free(sv)
            arr.append(["id": hb_obj_id(o).str,
                        "name": hb_obj_get_str(o, "name").str,
                        "address": hb_obj_get_str(o, "address").str,
                        "port": hb_obj_get_int(o, "port", 22),
                        "user": hb_obj_get_str(o, "user").str,
                        "group": hb_obj_get_str(o, "group").str,
                        "os": hb_obj_get_str(o, "os").str,
                        "auth": hb_obj_get_str(o, "auth").str,
                        "key_id": hb_obj_get_str(o, "key_id").str,
                        "identity_file": hb_obj_get_str(o, "identity_file").str,
                        "tags": tags, "jump": jumps])
            hb_obj_free(o)
        }
        let d = try? JSONSerialization.data(withJSONObject: arr)
        return "OK " + (d.flatMap { String(data: $0, encoding: .utf8) } ?? "[]")
    }

    /// 导入指定路径的 ssh config（测试用，非 ~/.ssh/config）。
    private func cmdImportCfg(_ arg: String) -> String {
        guard let hp = win?.contentView?.findPage(HostsPage.self) else {
            // 未在主机页 → 直接走 core
            var cfg = hb_sshcfg()
            guard hb_sshcfg_parse_file(arg, &cfg) == HB_OK,
                  let vv = Vault.shared.vault else { return "ERR parse" }
            var r = hb_import_result()
            let e = hb_hosts_import_sshcfg(vv, &cfg, &r)
            let n = Int(r.created) + Int(r.jumps_created)
            hb_import_result_free(&r); hb_sshcfg_free(&cfg)
            return e == HB_OK ? "OK \(n)" : "ERR import"
        }
        hp.importSSHConfig(path: arg)
        return "OK"
    }

    /// dev 连接：`connect <host-id|user@host:port>` → 新 SSH tab。
    private func cmdConnect(_ arg: String) -> String {
        guard let w = win else { return "ERR no window" }
        let isId = !arg.contains("@") && !arg.contains(":")
            && Vault.shared.get(arg) != nil
        if isId {
            let h = Vault.shared.get(arg)!
            let title = hb_obj_get_str(h, "name").str
            hb_obj_free(h)
            w.openSSH(hostId: arg, quickTarget: nil,
                      title: title.isEmpty ? arg : title)
        } else {
            w.openSSH(hostId: nil, quickTarget: arg, title: arg)
        }
        return "OK"
    }

    /// 关闭任意系统/应用 modal（如崩溃恢复提示）：按默认按钮。
    private func page<T: NSView>(_ t: T.Type) -> T? {
        win?.contentView?.findPage(t)
    }
    /// `search <q>` → 过滤耗时(ms)
    private func cmdSearch(_ arg: String) -> String {
        guard let p = page(HostsPage.self) else { return "ERR no page" }
        let t0 = CFAbsoluteTimeGetCurrent()
        p.devSearch(arg)
        let ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000
        return String(format: "OK %.1fms", ms)
    }
    /// `host-set <id> <field> <value>` — 直接写 host 字段（测试用）
    private func cmdHostSet(_ arg: String) -> String {
        let kv = arg.split(separator: " ", maxSplits: 2)
        guard kv.count == 3, let o = Vault.shared.get(String(kv[0]))
        else { return "ERR" }
        if ["keepalive", "port"].contains(kv[1]),
           let i = Int64(kv[2]) {
            hb_obj_set_int(o, String(kv[1]), i)
        } else if kv[1] == "tags" {
            let parts = String(kv[2]).split(separator: ",")
                .map { strdup(String($0)) }
            defer { parts.forEach { free($0) } }
            hb_obj_set_strv(o, "tags",
                            parts.map { UnsafePointer($0) }, parts.count)
        } else {
            hb_obj_set_str(o, String(kv[1]), String(kv[2]))
        }
        Vault.shared.put(o); hb_obj_free(o)
        page(HostsPage.self)?.reload()
        return "OK"
    }
    /// `kc-gen <type> <name> [pass-b64]`：生成密钥入 vault（同 KeychainPage 路径）
    private func cmdKcGen(_ arg: String) -> String {
        let kv = arg.split(separator: " ")
        guard kv.count >= 2 else { return "ERR usage" }
        let t: hb_key_type; var bits: Int32 = 0
        switch kv[0] {
        case "rsa": t = HB_KEY_RSA; bits = 3072
        case "ecdsa": t = HB_KEY_ECDSA_P256
        default: t = HB_KEY_ED25519
        }
        var k = hb_key_info()
        let e = String(kv[1]).withCString {
            hb_key_generate(t, bits, $0, &k) }
        guard e == HB_OK else { return "ERR gen" }
        defer { hb_key_info_clear(&k) }
        let o = hb_obj_new("key")!
        let kid = hb_obj_id(o).str
        hb_obj_set_str(o, "name", String(kv[1]))
        hb_obj_set_str(o, "keytype", k.kind.str)
        hb_obj_set_int(o, "bits", Int64(k.bits))
        hb_obj_set_str(o, "fingerprint", k.fingerprint)
        hb_obj_set_str(o, "public", k.public_line)
        hb_obj_set_str(o, "created",
                       "\(Int64(Date().timeIntervalSince1970))")
        Vault.shared.put(o)
        if let p = k.private_text {
            Vault.shared.setSecret(kid, "private", p.str) }
        // 导入口令不落库：首次使用时询问，由“记住”保存（与 Linux 一致）
        page(KeychainPage.self)?.reload()
        return "OK \(kid)"
    }
    /// `kc-import <path> [pass-b64]`
    private func cmdKcImport(_ arg: String) -> String {
        let kv = arg.split(separator: " ")
        guard !kv.isEmpty else { return "ERR" }
        var pass = ""
        if kv.count > 1, let d = Data(base64Encoded: String(kv[1])) {
            pass = String(data: d, encoding: .utf8) ?? ""
        }
        var k = hb_key_info()
        let e = pass.withCString {
            hb_key_inspect_file(String(kv[0]),
                                pass.isEmpty ? nil : $0, &k) }
        guard e == HB_OK else { return "ERR \(e.rawValue)" }
        defer { hb_key_info_clear(&k) }
        let o = hb_obj_new("key")!
        let kid = hb_obj_id(o).str
        let base = URL(fileURLWithPath: String(kv[0]))
            .deletingPathExtension().lastPathComponent
        hb_obj_set_str(o, "name", k.comment.str.isEmpty
                       ? base : k.comment.str)
        hb_obj_set_str(o, "keytype", k.kind.str)
        hb_obj_set_int(o, "bits", Int64(k.bits))
        hb_obj_set_str(o, "fingerprint", k.fingerprint)
        hb_obj_set_str(o, "public", k.public_line)
        hb_obj_set_bool(o, "encrypted", k.encrypted)
        Vault.shared.put(o)
        if let p = k.private_text {
            Vault.shared.setSecret(kid, "private", p.str) }
        // 导入口令不落库：首次使用时询问，由“记住”保存（与 Linux 一致）
        page(KeychainPage.self)?.reload()
        return "OK \(kid)"
    }
    private func cmdKcList() -> String {
        var arr: [[String: Any]] = []
        for o in Vault.shared.list("key") {
            arr.append(["id": hb_obj_id(o).str,
                        "name": hb_obj_get_str(o, "name").str,
                        "type": hb_obj_get_str(o, "keytype").str,
                        "bits": hb_obj_get_int(o, "bits", 0),
                        "fp": hb_obj_get_str(o, "fingerprint").str,
                        "public": hb_obj_get_str(o, "public").str])
            hb_obj_free(o)
        }
        let d = try? JSONSerialization.data(withJSONObject: arr)
        return "OK " + (d.flatMap { String(data: $0, encoding: .utf8) }
                        ?? "[]")
    }
    /// `kc-del <id>`：走级联删除核心 API（无对话框）
    private func cmdKcDel(_ arg: String) -> String {
        guard let vv = Vault.shared.vault else { return "ERR" }
        var refs: UnsafeMutablePointer<hb_key_ref>?
        var n = 0
        let e = arg.withCString {
            hb_vault_delete_key_cascade(vv, $0, &refs, &n) }
        if let refs { hb_key_refs_free(refs, n) }
        page(KeychainPage.self)?.reload()
        page(HostsPage.self)?.reload()
        return e == HB_OK ? "OK refs=\(n)" : "ERR \(e.rawValue)"
    }
    /// `kc-sel <id>`：选中凭据行（供 kc-delete 按钮用）
    private func cmdKcSel(_ arg: String) -> String {
        guard let p = page(KeychainPage.self)
        else { return "ERR no page" }
        return p.devSelect(id: arg) ? "OK" : "ERR no item"
    }
    /// `host-sel <id>`：选中主机行（供双击/菜单用）
    private func cmdHostSel(_ arg: String) -> String {
        guard let p = page(HostsPage.self)
        else { return "ERR no page" }
        return p.devSelect(id: arg) ? "OK" : "ERR no host"
    }
    /// `host-group <id> <group>`：拖到分组（与 acceptDrop 同一路径）
    private func cmdHostGroup(_ arg: String) -> String {
        let kv = arg.split(separator: " ", maxSplits: 1)
        guard let p = page(HostsPage.self), kv.count == 2
        else { return "ERR" }
        return p.devSetGroup(id: String(kv[0]),
                             group: String(kv[1])) ? "OK" : "ERR no host"
    }
    /// `host-del <id>`：选中并删除（弹撤销对话框）
    private func cmdHostDel(_ arg: String) -> String {
        guard let p = page(HostsPage.self) else { return "ERR no page" }
        return p.devDelete(id: arg) ? "OK" : "ERR no host"
    }
    /// `mode card|list`
    /// 当前 tab 的 workspace（没有 → nil）
    private var selWs: Workspace? {
        guard let w = win, w.selTab >= 0, w.selTab < w.tabs.count
        else { return nil }
        return (w.tabs[w.selTab] as? WsTab)?.ws
    }
    /// 当前选中的 tab（find dev 命令对单 pane/工作区通用）
    private var selTab: TermTab? {
        guard let w = win, w.selTab >= 0, w.selTab < w.tabs.count
        else { return nil }
        return w.tabs[w.selTab]
    }
    private func cmdWsFocusPane(_ arg: String) -> String {
        guard let ws = selWs, let i = Int(arg) else { return "ERR" }
        ws.devFocusPane(i)
        return "OK"
    }
    /// `watch-test <sample-b64> [signals]` — 规则测试器同路径（buildCfg + test_text）。
    private func cmdWatchTest(_ arg: String) -> String {
        let kv = arg.split(separator: " ")
        guard let b64 = kv.first,
              let d = Data(base64Encoded: String(b64)),
              let s = String(data: d, encoding: .utf8)
        else { return "ERR usage" }
        let signals = kv.count > 1
            ? UInt32(kv[1]) ?? WatchCtl.defaultSignals
            : WatchCtl.defaultSignals
        let hostId = win?.focusedHostId ?? selTab?.wsHostId
        guard let cfg = WatchCtl.buildCfg(hostId: hostId,
                                          signals: signals)
        else { return "ERR cfg" }
        defer { WatchCtl.freeCfg(cfg) }
        var hit = hb_watch_hit()
        guard hb_watch_test_text(cfg, s, &hit) else { return "MISS" }
        defer { hb_watch_hit_clear(&hit) }
        return "HIT rule=\(hit.rule?.str ?? "?") " +
               "ok=\(hit.ok ? 1 : 0) code=\(hit.exit_code) " +
               "line=\(hit.line?.str ?? "") " +
               "cmd=\(hit.command?.str ?? "")"
    }
    /// `snippet-new <title-b64> <body-b64> [scope]` — 建 snippet 对象。
    private func cmdSnippetNew(_ arg: String) -> String {
        let kv = arg.split(separator: " ")
        guard kv.count >= 2,
              let td = Data(base64Encoded: String(kv[0])),
              let bd = Data(base64Encoded: String(kv[1])),
              let title = String(data: td, encoding: .utf8),
              let body = String(data: bd, encoding: .utf8)
        else { return "ERR usage" }
        guard let o = hb_obj_new("snippet") else { return "ERR" }
        let sid = hb_obj_id(o).str
        hb_obj_set_str(o, "title", title)
        hb_obj_set_str(o, "body", body)
        hb_obj_set_str(o, "scope",
                       kv.count > 2 ? String(kv[2]) : "global")
        Vault.shared.put(o)
        return "OK \(sid)"
    }

    private func cmdMode(_ arg: String) -> String {
        guard let p = page(HostsPage.self) else { return "ERR no page" }
        p.devMode(list: arg.trimmingCharacters(in: .whitespaces) == "list")
        return "OK"
    }
    /// `scroll-test`：40 步滚动平均耗时（ms）
    private func cmdScrollTest() -> String {
        guard let p = page(HostsPage.self) else { return "ERR no page" }
        return "OK \(String(format: "%.2f", p.devScrollTest()))"
    }
    private func cmdDismissModal() -> String {
        for w in NSApp.windows where w !== win && w.isVisible {
            if let cell = w.defaultButtonCell {
                cell.performClick(nil)
                return "OK clicked"
            }
        }
        NSApp.stopModal()
        return "OK"
    }
}
#endif
