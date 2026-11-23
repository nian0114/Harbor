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
        case "hier":    return cmdHier(arg)
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
        // ---- M2b: Snippets/History/Known/Logs 页面 + 助手栏 ----
        case "snip-mode":
            guard let p = win?.vaultTab.snippetsRef
            else { return "ERR no-page" }
            p.devMode(arg); return "OK"
        case "snip-state":
            return win?.vaultTab.snippetsRef?.devState ?? "ERR no-page"
        case "snip-send":
            guard let w = win else { return "ERR" }
            w.sendSnippet(id: arg); return "OK"
        case "snip-editor":
            win?.vaultTab.snippetsRef?.devOpenEditor()
            return "OK"
        case "snip-imp":
            win?.vaultTab.snippetsRef?.importJSON(path: arg)
            return "OK"
        case "assist":
            guard let w = selWs else { return "ERR no-ws" }
            w.setAssist(arg == "on"); return "OK \(arg)"
        case "assist-state":
            guard let w = selWs
            else { return "ERR no-ws" }
            return w.assistOn ? "on" : "off"
        case "known-import": return cmdKnownImport(arg)
        case "known-export":
            guard let p = win?.vaultTab.knownRef
            else { return "ERR no-page" }
            p.exportFile(path: arg); return "OK"
        case "known-state":
            return win?.vaultTab.knownRef?.devState ?? "ERR no-page"
        case "known-del":
            guard let v = Vault.shared.vault else { return "ERR" }
            _ = arg.withCString { hb_vault_delete(v, $0) }
            win?.vaultTab.knownRef?.reload()
            return "OK"
        case "logs-state":
            return win?.vaultTab.logsRef?.devState ?? "ERR no-page"
        case "logs-export":
            let kv = arg.split(separator: " ")
            guard kv.count == 2, let p = win?.vaultTab.logsRef
            else { return "ERR usage" }
            p.export(json: kv[0] == "json", path: String(kv[1]))
            return "OK"
        case "logs-saved":
            guard let v = Vault.shared.vault,
                  let id = Int64(arg) else { return "ERR usage" }
            _ = hb_vault_log_set_saved(v, id, true)
            win?.vaultTab.logsRef?.reload()
            return "OK"
        case "hist-count":
            guard let v = Vault.shared.vault else { return "ERR" }
            let cb: hb_history_cb = { _, ud in
                ud!.assumingMemoryBound(to: Int.self).pointee += 1
                return true }
            var c = 0
            _ = hb_vault_history_foreach(v, nil, nil, cb, &c)
            return "OK \(c)"
        case "hist-clear":
            guard let v = Vault.shared.vault else { return "ERR" }
            _ = hb_vault_history_clear(v); return "OK"
        // ---- M3: SFTP ----
        case "tab-sel":                          // <i>：切换 tab
            guard let i = Int(arg), let w = win,
                  i >= 0 && i < w.tabs.count else { return "ERR" }
            w.select(i); return "OK \(i)"
        case "sftp":     return win?.sftpTab.devState ?? "ERR"
        case "sftp-pick":                        // <i> local|<host_id>
            let kv = arg.split(separator: " ", maxSplits: 1)
            guard kv.count == 2, let i = Int(kv[0]),
                  let p = sftpPane(i) else { return "ERR usage" }
            p.pick(kv[1] == "local" ? nil : String(kv[1]))
            return "OK"
        case "sftp-cd":                          // <i> <path>
            let kv = arg.split(separator: " ", maxSplits: 1)
            guard kv.count == 2, let i = Int(kv[0]),
                  let p = sftpPane(i) else { return "ERR usage" }
            p.browseTo(String(kv[1]), nav: true); return "OK"
        case "sftp-nav":                         // <i> up|back|fwd
            let kv = arg.split(separator: " ")
            guard kv.count == 2, let i = Int(kv[0]),
                  let p = sftpPane(i) else { return "ERR usage" }
            p.devNav(String(kv[1])); return "OK"
        case "sftp-filter":                      // <i> <text>
            let kv = arg.split(separator: " ", maxSplits: 1)
            guard let i = Int(kv[0]), let p = sftpPane(i)
            else { return "ERR usage" }
            p.devFilter(kv.count > 1 ? String(kv[1]) : ""); return "OK"
        case "sftp-hidden":                      // <i> on|off
            let kv = arg.split(separator: " ")
            guard kv.count == 2, let i = Int(kv[0]),
                  let p = sftpPane(i) else { return "ERR usage" }
            p.devHidden(kv[1] == "on"); return "OK"
        case "sftp-sel":                         // <i> <idx,idx,...>
            let kv = arg.split(separator: " ")
            guard kv.count == 2, let i = Int(kv[0]),
                  let p = sftpPane(i) else { return "ERR usage" }
            p.devSelect(kv[1].split(separator: ",")
                                   .compactMap { Int($0) })
            return "OK sel=\(p.selItems().count)"
        case "sftp-dbl":                         // <i> <idx>
            let kv = arg.split(separator: " ")
            guard kv.count == 2, let i = Int(kv[0]),
                  let r = Int(kv[1]), let p = sftpPane(i)
            else { return "ERR usage" }
            p.devActivate(r); return "OK"
        case "sftp-op":                          // <i> <mkdir|rename|delete|chmod|copy|open|refresh|selectall>
            let kv = arg.split(separator: " ")
            guard kv.count == 2, let i = Int(kv[0]),
                  let p = sftpPane(i) else { return "ERR usage" }
            switch kv[1] {
            case "mkdir": p.actMkdir()
            case "rename": p.actRename()
            case "delete": p.actDelete()
            case "chmod": p.actChmod()
            case "copy": p.actCopy()
            case "open": p.actOpen()
            case "refresh": p.actRefresh()
            case "selectall": p.actSelectAll()
            default: return "ERR op"
            }
            return "OK"
        case "sftp-cancel":                      // <i>：取消连接
            guard let i = Int(arg), let p = sftpPane(i)
            else { return "ERR usage" }
            p.devCancel(); return "OK"
        case "sftp-xfer":                        // <id>：cancel/retry（按行态）
            guard let id = Int(arg), let v = win?.sftpTab.sftp
            else { return "ERR usage" }
            v.devXfer(id); return "OK"
        case "sftp-clear":
            win?.sftpTab.sftp.devClearDone(); return "OK"
        /* ---- FR-06 端口转发 dev 钩子 ---- */
        case "fwd":
            return win?.vaultTab.forwardRef?.devState ?? "ERR no-page"
        case "fwd-mgr":                          // 运行态（不依赖页面）
            var s = ""
            for (id, st) in ForwardMgr.shared.devRuns {
                s += "\(id) \(st)\n"
            }
            return s.isEmpty ? "(none)" : s
        case "fwd-sw":                           // <id|idx> on|off
            let kv = arg.split(separator: " ")
            guard kv.count == 2 else { return "ERR usage" }
            guard let id = fwdRuleId(String(kv[0]))
            else { return "ERR no-rule" }
            win?.vaultTab.forwardRef?.devSwitch(id, kv[1] == "on")
            return "OK"
        case "fwd-add":                          // kind baddr bport daddr dport via [start] [auto]
            return cmdFwdAdd(arg)
        case "fwd-del":                          // <id|idx>
            guard let id = fwdRuleId(arg)
            else { return "ERR no-rule" }
            Vault.shared.delete(id); return "OK"
        case "fwd-form":                         // [id|idx]：打开新建/编辑 sheet
            let id = arg.isEmpty ? nil : fwdRuleId(arg)
            if !arg.isEmpty && id == nil { return "ERR no-rule" }
            win?.vaultTab.forwardRef?.devOpenForm(id)
            return "OK"
        /* ---- M6 账号同步 dev 钩子（对齐 linux app.sync-auth/
         *      sync-state/resolve/dev-words/objcount） ---- */
        case "sync-state":
            return (win?.vaultTab.accountRef?.devState
                    ?? "ERR no-page")
        case "sync-auth":                        // op|srv|em|pw|totp|words|newpw|extra
            return cmdSyncAuth(arg)
        case "sync-auth-result":                 // 最近一次异步结果
            return lastSyncAuthResult
        case "sync-kick":
            SyncService.shared.syncNow(); return "OK"
        case "sync-scope":                       // <scope> on|off
            let kv = arg.split(separator: " ")
            guard kv.count == 2 else { return "ERR usage" }
            SyncService.shared.setScope(String(kv[0]),
                                        kv[1] == "on")
            return "OK"
        case "resolve":                          // <id> other|keep
            let kv = arg.split(separator: " ")
            guard kv.count == 2,
                  let s = SyncService.shared.sync
            else { return "ERR usage" }
            if kv[1] == "other" {
                kv[0].withCString {
                    _ = hb_sync_conflict_take_other(s, $0) }
                SyncService.shared.syncNow()
            } else {
                kv[0].withCString {
                    _ = hb_sync_conflict_keep_local(s, $0) }
            }
            return "RESOLVE-RESULT id=\(kv[0]) mode=\(kv[1])"
        case "dev-words":                        // <24 words> → 恢复码网格
            win?.vaultTab.accountRef?.devWords(arg)
            return "OK"
        case "objcount":                         // <type> → 范围内对象数
            return "OK \(Vault.shared.list(arg).count)"
        case "mkobj":                            // type|k=v|k=v…（#i=int）
            return cmdMkobj(arg)
        case "setfield":                         // id|k=v（#i=int）
            return cmdSetfield(arg)
        case "setsecret":                        // id|field|text（@file）
            return cmdSetsecret(arg)
        case "chksecret":                        // id|field → len
            let kv = arg.components(separatedBy: "|")
            guard kv.count == 2,
                  let sec = Vault.shared.secret(kv[0], kv[1])
            else { return "CHKSECRET len=0" }
            return "CHKSECRET id=\(kv[0]) field=\(kv[1]) " +
                   "len=\(sec.count)"
        case "rd":                               // 桌面页状态 JSON
            return "OK " + (selDesktop()?.devState ?? "{}")
        case "rd-type":                          // <text>：逐字符 keysym
            selDesktop()?.devType(arg); return "OK"
        case "rd-click":                         // <x> <y> [btn]：fb 坐标
            let a = arg.split(separator: " ")
            guard a.count >= 2, let x = Int(a[0]), let y = Int(a[1])
            else { return "ERR args" }
            selDesktop()?.devClick(x, y,
                a.count > 2 ? UInt32(a[2]) ?? 1 : 1)
            return "OK"
        case "rd-move":                          // <x> <y>
            let a2 = arg.split(separator: " ")
            guard a2.count == 2, let x = Int(a2[0]), let y = Int(a2[1])
            else { return "ERR args" }
            selDesktop()?.devMove(x, y); return "OK"
        case "rd-key":                           // <keysym-hex>
            guard let s = UInt32(arg, radix: 16) else { return "ERR" }
            selDesktop()?.devKey(s); return "OK"
        case "rd-clip":                          // <text>：本地→远端
            selDesktop()?.devClip(arg); return "OK"
        case "rd-fps":                           // [sec]
            return "OK " + (selDesktop()?
                .devFps(Double(arg) ?? 0) ?? "no-desktop")
        case "quit":     NSApp.terminate(nil); return "OK"
        default: return "ERR unknown"
        }
    }

    private func sftpPane(_ i: Int) -> SftpPane? {
        let v = win?.sftpTab.sftp
        return (i == 0 || i == 1) ? v?.panes[i] : nil
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
    /// 视图树诊断：class + 窗口坐标 frame + hidden（布局回归排查）。
    private func cmdHier(_ arg: String) -> String {
        guard let root = win?.contentView?.superview ?? win?.contentView
        else { return "ERR" }
        let maxDepth = Int(arg) ?? 6
        var lines: [String] = []
        func walk(_ v: NSView, _ depth: Int) {
            guard depth <= maxDepth else { return }
            let f = v.superview.map {
                v.convert(v.bounds, to: $0) } ?? v.frame
            let wf = v.window.map { _ in
                v.convert(v.bounds, to: nil) } ?? f
            lines.append(
                "\(String(repeating: " ", count: depth * 2))" +
                "\(type(of: v)) f=\(NSStringFromRect(f))" +
                " w=\(NSStringFromRect(wf))" +
                (v.isHidden ? " H" : ""))
            v.subviews.forEach { walk($0, depth + 1) }
        }
        walk(root, 0)
        return "OK\n" + lines.joined(separator: "\n")
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
                } else if let sw = c as? NSSwitch {
                    sw.state = (text == "1" || text == "true") ? .on : .off
                    sw.sendAction(sw.action, to: sw.target)
                } else {
                    c.stringValue = text
                    // 像真实输入一样触发 controlTextDidChange（表单校验）
                    NotificationCenter.default.post(
                        name: NSControl.textDidChangeNotification,
                        object: c)
                    // 搜索框等靠 target/action 的控件：提交等效一次编辑结束
                    if c.action != nil {
                        c.sendAction(c.action, to: c.target)
                    }
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
                if let sw = c as? NSSwitch {
                    // NSSwitch 非 NSButton：performClick 不切 state
                    sw.state = sw.state == .on ? .off : .on
                    sw.sendAction(sw.action, to: sw.target)
                } else if let b = c as? NSButton { b.performClick(nil) }
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

    /// 选中的桌面页（或第一个 DesktopTab）。
    private func selDesktop() -> DesktopTab? {
        if let w = win {
            if let d = w.tabs[w.selTab] as? DesktopTab { return d }
            return w.tabs.compactMap { $0 as? DesktopTab }.first
        }
        return nil
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
    /// `fwd-add <kind> <baddr> <bport> <daddr> <dport> <via> [start] [auto]`
    /// → "OK <id>"；daddr/dport 用 "-" 表 dynamic 置空。
    private func cmdFwdAdd(_ arg: String) -> String {
        let kv = arg.split(separator: " ")
        guard kv.count >= 6, let bp = Int64(kv[2]),
              let o = Vault.shared.newObj("forward")
        else { return "ERR usage" }
        hb_obj_set_str(o, "kind", String(kv[0]))
        hb_obj_set_str(o, "bind_addr", String(kv[1]))
        hb_obj_set_int(o, "bind_port", bp)
        if kv[0] != "dynamic" {
            hb_obj_set_str(o, "dest_addr",
                           kv[3] == "-" ? "" : String(kv[3]))
            hb_obj_set_int(o, "dest_port", Int64(kv[4]) ?? 0)
        }
        if kv[5] != "-" {
            hb_obj_set_str(o, "via", String(kv[5]))
        }
        hb_obj_set_int(o, "order",
                       Int64(Date().timeIntervalSince1970))
        hb_obj_set_bool(o, "autostart", kv.count > 7 && kv[7] == "auto")
        let start = kv.count > 6 && kv[6] == "start"
        hb_obj_set_bool(o, "enabled", start)
        Vault.shared.put(o)
        let id = hb_obj_id(o).str
        hb_obj_free(o)
        if start { ForwardMgr.shared.start(id) }
        return "OK \(id)"
    }
    // ---- M6 sync dev ----
    private var lastSyncAuthResult = "(none)"
    /// `sync-auth op|server|email|pw|totp|words|newpw|extra` —
    /// 对齐 linux app.sync-auth；结果异步落 lastSyncAuthResult。
    private func cmdSyncAuth(_ arg: String) -> String {
        let f = arg.components(separatedBy: "|")
        guard !f.isEmpty else { return "ERR bad-op" }
        let s = SyncService.shared
        func fld(_ i: Int) -> String? { i < f.count ? f[i] : nil }
        func devCb(_ r: SyncService.OpResult) {
            var t = "SYNC-AUTH-RESULT ok=\(r.ok ? 1 : 0) " +
                    "need_merge=\(r.needMerge ? 1 : 0) " +
                    "code=\(r.code ?? "-") msg=\(r.message ?? "-")"
            if let w = r.words { t += "\nSYNC-AUTH-WORDS \(w)" }
            if let sec = r.totpSecret {
                t += "\nSYNC-AUTH-TOTP secret=\(sec) " +
                     "uri=\(r.totpUri ?? "-")"
            }
            if let ds = r.devices {
                for d in ds {
                    t += "\nDEVICE id=\(d.id) name=\(d.name) " +
                         "platform=\(d.platform) current=\(d.current)"
                }
            }
            lastSyncAuthResult = t
            // dev 驱动：need_merge 自动确认合并（同 linux dev_auth_cb）
            if r.needMerge { s.confirmMerge(true) }
        }
        switch f[0] {
        case "login" where fld(3) != nil:
            s.login(server: f[1], email: f[2], password: f[3],
                    totp: fld(4), cb: devCb)
        case "register" where fld(3) != nil:
            s.register(server: f[1], email: f[2], password: f[3],
                       cb: devCb)
        case "recover" where fld(5) != nil && fld(6) != nil:
            s.recover(server: f[1], email: f[2], words: f[5],
                      newPassword: f[6], cb: devCb)
        case "rotate" where fld(3) != nil:
            s.rotateRecovery(password: f[3], cb: devCb)
        case "password" where fld(3) != nil && fld(6) != nil:
            s.changePassword(old: f[3], new: f[6], cb: devCb)
        case "logout":
            s.logout(fld(7) == "wipe")
            lastSyncAuthResult = "SYNC-AUTH-RESULT ok=1 logout"
        case "totp-setup":
            s.totpSetup(cb: devCb)
        case "totp-enable" where fld(7) != nil:
            s.totpEnable(f[7], cb: devCb)
        case "totp-disable" where fld(7) != nil:
            s.totpDisable(f[7], cb: devCb)
        case "revoke" where fld(7) != nil:
            s.revoke(f[7], wipe: fld(4) == "wipe", cb: devCb)
        case "devices":
            s.devices(cb: devCb)
        case "kick":
            s.syncNow()
        default:
            return "ERR bad-op"
        }
        return "OK pending"
    }
    /// `mkobj type|k=v|k=v…` → 建 vault 对象返回 id（对齐 linux mkobj）。
    private func cmdMkobj(_ arg: String) -> String {
        let f = arg.components(separatedBy: "|")
        guard let type = f.first, !type.isEmpty,
              let o = Vault.shared.newObj(type)
        else { return "ERR usage" }
        defer { hb_obj_free(o) }
        for kv in f.dropFirst() {
            guard let eq = kv.firstIndex(of: "=") else { continue }
            let k = String(kv[kv.startIndex..<eq])
            var v = String(kv[kv.index(after: eq)...])
            if v.hasSuffix("#i") {
                v.removeLast(2)
                hb_obj_set_int(o, k, Int64(v) ?? 0)
            } else {
                hb_obj_set_str(o, k, v)
            }
        }
        Vault.shared.put(o)
        return "MKOBJ-RESULT id=\(hb_obj_id(o).str) type=\(type)"
    }
    /// `setfield id|k=v`（#i=int，对齐 linux setfield）。
    private func cmdSetfield(_ arg: String) -> String {
        let f = arg.components(separatedBy: "|")
        guard f.count == 2, let eq = f[1].firstIndex(of: "="),
              let o = Vault.shared.get(f[0])
        else { return "ERR" }
        defer { hb_obj_free(o) }
        let k = String(f[1][f[1].startIndex..<eq])
        var v = String(f[1][f[1].index(after: eq)...])
        if v.hasSuffix("#i") {
            v.removeLast(2)
            hb_obj_set_int(o, k, Int64(v) ?? 0)
        } else {
            hb_obj_set_str(o, k, v)
        }
        Vault.shared.put(o)
        return "SETFIELD-RESULT id=\(f[0]) ok=1"
    }
    /// `setsecret id|field|text`（text 以 @ 开头=读文件，对齐 linux）。
    private func cmdSetsecret(_ arg: String) -> String {
        let f = arg.components(separatedBy: "|")
        guard f.count == 3 else { return "ERR usage" }
        var text = f[2]
        if text.hasPrefix("@"),
           let d = FileManager.default.contents(
               atPath: String(text.dropFirst())),
           let s = String(data: d, encoding: .utf8) {
            text = s
        }
        Vault.shared.setSecret(f[0], f[1], text)
        return "SETSECRET-RESULT id=\(f[0]) field=\(f[1]) ok=1"
    }
    /// <id|显示行号> → 规则 id（行号按列表顺序）。
    private func fwdRuleId(_ s: String) -> String? {
        let objs = Vault.shared.list("forward")
        defer { objs.forEach { hb_obj_free($0) } }
        if let i = Int(s), i >= 0 && i < objs.count {
            return hb_obj_id(objs[i]).str
        }
        return objs.contains(where: { hb_obj_id($0).str == s }) ? s : nil
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
    private func cmdKnownImport(_ arg: String) -> String {
        guard let p = win?.vaultTab.knownRef
        else { return "ERR no-page" }
        p.importFile(path: arg)
        return "OK"
    }

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
