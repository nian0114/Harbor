import AppKit
import CHarbor
import HarborKit

/// 固定的 Vault 标签页（prototype：pinned，不可关/不可拖走）。
/// 视图 = 侧栏 + 分区内容；分区页（主机/密钥/外观…）挂在 sectionArea。
final class VaultTab: TermTab {
    override var onlineCount: Int { 0 }
    override var wakeReconnectable: Bool { false }
    let sidebar = SidebarView()
    let sectionArea = NSView()
    private(set) var sidebarVisible = true
    private var lastSection = -1
    private var hostsPage: HostsPage?
    private var keychainPage: KeychainPage?
    private var appearancePanel: AppearancePanel?
    private var snippetsPage: SnippetsPage?
    private var knownPage: KnownPage?
    private var logsPage: LogsPage?
    private var forwardPage: ForwardPage?
    private var sharePage: SharePage?
    private var accountPage: AccountPage?

    init() {
        super.init(title: L("Vault"), view: NSView())
        closable = false
        iconName = "server"
        let v = view
        v.wantsLayer = true
        v.layer?.hbBg = "page"
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        sectionArea.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(sidebar); v.addSubview(sectionArea)
        NSLayoutConstraint.activate([
            sidebar.leadingAnchor.constraint(equalTo: v.leadingAnchor),
            sidebar.topAnchor.constraint(equalTo: v.topAnchor),
            sidebar.bottomAnchor.constraint(equalTo: v.bottomAnchor),
            sidebar.widthAnchor.constraint(equalToConstant: 208),
            sectionArea.leadingAnchor.constraint(
                equalTo: sidebar.trailingAnchor),
            sectionArea.trailingAnchor.constraint(equalTo: v.trailingAnchor),
            sectionArea.topAnchor.constraint(equalTo: v.topAnchor),
            sectionArea.bottomAnchor.constraint(equalTo: v.bottomAnchor),
        ])
    }

    var onSSH: ((String?, String?, String) -> Void)?

    func toggleSidebar() { sidebarVisible.toggle(); sidebar.isHidden = !sidebarVisible }

    func showSection(_ i: Int) {
        guard i >= 0 && i < kSections.count else { return }
        lastSection = i
        // 分区页按 area.bounds 一次性排固定帧：视图树未排布时
        // bounds 为空 → 子帧全算错（搜索框出界、滚动区负高）。
        // 先求解约束；仍为空（首启先于 select）→ 下次 runloop 再建。
        view.layoutSubtreeIfNeeded()
        let area = sectionArea
        guard !area.bounds.isEmpty else {
            DispatchQueue.main.async { [weak self] in
                guard let self, self.lastSection == i else { return }
                self.showSection(i)
            }
            return
        }
        let sec = kSections[i].id
        appearancePanel?.removeFromSuperview(); appearancePanel = nil
        hostsPage?.removeFromSuperview(); hostsPage = nil
        keychainPage?.removeFromSuperview(); keychainPage = nil
        snippetsPage?.removeFromSuperview(); snippetsPage = nil
        knownPage?.removeFromSuperview(); knownPage = nil
        logsPage?.removeFromSuperview(); logsPage = nil
        forwardPage?.removeFromSuperview(); forwardPage = nil
        sharePage?.removeFromSuperview(); sharePage = nil
        accountPage?.removeFromSuperview(); accountPage = nil
        if sec == "hosts" {
            let p = HostsPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            p.onConnect = { [weak self] h in
                self?.onSSH?(h.id, nil, h.label)
            }
            p.onQuick = { [weak self] s in
                self?.onSSH?(nil, s, s)
            }
            p.onSftp = { [weak self] id in
                (self?.view.window as? MainWindow)?.openSftp(hostId: id)
            }
            area.addSubview(p)
            hostsPage = p
            return
        }
        if sec == "keychain" {
            let p = KeychainPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            area.addSubview(p)
            keychainPage = p
            return
        }
        if sec == "snippets" {
            let p = SnippetsPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            p.onSendSnippet = { [weak self] id in
                (self?.view.window as? MainWindow)?.sendSnippet(id: id)
            }
            p.onHistoryToggle = { [weak self] in
                (self?.view.window as? MainWindow)?.applyHistorySetting()
            }
            p.hostLabel = { id in
                guard let h = Vault.shared.get(id) else { return id }
                defer { hb_obj_free(h) }
                let n = hb_obj_get_str(h, "name").str
                let a = hb_obj_get_str(h, "addr").str
                return n.isEmpty ? (a.isEmpty ? id : a)
                    : (a.isEmpty ? n : "\(n) (\(a))")
            }
            area.addSubview(p)
            snippetsPage = p
            return
        }
        if sec == "knownhosts" {
            let p = KnownPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            area.addSubview(p)
            knownPage = p
            return
        }
        if sec == "logs" {
            let p = LogsPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            area.addSubview(p)
            logsPage = p
            return
        }
        if sec == "fwd" {
            let p = ForwardPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            area.addSubview(p)
            forwardPage = p
            return
        }
        if sec == "shared" {
            let p = SharePage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            area.addSubview(p)
            sharePage = p
            return
        }
        if sec == "account" {
            let p = AccountPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            area.addSubview(p)
            accountPage = p
            return
        }
        if sec == "appearance" {
            let p = AppearancePanel()
            p.onChange = { [weak self] in
                (self?.view.window as? MainWindow)?.applySettings()
            }
            appearancePanel = p
            p.frame.origin = NSPoint(
                x: 20, y: area.bounds.height - p.frame.height - 20)
            p.autoresizingMask = [.minYMargin]
            area.addSubview(p)
        }
    }
    func refreshHosts() { hostsPage?.reload() }
    var snippetsRef: SnippetsPage? { snippetsPage }
    var knownRef: KnownPage? { knownPage }
    var forwardRef: ForwardPage? { forwardPage }
    var accountRef: AccountPage? { accountPage }
    var logsRef: LogsPage? { logsPage }
    /// dev hook 找页面（page(HostsPage.self) 走 contentCard 子树）
    /// SFTP 连接失败卡“编辑主机”入口：临时 HostStore 取 rec → 同
    /// hosts 页的编辑 sheet（HostForm + apply 写回同一对象）。
    func editHostById(_ id: String) {
        guard let w = view.window else { return }
        let store = HostStore()
        store.reload()
        guard let h = store.hosts.first(where: { $0.id == id }) else {
            return
        }
        let sheet = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 640),
            styleMask: [.titled], backing: .buffered, defer: false)
        sheet.title = L("编辑主机")
        sheet.isReleasedWhenClosed = false
        let form = HostForm(frame: sheet.contentView!.bounds)
        form.fill(h)
        form.onSave = { rec, pw in
            let o = store.apply(rec)
            if let pw {
                Vault.shared.setSecret(hb_obj_id(o).str, "password", pw)
            }
            w.endSheet(sheet)
        }
        form.onCancel = { w.endSheet(sheet) }
        sheet.contentView = form
        w.beginSheet(sheet) { _ in }
    }

    func page<T: NSView>(_ t: T.Type) -> T? {
        func walk(_ v: NSView) -> T? {
            for s in v.subviews {
                if let m = s as? T { return m }
                if let m = walk(s) { return m }
            }
            return nil
        }
        return walk(sectionArea)
    }
}

/// 固定的 SFTP 标签页（pinned）：SftpView 双栏浏览器。
final class SFTPTab: TermTab {
    let sftp: SftpView
    init() {
        let v = SftpView()
        sftp = v
        super.init(title: "SFTP", view: v)
        closable = false
        iconName = "folder"
        sftp.tab = self
        sftp.hostEditor = { [weak self] id in
            (self?.view.window as? MainWindow)?.editHostById(id)
        }
    }
    /// linux hb_sftp_view_open_host：左 Local，右该 host。
    func openHost(_ id: String) {
        sftp.panes[0].pick(nil)
        sftp.panes[1].pick(id)
    }
    /// 远端 pane 的 ConnCtl 集合（日志行收尾/断开用）
    var devState: String { sftp.devState }
    override var running: Bool {
        panesAlive() || sftp.xers.contains { $0.state == .run }
    }
    private func panesAlive() -> Bool {
        sftp.panes.contains { $0.sess != nil || $0.ctl != nil }
    }
    /// linux hb_sftp_view_online：每个已连接的远端 pane +1。
    override var onlineCount: Int {
        sftp.panes.filter { $0.sess != nil }.count
    }
    /// linux hb_sftp_view_disconnect_all：远端 pane 回 select。
    override func disconnect() { sftp.disconnectAll() }
    /// 休眠唤醒不走 restart：用户重新挑选远端（linux 同）。
    override var wakeReconnectable: Bool { false }
    override func kill() {
        sftp.teardown()   // 各 pane ConnCtl cancel+join → 日志行收尾
        super.kill()
    }
    deinit { sftp.teardown() }
}
