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
    private weak var win: MainWindow?
    let path: String

    init?(win: MainWindow) {
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
        let s = DispatchSource.makeReadSource(fileDescriptor: fd,
                                              queue: .main)
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
                                                      queue: .main)
                var buf = ""
                s.setEventHandler { [weak self] in
                    var chunk = [UInt8](repeating: 0, count: 8192)
                    let n = read(cfd, &chunk, chunk.count)
                    if n <= 0 { s.cancel(); close(cfd); return }
                    buf += String(decoding: chunk[0..<n], as: UTF8.self)
                    while let eol = buf.firstIndex(of: "\n") {
                        let line = String(buf[..<eol])
                        buf.removeSubrange(...eol)
                        let resp = self?.handle(line) ?? "ERR"
                        resp.withCString { _ = Darwin.write(cfd, $0,
                                                            strlen($0)) }
                        _ = Darwin.write(cfd, "\n", 1)
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
            if let i = Int(arg) { win?.sidebar.select(i)
                win?.showSection(i) }
            return "OK"
        case "action": return cmdAction(arg)
        case "select": return cmdSelect(arg)
        case "stall-reset":
            win?.maxRunloopStall = 0; return "OK"
        case "set":    return cmdSet(arg)
        case "state":  return cmdState()
        case "gridinfo": return cmdGridInfo()
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
        return "OK \(path)"
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
        _ = NSApp.sendAction(act, to: it.target, from: it)
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
        guard let t = win?.tabs[win?.selTab ?? 0].session else { return "ERR" }
        guard let data = Data(base64Encoded: arg) else { return "ERR b64" }
        data.withUnsafeBytes { ptr in
            hb_session_write(t.sess, ptr.baseAddress!
                .assumingMemoryBound(to: CChar.self), data.count)
        }
        return "OK"
    }
    private func cmdKey(_ arg: String) -> String {
        // "ctrl+c" | "enter" | "f1" | "up" …
        guard let t = win?.tabs[win?.selTab ?? 0].session else { return "ERR" }
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
        if let kk = named[name] {
            hb_session_key(t.sess, hb_key(rawValue: UInt32(kk)), mods); return "OK"
        }
        if name.hasPrefix("f"), let n = Int32(name.dropFirst()),
           n >= 1 && n <= 12 {
            hb_session_key(t.sess, hb_key(rawValue: UInt32(KeyMap.fkey(n))), mods)
            return "OK"
        }
        if let ch = name.first, name.count == 1 {
            if mods & KeyMap.HB_MOD_CTRL != 0, let a = ch.asciiValue {
                // 真终端发的是裸控制字节，不是 CSI-u
                var b = a & 0x1f
                withUnsafeBytes(of: &b) {
                    hb_session_write(t.sess, $0.baseAddress!
                        .assumingMemoryBound(to: CChar.self), 1)
                }
            } else if let u = ch.unicodeScalars.first {
                hb_session_unichar(t.sess, u.value, mods)
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
}
#endif
