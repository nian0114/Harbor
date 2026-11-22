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
        col.width = 168
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
        wantsLayer = true
        layer?.backgroundColor = Theme.sidebar.color.cgColor
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }
    required init?(coder: NSCoder) { nil }
    func numberOfRows(in t: NSTableView) -> Int { kSections.count }
    func tableView(_ tv: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let s = kSections[row]
        let v = NSTableCellView()
        let iv = NSImageView(image: hbIcon(s.icon, size: 15,
                                           tint: Theme.text.color))
        iv.translatesAutoresizingMaskIntoConstraints = false
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
