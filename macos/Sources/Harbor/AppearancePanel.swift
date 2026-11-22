import AppKit
import CHarbor
import HarborKit

// ---------- 外观与终端 设置页 ----------
final class AppearancePanel: NSView {
    override var isFlipped: Bool { true }
    private let v = Vault.shared
    var onChange: (() -> Void)?
    init() {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = Theme.content.color.cgColor
        var y: CGFloat = 14
        func row(_ label: String, _ ctrl: NSView) {
            let l = NSTextField(labelWithString: L(label))
            l.font = .systemFont(ofSize: 12)
            l.frame = NSRect(x: 24, y: y + 2, width: 140, height: 20)
            ctrl.frame = NSRect(x: 170, y: y, width: 200, height: 24)
            addSubview(l); addSubview(ctrl)
            y += 34
        }
        let fonts = NSPopUpButton()
        fonts.addItems(withTitles: ["Menlo", "Monaco", "SF Mono", "Courier New"])
        fonts.selectItem(withTitle: v.setting("term_font") ?? "Menlo")
        fonts.target = self; fonts.action = #selector(changed)
        row(L("终端字体"), fonts)
        let size = NSTextField()
        size.stringValue = v.setting("term_font_size") ?? "14"
        size.target = self; size.action = #selector(changed)
        row(L("字号"), size)
        let lh = NSTextField()
        lh.stringValue = v.setting("term_line_height") ?? "1.15"
        lh.target = self; lh.action = #selector(changed)
        row(L("行高"), lh)
        let theme = NSPopUpButton()
        theme.addItems(withTitles: TermTheme.all.map { $0.name })
        let selTheme = TermTheme.for(v.setting("term_theme")).name
        theme.selectItem(withTitle: selTheme)
        theme.tag = 1
        theme.target = self; theme.action = #selector(changed)
        row(L("终端配色"), theme)
        let sb = NSTextField()
        sb.stringValue = v.setting("term_scrollback") ?? "10000"
        sb.target = self; sb.action = #selector(changed)
        row(L("回滚行数"), sb)
        let cursor = NSPopUpButton()
        cursor.tag = 2
        cursor.addItems(withTitles: [L("块"), L("下划线"), L("竖线")])
        cursor.selectItem(at: Int(v.setting("term_cursor") ?? "0") ?? 0)
        cursor.target = self; cursor.action = #selector(changed)
        row(L("光标样式"), cursor)
        let meta = NSButton(checkboxWithTitle: L("⌥ 键作为 Meta"),
                            target: self, action: #selector(changed))
        meta.state = (v.setting("term_option_meta") ?? "0") == "1"
            ? .on : .off
        meta.frame = NSRect(x: 24, y: y, width: 220, height: 22)
        addSubview(meta)
        y += 40
        frame = NSRect(x: 0, y: 0, width: 480, height: y)
    }
    required init?(coder: NSCoder) { nil }
    @objc private func changed(_ s: NSControl) {
        if let p = s as? NSPopUpButton {
            if p.tag == 1 {
                let t = TermTheme.all[max(0, p.indexOfSelectedItem)]
                v.setSetting("term_theme", t.id)
            } else if p.tag == 2 {
                v.setSetting("term_cursor",
                             String(p.indexOfSelectedItem))
            } else if let t = p.titleOfSelectedItem {
                v.setSetting("term_font", t)
            }
        } else if let b = s as? NSButton {
            v.setSetting("term_option_meta", b.state == .on ? "1" : "0")
        } else if let t = s as? NSTextField {
            if Double(t.stringValue) != nil {
                if t.stringValue.contains(".") {
                    v.setSetting("term_line_height", t.stringValue)
                } else if let n = Int(t.stringValue), n > 1000 {
                    v.setSetting("term_scrollback", t.stringValue)
                } else {
                    v.setSetting("term_font_size", t.stringValue)
                }
            }
        }
        onChange?()
    }
}
