import AppKit
import CHarbor
import HarborKit

// ---------- 主窗口 ----------
final class MainWindow: NSWindow, NSWindowDelegate {
    let strip = TabStrip()
    let sidebar = SidebarView()
    let content = NSView()
    var tabs: [TermTab] = []
    var selTab = 0
    private var contentCard: NSView?
    private var sidebarVisible = true
    private var appearancePanel: AppearancePanel?
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

        // body
        sidebar.frame = NSRect(x: 0, y: 0, width: 172, height: 662)
        sidebar.autoresizingMask = [.height, .maxXMargin]
        contentView?.addSubview(sidebar)
        content.frame = NSRect(x: 172, y: 0, width: 928, height: 662)
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
        sidebar.onSelect = { [weak self] i in self?.showSection(i) }
        sidebar.select(0)

        newLocalTab()
        showSection(0)
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
        let t = TermTab.local(rows: 24, cols: 80)
        tabs.append(t)
        strip.tabs = tabs
        select(tabs.count - 1)
        t.start()
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
        guard i < tabs.count else { return }
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
        guard i < tabs.count else { return }
        let t = tabs[i]
        t.kill()
        t.view.removeFromSuperview()
        tabs.remove(at: i)
        strip.tabs = tabs
        if tabs.isEmpty { newLocalTab(); return }
        select(min(selTab, tabs.count - 1))
    }
    @objc func toggleSidebar() {
        sidebarVisible.toggle()
        sidebar.isHidden = !sidebarVisible
        content.frame = sidebarVisible
            ? NSRect(x: 172, y: 0, width: (contentView?.bounds.width ?? 1100)-172,
                     height: (contentView?.bounds.height ?? 700)-38)
            : NSRect(x: 0, y: 0, width: contentView?.bounds.width ?? 1100,
                     height: (contentView?.bounds.height ?? 700)-38)
    }
    private var hostsPage: HostsPage?
    private var keychainPage: KeychainPage?

    /// 由主机页/快速连接起 SSH tab。
    func openSSH(hostId: String?, quickTarget: String?, title: String) {
        let t = SSHTab(hostId: hostId, quickTarget: quickTarget,
                       window: self, title: title)
        tabs.append(t)
        strip.tabs = tabs
        select(tabs.count - 1)
        applySettings()
        t.connect()
    }

    func showSection(_ i: Int) {
        guard i >= 0 && i < kSections.count else { return }
        let sec = kSections[i].id
        appearancePanel?.removeFromSuperview()
        hostsPage?.removeFromSuperview(); hostsPage = nil
        keychainPage?.removeFromSuperview(); keychainPage = nil
        if sec == "hosts", let card = contentCard {
            let p = HostsPage(frame: card.bounds)
            p.autoresizingMask = [.width, .height]
            p.onConnect = { [weak self] h in
                self?.openSSH(hostId: h.id, quickTarget: nil,
                              title: h.label)
            }
            p.onQuick = { [weak self] s in
                self?.openSSH(hostId: nil, quickTarget: s, title: s)
            }
            card.addSubview(p)
            hostsPage = p
            return
        }
        if sec == "keychain", let card = contentCard {
            let p = KeychainPage(frame: card.bounds)
            p.autoresizingMask = [.width, .height]
            card.addSubview(p)
            keychainPage = p
            return
        }
        if sec == "appearance" {
            let p = AppearancePanel()
            p.onChange = { [weak self] in self?.applySettings() }
            appearancePanel = p
            // 显示在当前 tab 上方半透明覆盖（card 非翻转，置顶靠上）
            if let card = contentCard {
                p.frame.origin = NSPoint(
                    x: 20, y: card.bounds.height - p.frame.height - 20)
                p.autoresizingMask = [.minYMargin]
                card.addSubview(p)
            }
        }
    }
    func applySettings() {
        let v = Vault.shared
        for t in tabs {
            guard let term = t.term else { continue }
            term.fontName = v.setting("term_font") ?? "Menlo"
            term.fontSize = CGFloat(
                Double(v.setting("term_font_size") ?? "14") ?? 14)
            term.lineHeight = CGFloat(
                Double(v.setting("term_line_height") ?? "1.15") ?? 1.15)
            term.cursorStyle = (Int(v.setting("term_cursor") ?? "0") ?? 0) + 1
            term.optionIsMeta = (v.setting("term_option_meta") ?? "0") == "1"
            term.theme = TermTheme.for(v.setting("term_theme"))
            term.onSizeChange = { [weak t] r, c in
                t?.session.map {
                    hb_session_resize($0.sess, Int32(r), Int32(c))
                }
            }
        }
    }
    func windowWillClose(_ n: Notification) {
        for t in tabs { t.session?.free_() }
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
