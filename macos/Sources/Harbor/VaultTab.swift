import AppKit
import CHarbor
import HarborKit

/// 固定的 Vault 标签页（prototype：pinned，不可关/不可拖走）。
/// 视图 = 侧栏 + 分区内容；分区页（主机/密钥/外观…）挂在 sectionArea。
final class VaultTab: TermTab {
    let sidebar = SidebarView()
    let sectionArea = NSView()
    private(set) var sidebarVisible = true
    private var lastSection = -1
    private var hostsPage: HostsPage?
    private var keychainPage: KeychainPage?
    private var appearancePanel: AppearancePanel?

    init() {
        super.init(title: L("Vault"), view: NSView())
        closable = false
        iconName = "server"
        let v = view
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.page.color.cgColor
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        sectionArea.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(sidebar); v.addSubview(sectionArea)
        NSLayoutConstraint.activate([
            sidebar.leadingAnchor.constraint(equalTo: v.leadingAnchor),
            sidebar.topAnchor.constraint(equalTo: v.topAnchor),
            sidebar.bottomAnchor.constraint(equalTo: v.bottomAnchor),
            sidebar.widthAnchor.constraint(equalToConstant: 172),
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
        if sec == "hosts" {
            let p = HostsPage(frame: area.bounds)
            p.autoresizingMask = [.width, .height]
            p.onConnect = { [weak self] h in
                self?.onSSH?(h.id, nil, h.label)
            }
            p.onQuick = { [weak self] s in
                self?.onSSH?(nil, s, s)
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
    /// dev hook 找页面（page(HostsPage.self) 走 contentCard 子树）
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

/// 固定的 SFTP 占位标签页（pinned，M3 前占位）。
final class SFTPTab: TermTab {
    init() {
        super.init(title: "SFTP", view: NSView())
        closable = false
        iconName = "folder"
        let l = NSTextField(labelWithString: L("SFTP（开发中）"))
        l.font = .systemFont(ofSize: 14)
        l.textColor = .secondaryLabelColor
        l.translatesAutoresizingMaskIntoConstraints = false
        view.wantsLayer = true
        view.layer?.backgroundColor = Theme.page.color.cgColor
        view.addSubview(l)
        NSLayoutConstraint.activate([
            l.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            l.centerYAnchor.constraint(equalTo: view.centerYAnchor),
        ])
    }
}
