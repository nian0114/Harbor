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

    private let field = NSTextField()
    private let count = NSTextField(labelWithString: "0/0")
    private var iconBtns: [NSButton] = []

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        field.placeholderString = L("在终端中查找…")
        field.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        field.focusRingType = .none
        field.isBezeled = true
        field.bezelStyle = .roundedBezel
        field.drawsBackground = true
        field.delegate = self
        field.setAccessibilityIdentifier("find-field")
        count.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
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
        [field, count, pv, nx, cl].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            addSubview($0)
        }
        NSLayoutConstraint.activate([
            field.leadingAnchor.constraint(equalTo: leadingAnchor,
                                           constant: 12),
            field.centerYAnchor.constraint(equalTo: centerYAnchor),
            field.widthAnchor.constraint(equalToConstant: 220),
            count.leadingAnchor.constraint(
                equalTo: field.trailingAnchor, constant: 8),
            count.centerYAnchor.constraint(equalTo: centerYAnchor),
            count.widthAnchor.constraint(greaterThanOrEqualToConstant: 44),
            cl.trailingAnchor.constraint(equalTo: trailingAnchor,
                                         constant: -8),
            cl.centerYAnchor.constraint(equalTo: centerYAnchor),
            cl.widthAnchor.constraint(equalToConstant: 18),
            nx.trailingAnchor.constraint(equalTo: cl.leadingAnchor,
                                         constant: -4),
            nx.centerYAnchor.constraint(equalTo: centerYAnchor),
            nx.widthAnchor.constraint(equalToConstant: 18),
            pv.trailingAnchor.constraint(equalTo: nx.leadingAnchor,
                                         constant: -2),
            pv.centerYAnchor.constraint(equalTo: centerYAnchor),
            pv.widthAnchor.constraint(equalToConstant: 18),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }

    /// 按终端主题取色（对齐 linux .find-bar 的 --t-bar/--t-line/
    /// --t-title/--t-fg/--t-muted 变量）。
    private func applyTermTheme(_ t: TermTheme) {
        layer?.backgroundColor = t.bar.color.cgColor
        layer?.borderColor = t.line.color.cgColor
        layer?.borderWidth = 1
        field.textColor = t.fg.color
        // bezeled 控件按系统外观取色 → 主题明暗决定 field 外观
        field.appearance = NSAppearance(
            named: t.id == "light" ? .aqua : .darkAqua)
        field.placeholderAttributedString = NSAttributedString(
            string: L("在终端中查找…"),
            attributes: [.foregroundColor: t.muted.color])
        count.textColor = t.title.color
        for b in iconBtns {
            let ic = b.identifier?.rawValue.dropFirst(3) ?? ""
            b.image = hbIcon(String(ic), size: 11, tint: t.title.color)
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
