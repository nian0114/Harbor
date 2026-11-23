import AppKit
import CHarbor
import HarborKit

// ---------- 主窗口 ----------
final class MainWindow: NSWindow, NSWindowDelegate {
    let strip = TabStrip()
    let vaultTab = VaultTab()
    let sftpTab = SFTPTab()
    let content = NSView()
    var tabs: [TermTab] = []
    var selTab = 0
    private var contentCard: NSView?
    var maxRunloopStall: TimeInterval = 0
    private var stallTimer: Timer?
    private var lastBeat = Date()
    // 通知中心：铃铛 + 未读角标 + 弹层
    private let bell = NSButton()
    private let bellBadge = NSTextField(labelWithString: "")
    private var notifPanel: NotifyPanel?
    private var notifMonitor: Any?

    init() {
        super.init(contentRect: NSRect(x: 200, y: 200, width: 1100, height: 700),
                   styleMask: [.titled, .closable, .miniaturizable,
                               .resizable, .fullSizeContentView],
                   backing: .buffered, defer: false)
        isRestorable = false   // 自管理 tab；避免系统崩溃恢复弹窗
        titlebarAppearsTransparent = true
        titleVisibility = .hidden
        isMovableByWindowBackground = true
        delegate = self
        minSize = NSSize(width: 640, height: 420)

        // titlebar row: strip + sidebar toggle + bell
        let bar = NSView(frame: NSRect(x: 0, y: 662, width: 1100, height: 38))
        bar.wantsLayer = true
        bar.layer?.backgroundColor = Theme.titlebar.color.cgColor
        contentView?.addSubview(bar)
        bar.autoresizingMask = [.width, .minYMargin]
        strip.frame = NSRect(x: 78, y: 0, width: 902, height: 38)
        strip.autoresizingMask = [.width]
        bar.addSubview(strip)
        let side = NSButton()
        side.bezelStyle = .texturedRounded
        side.image = hbIcon("panel", size: 16, tint: .white)
        side.isBordered = false
        side.frame = NSRect(x: 992, y: 8, width: 28, height: 22)
        side.autoresizingMask = [.minXMargin]
        side.target = self; side.action = #selector(toggleSidebar)
        bar.addSubview(side)
        bell.bezelStyle = .texturedRounded
        bell.image = hbIcon("bell", size: 16, tint: .white)
        bell.isBordered = false
        bell.frame = NSRect(x: 1056, y: 8, width: 28, height: 22)
        bell.autoresizingMask = [.minXMargin]
        bell.target = self
        bell.action = #selector(toggleNotifs)
        bell.setAccessibilityIdentifier("bell")
        bar.addSubview(bell)
        // 角标钉在铃铛右上内缘（不贴窗口圆角边缘，避免被裁）
        bellBadge.frame = NSRect(x: 1068, y: 17, width: 16, height: 13)
        bellBadge.autoresizingMask = [.minXMargin]
        bellBadge.wantsLayer = true
        bellBadge.layer?.cornerRadius = 6
        bellBadge.layer?.backgroundColor = Theme.red.color.cgColor
        bellBadge.isHidden = true
        bar.addSubview(bellBadge)
        NotifyCenter.shared.onChange = { [weak self] in
            self?.refreshBellBadge()
        }
        refreshBellBadge()

        // body：Vault/SFTP pinned + 会话 tab 全宽；侧栏属 Vault tab 内部
        content.frame = NSRect(x: 0, y: 0, width: 1100, height: 662)
        content.autoresizingMask = [.width, .height]
        content.wantsLayer = true
        content.layer?.backgroundColor = Theme.page.color.cgColor
        contentView?.addSubview(content)

        strip.onSelect = { [weak self] i in self?.select(i) }
        strip.onClose  = { [weak self] i in self?.closeTab(i) }
        strip.onNew    = { [weak self] in self?.newLocalTab() }
        strip.onReorder = { [weak self] a, b in
            guard let self else { return }
            let t = self.tabs.remove(at: a)
            self.tabs.insert(t, at: b)
            self.select(b)
        }
        vaultTab.sidebar.onSelect = { [weak self] i in self?.showSection(i) }
        vaultTab.sidebar.select(0)
        vaultTab.onSSH = { [weak self] id, q, t in self?.openSSH(hostId: id, quickTarget: q, title: t) }

        // prototype：Vault/SFTP pinned 在前；启动停在 Vault（主机）
        tabs = [vaultTab, sftpTab]
        strip.tabs = tabs
        select(0)
        vaultTab.showSection(0)
        applySettings()
        /* FR-06：窗口就绪后恢复 enabled 规则（linux 启动拉起同）。
         * 对话框（key-confirm/密码）以本窗为 parent。 */
        ForwardMgr.shared.window = self
        ForwardMgr.shared.startAll()

        // 主 runloop 卡顿计量（perf 验收用）
        stallTimer = Timer.scheduledTimer(withTimeInterval: 0.05,
                                          repeats: true) { [weak self] _ in
            guard let self else { return }
            let gap = Date().timeIntervalSince(self.lastBeat) - 0.05
            if gap > self.maxRunloopStall { self.maxRunloopStall = gap }
            self.lastBeat = Date()
        }
    }

    func newLocalTab() {
        let leaf = TermTab.local(rows: 24, cols: 80)
        let t = WsTab(leaf: leaf)
        tabs.append(t)
        t.onChange = { [weak self] in self?.strip.needsDisplay = true }
        strip.tabs = tabs
        select(tabs.count - 1)
        applySettings()
        t.ws.layoutNow()          // term 真实 frame → 正确 rows/cols
        leaf.startSession()
        t.term?.window?.makeFirstResponder(t.term)
    }
    func select(_ i: Int) {
        guard i >= 0 && i < tabs.count else { return }
        selTab = i
        strip.selected = i
        contentCard?.removeFromSuperview()
        let card = tabs[i].view
        card.frame = content.bounds.insetBy(dx: 8, dy: 8)
        card.autoresizingMask = [.width, .height]
        card.wantsLayer = true
        card.layer?.cornerRadius = 8
        card.layer?.backgroundColor = Theme.card.color.cgColor
        content.addSubview(card)
        contentCard = card
        makeFirstResponder(tabs[i].term)
        // 切到该 tab → 清 watch 角标（对齐 linux refresh_active 清 dot）
        if let w = tabs[i] as? WsTab {
            for l in w.ws.leavesList where l.watchBadge != nil {
                l.watchBadge = nil
                l.onChange?()
            }
        }
        strip.needsDisplay = true
    }
    func closeTab(_ i: Int) {
        guard i < tabs.count, tabs[i].closable else { return }   // pinned 不可关
        let t = tabs[i]
        if t.running {
            let a = NSAlert()
            a.messageText = L("会话仍在运行")
            a.informativeText = L("关闭标签页将终止其中的进程。")
            a.addButton(withTitle: L("关闭"))
            a.addButton(withTitle: L("取消"))
            DialogRegistry.shared.present(alert: a, kind: "close-tab",
                                          in: self) { [weak self] ok in
                guard ok, let self else { return }
                self.closeTabNow(i)
            }
            return
        }
        closeTabNow(i)
    }
    private func closeTabNow(_ i: Int) {
        guard i < tabs.count, tabs[i].closable else { return }
        let t = tabs[i]
        t.kill()
        t.view.removeFromSuperview()
        tabs.remove(at: i)
        strip.tabs = tabs
        select(min(selTab, tabs.count - 1))
    }
    @objc func toggleSidebar() {
        // prototype：仅 Vault tab 激活时切换侧栏；会话 tab 上无操作
        if selTab == 0 { vaultTab.toggleSidebar() }
    }

    // ---- 通知中心（FR-13，对齐 hb-window.c build_bell） ----
    @objc private func toggleNotifs() {
        if let p = notifPanel {
            closeNotifs(p)
            return
        }
        let p = NotifyPanel()
        p.onJump = { [weak self] ti in
            guard let self else { return }
            self.select(min(ti, self.tabs.count - 1))
            self.closeNotifs(self.notifPanel)
        }
        p.onClose = { [weak self] in self?.closeNotifs(self?.notifPanel) }
        // 锚在铃铛下缘
        let pt = bell.convert(NSPoint(x: bell.bounds.maxX, y: 0),
                              to: contentView)
        p.frame = NSRect(x: pt.x - p.frame.width - 2,
                         y: pt.y - p.frame.height - 4,
                         width: p.frame.width, height: p.frame.height)
        p.autoresizingMask = [.minXMargin, .minYMargin]
        contentView?.addSubview(p)
        notifPanel = p
        // 面板外点击 → 关闭
        notifMonitor = NSEvent.addLocalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown]) { [weak self] e in
                guard let self, let p = self.notifPanel else { return e }
                let loc = p.convert(e.locationInWindow, from: nil)
                if !p.bounds.contains(loc) && e.window === self {
                    self.closeNotifs(p)
                }
                return e
            }
    }
    private func closeNotifs(_ p: NotifyPanel?) {
        p?.removeFromSuperview()
        if let m = notifMonitor {
            NSEvent.removeMonitor(m)
            notifMonitor = nil
        }
        if p === notifPanel { notifPanel = nil }
    }
    private func refreshBellBadge() {
        let n = NotifyCenter.shared.unread
        bellBadge.isHidden = n == 0
        bellBadge.stringValue = n > 99 ? "99" : "\(n)"
        bellBadge.font = .systemFont(ofSize: 8, weight: .bold)
        bellBadge.textColor = .white
        bellBadge.alignment = .center
        notifPanel?.rebuild()
    }

    /// 由主机页/快速连接起 SSH tab。
    /// 主机 ctx/抽屉 SFTP（linux hb_host_action_sftp）：
    /// 左 Local + 右 host，并切到 SFTP tab。
    func openSftp(hostId: String) {
        sftpTab.openHost(hostId)
        select(1)
    }
    /// SFTP 连接失败卡的“编辑主机”：复用 hosts 页的编辑表单。
    func editHostById(_ id: String) {
        vaultTab.editHostById(id)
    }

    /// rdp/vnc 主机 → 桌面 tab（hosts 页/命令面板/cmdConnect 共用入口）。
    /// 桌面页无终端，不进 Workspace（linux 同样是独立 page 类型）。
    func openDesktop(hostId: String, title: String) {
        let leaf = DesktopTab(hostId: hostId, title: title)
        tabs.append(leaf)
        leaf.onChange = { [weak self] in self?.strip.needsDisplay = true }
        strip.tabs = tabs
        select(tabs.count - 1)
        applySettings()
        leaf.startSession()     // view 已入 content、尺寸定稿
    }

    func openSSH(hostId: String?, quickTarget: String?, title: String) {
        if let hostId, let o = Vault.shared.get(hostId) {
            let proto = hb_obj_get_str(o, "proto").str
            hb_obj_free(o)
            if proto == "rdp" || proto == "vnc" {
                openDesktop(hostId: hostId, title: title)
                return
            }
        }
        let leaf = SSHTab(hostId: hostId, quickTarget: quickTarget,
                          window: self, title: title)
        let t = WsTab(leaf: leaf)
        tabs.append(t)
        t.onChange = { [weak self] in self?.strip.needsDisplay = true }
        strip.tabs = tabs
        select(tabs.count - 1)
        applySettings()
        t.ws.layoutNow()
        leaf.startSession()
    }

    /// Workspace 选择器：2–4 目标一次铺一行（对齐 hb_workspace_pick_targets）。
    func openWorkspacePicker() {
        WsPickSheet(window: self).present()
    }
    /// 创建多 pane Workspace tab（picker 回调 / 命令面板）。
    func openWorkspace(leaves: [TermTab]) {
        guard !leaves.isEmpty else { return }
        let t = WsTab(leaves: leaves)
        tabs.append(t)
        t.onChange = { [weak self] in self?.strip.needsDisplay = true }
        strip.tabs = tabs
        select(tabs.count - 1)
        applySettings()
        t.ws.layoutNow()
        for l in leaves { l.startSession() }
    }

    // ---- 工作区动作（菜单/快捷键/命令面板共用入口） ----
    private var selWs: Workspace? { (tabs[selTab] as? WsTab)?.ws }
    /// 普通会话 tab → 单叶 WsTab（对齐 linux 首次 split 自动包工作区）。
    private func ensureWorkspace() {
        guard selTab >= 0 && selTab < tabs.count else { return }
        let t = tabs[selTab]
        guard !(t is WsTab), t.closable, t.term != nil else { return }
        let w = WsTab(leaf: t)
        tabs[selTab] = w
        strip.tabs = tabs
        select(selTab)
    }
    func wsSplitRight() { wsSplit(true) }
    func wsSplitDown() { wsSplit(false) }
    private func wsSplit(_ h: Bool) {
        if selWs == nil { ensureWorkspace() }
        _ = selWs?.splitFocused(horizontal: h)
        applySettings()
        strip.needsDisplay = true
    }
    func wsClosePane() {
        if let ws = selWs {
            ws.closeFocused()
            strip.needsDisplay = true
        } else {
            closeTab(selTab)   // 单 pane：关 pane = 关 tab（对齐 linux）
        }
    }
    func wsBroadcast() {
        selWs?.setBroadcast(!(selWs?.broadcast ?? false))
    }
    func wsFocusMode() { selWs?.toggleFocusMode() }
    func wsMove(_ dir: Int) { selWs?.moveFocus(dir: dir) }
    func wsFind() {
        guard selTab >= 0 && selTab < tabs.count else { return }
        tabs[selTab].openFind()
    }

    // ---- 命令面板 ⌘K（FR-20） ----
    func openPalette() { Palette.open(in: self) }
    func restoreFocusToTab() {
        if selTab >= 0 && selTab < tabs.count {
            makeFirstResponder(tabs[selTab].term)
        }
    }
    /// 焦点会话 host（snippet scope 用）
    var focusedHostId: String? {
        selWs?.focusedLeaf?.wsHostId
    }
    var focusedHostGroup: String? {
        guard let id = focusedHostId, let h = Vault.shared.get(id)
        else { return nil }
        defer { hb_obj_free(h) }
        let g = hb_obj_get_str(h, "group").str
        return g.isEmpty ? nil : g
    }

    func paletteExec(_ it: PalItem) {
        switch it.kind {
        case .host:
            openSSH(hostId: it.id, quickTarget: nil, title: it.title)
        case .section:
            if let i = Int(it.id) {
                select(0); vaultTab.showSection(i)
                vaultTab.sidebar.select(i)
            }
        case .snippet:
            sendSnippet(id: it.id)
        case .cmd:
            paletteCmd(it.id)
        }
    }
    private func paletteCmd(_ id: String) {
        switch id {
        case "local":      newLocalTab()
        case "workspace":  openWorkspacePicker()
        case "broadcast":  wsBroadcast()
        case "focus":      wsFocusMode()
        case "export-logs": exportLogs()
        case "appearance": select(0); vaultTab.showSection(7)
                           vaultTab.sidebar.select(7)
        default: break
        }
    }

    /// 导出连接日志 CSV（对齐 linux run_export_logs）。
    func exportLogs() {
        let sp = NSSavePanel()
        sp.nameFieldStringValue = "harbor-logs.csv"
        sp.allowedContentTypes = [.commaSeparatedText]
        sp.beginSheetModal(for: self) { r in
            guard r == .OK, let url = sp.url,
                  let v = Vault.shared.vault else { return }
            var csv: UnsafeMutablePointer<CChar>?
            guard hb_vault_log_export_csv(v, &csv) == HB_OK, let csv
            else { return }
            defer { free(csv) }
            try? String(cString: csv)
                .write(to: url, atomically: true, encoding: .utf8)
        }
    }

    /// Snippet 发送（对齐 hb_snippet_send）：
    ///   无变量 → 直接 paste 到焦点 session（broadcast 开 → 全 pane）；
    ///   有 {{var}} → 变量对话框（默认/记忆值），渲染后发送；
    ///   run=true → 追加回车。
    private static var snippetLastVals: [String: [String: String]] = [:]
    func sendSnippet(id: String) {
        let v = Vault.shared
        guard let snip = v.get(id) else { return }
        defer { hb_obj_free(snip) }
        let body = hb_obj_get_str(snip, "body").str
        let run = hb_obj_get_bool(snip, "run", false)
        // 变量提取
        var names: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?
        var defs: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?
        let nv = hb_snippet_vars(body, &names, &defs)
        var ns: [String] = []
        var ds: [String?] = []
        for i in 0..<nv {
            ns.append(names?[i].map { String(cString: $0) } ?? "")
            ds.append(defs?[i].map { String(cString: $0) })
        }
        if let names { hb_strfreev(names, nv) }
        if let defs { hb_strfreev(defs, nv) }
        if nv == 0 { deliverSnippet(body, run: run); return }
        // 变量对话框：每变量一行 field，默认/上次值预填
        let a = NSAlert()
        a.messageText = L("发送 Snippet")
        a.informativeText = L("填写变量值：")
        a.addButton(withTitle: L("发送"))
        a.addButton(withTitle: L("取消"))
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false
        var fields: [NSTextField] = []
        var prev: NSView = acc
        let last = MainWindow.snippetLastVals[id] ?? [:]
        for (i, n) in ns.enumerated() {
            let l = NSTextField(labelWithString: n)
            let f = NSTextField()
            f.placeholderString = ds[i] ?? ""
            f.stringValue = last[n] ?? ds[i] ?? ""
            f.setAccessibilityIdentifier("snipvar-\(n)")
            l.translatesAutoresizingMaskIntoConstraints = false
            f.translatesAutoresizingMaskIntoConstraints = false
            acc.addSubview(l); acc.addSubview(f)
            NSLayoutConstraint.activate([
                l.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
                l.widthAnchor.constraint(equalToConstant: 110),
                f.leadingAnchor.constraint(equalTo: l.trailingAnchor,
                                           constant: 8),
                f.trailingAnchor.constraint(equalTo: acc.trailingAnchor),
                l.centerYAnchor.constraint(equalTo: f.centerYAnchor),
                prev === acc
                    ? f.topAnchor.constraint(equalTo: acc.topAnchor)
                    : f.topAnchor.constraint(
                        equalTo: prev.bottomAnchor, constant: 6),
            ])
            fields.append(f)
            prev = f
        }
        NSLayoutConstraint.activate([
            acc.widthAnchor.constraint(equalToConstant: 320),
            prev.bottomAnchor.constraint(equalTo: acc.bottomAnchor),
        ])
        a.accessoryView = acc
        DialogRegistry.shared.present(alert: a, kind: "snip-vars",
                                      in: self) { ok in
            guard ok else { return }
            let vals: [String] = fields.map { $0.stringValue }
            var rec: [String: String] = [:]
            for (i, n) in ns.enumerated() { rec[n] = vals[i] }
            MainWindow.snippetLastVals[id] = rec
            // 渲染：空值回退默认（values NULL 元素 = 默认）
            var cvals: [UnsafePointer<CChar>?] = vals.map {
                $0.isEmpty ? nil : UnsafePointer(strdup($0))
            }
            var cnames: [UnsafePointer<CChar>?] = ns.map {
                UnsafePointer(strdup($0))
            }
            var text = body
            if let r = hb_snippet_render(body, &cnames, &cvals, nv) {
                text = String(cString: r)
                free(r)
            }
            for p in cnames { free(UnsafeMutablePointer(mutating: p)) }
            for p in cvals { free(UnsafeMutablePointer(mutating: p)) }
            self.deliverSnippet(text, run: run)
        }
    }
    /// paste 到焦点 session；broadcast 开 → 全 pane（对齐 linux 广播确认）。
    private func deliverSnippet(_ text: String, run: Bool) {
        let w = selWs
        var targets: [TermTab]
        if let w {
            targets = w.broadcast
                ? w.leavesList
                : (w.focusedLeaf.map { [$0] } ?? [])
        } else if selTab >= 0 && selTab < tabs.count {
            targets = [tabs[selTab]]   // 单 pane tab：送到本 tab（linux parity）
        } else {
            targets = []
        }
        // broadcast 且目标 >1 → 先确认（对齐 linux maybe_send 的广播确认）。
        if let w, w.broadcast, targets.count > 1 {
            let a = NSAlert()
            a.messageText = String(
                format: L("广播已开启：发送到全部 %d 个分屏？"),
                targets.count)
            a.addButton(withTitle: L("发送"))
            a.addButton(withTitle: L("取消"))
            DialogRegistry.shared.present(alert: a, kind: "snip-bcast",
                                          in: self) { ok in
                if ok { self.deliverConfirmed(text, run: run,
                                              targets: targets) }
            }
            return
        }
        deliverConfirmed(text, run: run, targets: targets)
    }
    private func deliverConfirmed(_ text: String, run: Bool,
                                  targets: [TermTab]) {
        var sent = false
        for l in targets {
            guard let s = l.session, s.alive else { continue }
            text.withCString { hb_session_paste(s.sess, $0) }
            if run {
                // HB_KEY_ENTER
                hb_session_key(s.sess, hb_key(rawValue: 1), 0)
            }
            sent = true
        }
        if !sent {
            let a = NSAlert()
            a.messageText = L("没有活动终端")
            DialogRegistry.shared.present(alert: a, kind: "toast",
                                          in: self) { _ in }
        }
    }

    /// history_enabled 变化 → 应用到所有已开 session（Snippets 页开关）。
    func applyHistorySetting() {
        for t in tabs { t.applyHistoryEnabled() }
    }

    /// 侧栏选区 → 切到 Vault tab 并展示分区（dev select-sidebar 同路）。
    func showSection(_ i: Int) {
        guard i >= 0 && i < kSections.count else { return }
        if selTab != 0 { select(0) }
        vaultTab.showSection(i)
    }
    func refreshHosts() { vaultTab.refreshHosts() }
    func applySettings() {
        let v = Vault.shared
        for t in tabs {
            for term in t.allTerms {
                term.fontName = v.setting("term_font") ?? "Menlo"
                term.fontSize = CGFloat(
                    Double(v.setting("term_font_size") ?? "14") ?? 14)
                term.lineHeight = CGFloat(
                    Double(v.setting("term_line_height") ?? "1.15") ?? 1.15)
                term.cursorStyle =
                    (Int(v.setting("term_cursor") ?? "0") ?? 0) + 1
                term.optionIsMeta =
                    (v.setting("term_option_meta") ?? "0") == "1"
                term.theme = TermTheme.for(v.setting("term_theme"))
                // 尺寸变化 resize 自己的 session（多 pane 各管各）
                term.onSizeChange = { [weak term] r, c in
                    term?.session.map {
                        hb_session_resize($0.sess, Int32(r), Int32(c))
                    }
                }
            }
        }
    }
    func windowWillClose(_ n: Notification) {
        for t in tabs { t.kill() }
    }
    func windowShouldClose(_ s: NSWindow) -> Bool {
        let live = tabs.contains { $0.running }
        if live {
            let a = NSAlert()
            a.messageText = L("仍有会话在运行")
            a.informativeText = L("退出 Harbor 将终止所有活动会话。")
            a.addButton(withTitle: L("退出"))
            a.addButton(withTitle: L("取消"))
            return a.runModal() == .alertFirstButtonReturn
        }
        return true
    }
}
