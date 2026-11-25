import AppKit
import HarborKit

/// 终端查找条（对齐 linux hb-find-bar.c）：
///   entry + n/总数 + ↑↓ 循环 + ×；
///   Enter=下一个 ⇧Enter=上一个；Esc 关条不进 session。
/// 绑定当前焦点 pane 的 TerminalView（Workspace.findHost 承载）。
final class FindBar: NSView, NSTextFieldDelegate {
    private weak var term: TerminalView?
    var onClose: (() -> Void)?
    var onChange: ((Int, Int) -> Void)?   // dev hook 用

    private let fieldWrap = NSView()
    private let searchIco = NSImageView()
    private let field = NSTextField()
    private let count = NSTextField(labelWithString: "0/0")
    private var iconBtns: [NSButton] = []

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        fieldWrap.wantsLayer = true
        fieldWrap.layer?.cornerRadius = 7
        fieldWrap.layer?.borderWidth = 1
        field.placeholderString = L("在 scrollback 中搜索…")
        field.font = .systemFont(ofSize: 12.5)
        field.focusRingType = .none
        field.isBezeled = false
        field.drawsBackground = false
        field.delegate = self
        field.setAccessibilityIdentifier("find-field")
        count.font = .systemFont(ofSize: 11.5)
        count.alignment = .center
        count.setAccessibilityIdentifier("find-count")
        func iconBtn(_ icon: String, _ act: Selector, _ aid: String,
                     _ tip: String) -> NSButton {
            let b = NSButton()
            b.bezelStyle = .texturedRounded
            b.isBordered = false
            b.target = self; b.action = act
            b.toolTip = tip
            b.identifier = NSUserInterfaceItemIdentifier("fb-\(icon)")
            b.setAccessibilityIdentifier(aid)
            return b
        }
        let pv = iconBtn("chevron-u", #selector(onPrev),
                         "find-prev", L("上一个 (⇧Enter)"))
        let nx = iconBtn("chevron-d", #selector(onNext),
                         "find-next", L("下一个 (Enter)"))
        let cl = iconBtn("x", #selector(onCloseBtn),
                         "find-close", L("关闭 (Esc)"))
        iconBtns = [pv, nx, cl]
        [fieldWrap, searchIco, field, count, pv, nx, cl].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
        }
        addSubview(fieldWrap); addSubview(count); addSubview(pv); addSubview(nx); addSubview(cl)
        fieldWrap.addSubview(searchIco); fieldWrap.addSubview(field)
        NSLayoutConstraint.activate([
            fieldWrap.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            fieldWrap.centerYAnchor.constraint(equalTo: centerYAnchor),
            fieldWrap.heightAnchor.constraint(equalToConstant: 26),
            fieldWrap.widthAnchor.constraint(lessThanOrEqualToConstant: 340),
            fieldWrap.widthAnchor.constraint(greaterThanOrEqualToConstant: 180),
            searchIco.leadingAnchor.constraint(equalTo: fieldWrap.leadingAnchor, constant: 8),
            searchIco.centerYAnchor.constraint(equalTo: fieldWrap.centerYAnchor),
            searchIco.widthAnchor.constraint(equalToConstant: 13),
            searchIco.heightAnchor.constraint(equalToConstant: 13),
            field.leadingAnchor.constraint(equalTo: searchIco.trailingAnchor, constant: 6),
            field.trailingAnchor.constraint(equalTo: fieldWrap.trailingAnchor, constant: -8),
            field.centerYAnchor.constraint(equalTo: fieldWrap.centerYAnchor),
            count.leadingAnchor.constraint(greaterThanOrEqualTo: fieldWrap.trailingAnchor, constant: 8),
            count.trailingAnchor.constraint(equalTo: pv.leadingAnchor, constant: -6),
            count.centerYAnchor.constraint(equalTo: centerYAnchor),
            count.widthAnchor.constraint(greaterThanOrEqualToConstant: 38),
            cl.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            cl.centerYAnchor.constraint(equalTo: centerYAnchor),
            cl.widthAnchor.constraint(equalToConstant: 22),
            nx.trailingAnchor.constraint(equalTo: cl.leadingAnchor, constant: -4),
            nx.centerYAnchor.constraint(equalTo: centerYAnchor),
            nx.widthAnchor.constraint(equalToConstant: 22),
            pv.trailingAnchor.constraint(equalTo: nx.leadingAnchor, constant: -4),
            pv.centerYAnchor.constraint(equalTo: centerYAnchor),
            pv.widthAnchor.constraint(equalToConstant: 22),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }

    private func applyTermTheme(_ t: TermTheme) {
        layer?.backgroundColor = t.bar.color.cgColor
        layer?.borderColor = t.line.color.cgColor
        layer?.borderWidth = 1
        fieldWrap.layer?.backgroundColor = t.line.color.cgColor
        fieldWrap.layer?.borderColor = t.border.color.cgColor
        searchIco.image = hbIcon("search", size: 13, tint: t.dim.color)
        field.textColor = t.fg.color
        field.placeholderAttributedString = NSAttributedString(
            string: L("在 scrollback 中搜索…"),
            attributes: [.foregroundColor: t.muted.color])
        count.textColor = t.dim.color
        for b in iconBtns {
            let ic = b.identifier?.rawValue.dropFirst(3) ?? ""
            b.image = hbIcon(String(ic), size: 12, tint: t.title.color)
        }
    }

    // ---- 生命周期 ----
    func bind(_ t: TerminalView) {
        term = t
        applyTermTheme(t.theme)
        // 主题切换时重取色（linux CSS var 自动跟随）
        t.onThemeChange = { [weak self, weak t] in
            guard let t else { return }
            self?.applyTermTheme(t.theme)
        }
    }
    func open() {
        field.stringValue = ""
        updateCount(0, 0)
        window?.makeFirstResponder(field)
    }
    @objc private func onCloseBtn() { onClose?() }
    @objc private func onPrev() { step(-1) }
    @objc private func onNext() { step(1) }
    private func step(_ dir: Int) {
        _ = term?.findStep(dir)
        refresh()
    }
    private func refresh() {
        guard let t = term else { return }
        let (c, n) = t.findCounts()
        updateCount(c, n)
    }
    private func updateCount(_ c: Int, _ n: Int) {
        count.stringValue = "\(c)/\(n)"
        onChange?(c, n)
    }
    /// dev hook：当前计数文本
    var countText: String { count.stringValue }
    /// dev hook：设 needle（走 controlTextDidChange 同路）
    func devSet(_ s: String) {
        field.stringValue = s
        controlTextDidChange(Notification(name: .init("dev")))
    }
    /// dev hook：上/下一条
    func devStep(_ dir: Int) { step(dir) }

    // ---- 输入 ----
    func controlTextDidChange(_ n: Notification) {
        guard let t = term else { return }
        let total = t.findSet(field.stringValue)
        if total > 0 {
            let (c, _) = t.findCounts()
            updateCount(c, total)
        } else {
            updateCount(0, 0)
        }
    }
    /// Enter/⇧Enter/Esc —— NSTextField 走 control(_:textView:doCommandBy:)
    func control(_ control: NSControl, textView: NSTextView,
                 doCommandBy sel: Selector) -> Bool {
        if sel == #selector(NSResponder.insertNewline(_:)) {
            if NSApp.currentEvent?.modifierFlags
                .contains(.shift) == true { step(-1) }
            else { step(1) }
            return true
        }
        if sel == #selector(NSResponder.cancelOperation(_:)) {
            onClose?()
            return true
        }
        return false
    }
}
