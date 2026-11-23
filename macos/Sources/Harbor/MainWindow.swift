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
        let bell = NSButton()
        bell.bezelStyle = .texturedRounded
        bell.image = hbIcon("bell", size: 16, tint: .white)
        bell.isBordered = false
        bell.frame = NSRect(x: 1056, y: 8, width: 28, height: 22)
        bell.autoresizingMask = [.minXMargin]
        bar.addSubview(bell)

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
        strip.tabs = tabs
        select(tabs.count - 1)
        leaf.start()
        applySettings()
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

    /// 由主机页/快速连接起 SSH tab。
    func openSSH(hostId: String?, quickTarget: String?, title: String) {
        let leaf = SSHTab(hostId: hostId, quickTarget: quickTarget,
                          window: self, title: title)
        let t = WsTab(leaf: leaf)
        tabs.append(t)
        strip.tabs = tabs
        select(tabs.count - 1)
        applySettings()
        leaf.connect()
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
        strip.tabs = tabs
        select(tabs.count - 1)
        applySettings()
        for l in leaves {
            if let s = l as? SSHTab { s.connect() }
            else { l.start() }
        }
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
