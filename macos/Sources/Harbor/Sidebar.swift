import AppKit
import CHarbor
import HarborKit

// ---------- 侧栏 ----------
final class SidebarView: NSView, NSTableViewDataSource, NSTableViewDelegate {
    var onSelect: ((Int) -> Void)?
    private let table = NSTableView()
    init() {
        super.init(frame: .zero)
        let col = NSTableColumn(identifier: .init("s"))
        col.width = 208
        table.addTableColumn(col)
        table.headerView = nil
        table.rowHeight = 30
        table.backgroundColor = .clear
        table.dataSource = self; table.delegate = self
        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        addSubview(scroll)
        // 账户页脚：未登录 / 点击登录 → 登录后显示邮箱与同步状态（M6）
        let foot = NSButton()
        foot.title = ""
        foot.isBordered = false
        foot.translatesAutoresizingMaskIntoConstraints = false
        foot.setAccessibilityIdentifier("sidebar-account")
        foot.target = self
        foot.action = #selector(accountTap)
        let f1 = NSTextField(labelWithString: L("未登录"))
        f1.font = .boldSystemFont(ofSize: 12)
        f1.translatesAutoresizingMaskIntoConstraints = false
        let f2 = NSTextField(labelWithString: L("点击登录"))
        f2.font = .systemFont(ofSize: 10)
        f2.textColor = .secondaryLabelColor
        f2.translatesAutoresizingMaskIntoConstraints = false
        acctL1 = f1; acctL2 = f2
        acctObs = NotificationCenter.default.addObserver(
            forName: .hbSyncChanged, object: nil, queue: .main) {
            [weak self] _ in self?.refreshAccount()
        }
        refreshAccount()
        foot.addSubview(f1); foot.addSubview(f2)
        let sep = NSView()
        sep.wantsLayer = true
        sep.layer?.hbBg = "line"
        sep.translatesAutoresizingMaskIntoConstraints = false
        addSubview(sep); addSubview(foot)
        wantsLayer = true
        layer?.hbBg = "sidebar"
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.bottomAnchor.constraint(equalTo: sep.topAnchor),
            sep.leadingAnchor.constraint(equalTo: leadingAnchor),
            sep.trailingAnchor.constraint(equalTo: trailingAnchor),
            sep.heightAnchor.constraint(equalToConstant: 1),
            foot.leadingAnchor.constraint(equalTo: leadingAnchor),
            foot.trailingAnchor.constraint(equalTo: trailingAnchor),
            foot.topAnchor.constraint(equalTo: sep.bottomAnchor),
            foot.bottomAnchor.constraint(equalTo: bottomAnchor),
            foot.heightAnchor.constraint(equalToConstant: 46),
            f1.leadingAnchor.constraint(equalTo: foot.leadingAnchor,
                                        constant: 14),
            f1.topAnchor.constraint(equalTo: foot.topAnchor, constant: 8),
            f2.leadingAnchor.constraint(equalTo: f1.leadingAnchor),
            f2.topAnchor.constraint(equalTo: f1.bottomAnchor, constant: 1),
        ])
    }
    required init?(coder: NSCoder) { nil }
    private weak var acctL1: NSTextField?
    private weak var acctL2: NSTextField?
    private var acctObs: NSObjectProtocol?
    deinit {
        if let o = acctObs {
            NotificationCenter.default.removeObserver(o)
        }
    }
    @objc private func accountTap() {
        // 账户是侧栏最后一个分区
        onSelect?(kSections.count - 1)
    }
    private func refreshAccount() {
        let s = SyncService.shared
        if s.state == .signedOut {
            acctL1?.stringValue = L("未登录")
            acctL2?.stringValue = L("点击登录")
        } else {
            acctL1?.stringValue = s.email ?? L("账号")
            acctL2?.stringValue = s.statusText
        }
    }
    func numberOfRows(in t: NSTableView) -> Int { kSections.count }
    func tableView(_ tv: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let s = kSections[row]
        let v = NSTableCellView()
        let iv = NSImageView(image: hbIcon(s.icon, size: 15,
                                           tint: Theme.dyn("text")))
        iv.translatesAutoresizingMaskIntoConstraints = false
        // kSections 全局表在 Lang.apply 前可能已求值，渲染处再查
        // 一次（幂等：已译串查表不中原样返回）。
        // i18n-dynamic
        let tf = NSTextField(labelWithString: L(s.title))
        tf.font = .systemFont(ofSize: 12.5)
        tf.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(iv); v.addSubview(tf)
        NSLayoutConstraint.activate([
            iv.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 10),
            iv.centerYAnchor.constraint(equalTo: v.centerYAnchor),
            iv.widthAnchor.constraint(equalToConstant: 15),
            iv.heightAnchor.constraint(equalToConstant: 15),
            tf.leadingAnchor.constraint(equalTo: iv.trailingAnchor,
                                        constant: 8),
            tf.centerYAnchor.constraint(equalTo: v.centerYAnchor),
        ])
        return v
    }
    func tableViewSelectionDidChange(_ n: Notification) {
        onSelect?(table.selectedRow) }
    func select(_ i: Int) {
        table.selectRowIndexes([i], byExtendingSelection: false) }
}
