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
        layer?.hbBg = "content"
        var y: CGFloat = 14
        func row(_ label: String, _ ctrl: NSView) {
            let l = NSTextField(labelWithString: label)
            l.font = .systemFont(ofSize: 12)
            l.frame = NSRect(x: 24, y: y + 2, width: 140, height: 20)
            ctrl.frame = NSRect(x: 170, y: y, width: 200, height: 24)
            addSubview(l); addSubview(ctrl)
            y += 34
        }
        // ---- 界面（主题/语言；主题同步、语言设备本地）----
        let uiTheme = NSPopUpButton()
        uiTheme.tag = 3
        uiTheme.addItems(withTitles: [L("浅色"), L("深色"), L("跟随系统")])
        uiTheme.selectItem(at: ["light","dark","system"]
            .firstIndex(of: v.setting("ui_theme") ?? "light") ?? 0)
        uiTheme.target = self; uiTheme.action = #selector(changed)
        row(L("界面主题"), uiTheme)

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
        y += 30
        y += 6

        // ---- 系统集成（设备本地，不同步）----
        let sect = NSTextField(labelWithString: L("系统集成"))
        sect.font = .systemFont(ofSize: 12, weight: .semibold)
        sect.hbText = "termDim"
        sect.frame = NSRect(x: 24, y: y, width: 200, height: 20)
        addSubview(sect)
        y += 28
        let tray = NSButton(checkboxWithTitle:
            L("关闭窗口时留在菜单栏（会话保持运行）"),
            target: self, action: #selector(trayChanged))
        tray.state = v.settingsBool("close_to_tray", true) ? .on : .off
        tray.frame = NSRect(x: 24, y: y, width: 380, height: 22)
        addSubview(tray)
        y += 30
        login = NSButton(checkboxWithTitle: L("登录时启动 Harbor"),
                         target: self, action: #selector(loginChanged))
        login.state = v.settingsBool("start_at_login", false)
            ? .on : .off
        login.frame = NSRect(x: 24, y: y, width: 380, height: 22)
        addSubview(login)
        y += 24
        loginNote = NSTextField(wrappingLabelWithString: "")
        loginNote.font = .systemFont(ofSize: 11)
        loginNote.hbText = "amber"
        loginNote.frame = NSRect(x: 44, y: y, width: 400, height: 30)
        addSubview(loginNote)
        y += 36
        let keep = NSButton(checkboxWithTitle:
            L("保持连接（防止休眠断言）"),
            target: self, action: #selector(keepChanged))
        keep.state = v.settingsBool("keep_connected", false) ? .on : .off
        keep.frame = NSRect(x: 24, y: y, width: 380, height: 22)
        addSubview(keep)
        y += 34

        // ---- 快捷键（shortcuts_macos 覆盖，随 E2EE 同步到其它 Mac）----
        y += 8
        let sc = NSTextField(labelWithString: L("快捷键"))
        sc.font = .systemFont(ofSize: 12, weight: .semibold)
        sc.hbText = "termDim"
        sc.frame = NSRect(x: 24, y: y, width: 200, height: 20)
        addSubview(sc)
        let rstAll = NSButton(title: L("全部恢复默认"), target: self,
                              action: #selector(resetAllShortcuts))
        rstAll.bezelStyle = .rounded
        rstAll.controlSize = .small
        rstAll.frame = NSRect(x: 300, y: y - 2, width: 120, height: 22)
        addSubview(rstAll)
        y += 26
        scHint = NSTextField(wrappingLabelWithString: "")
        scHint.font = .systemFont(ofSize: 11)
        scHint.hbText = "amber"
        scHint.frame = NSRect(x: 24, y: y, width: 430, height: 16)
        addSubview(scHint)
        y += 20
        ShortcutKit.shared.onToast = { [weak scHint] t in
            scHint?.stringValue = t
        }
        for i in 0..<ShortcutTable.rows.count
            where ShortcutTable.editable(i) {
            let r = ShortcutKit.shared.rows[i]
            let l = NSTextField(labelWithString: r.label)
            l.font = .systemFont(ofSize: 12)
            l.frame = NSRect(x: 24, y: y + 2, width: 200, height: 20)
            addSubview(l)
            let b = ShortcutCapButton(idx: i)
            b.frame = NSRect(x: 230, y: y, width: 150, height: 24)
            b.bezelStyle = .rounded
            addSubview(b)
            let rst = NSButton(title: L("恢复默认"), target: self,
                               action: #selector(resetOne(_:)))
            rst.tag = i
            rst.bezelStyle = .rounded
            rst.controlSize = .small
            rst.frame = NSRect(x: 388, y: y, width: 80, height: 24)
            addSubview(rst)
            y += 30
        }
        y += 14

        y += 40
        frame = NSRect(x: 0, y: 0, width: 480, height: y)
    }
    required init?(coder: NSCoder) { nil }
    private var login: NSButton!
    private var loginNote: NSTextField!
    private var scHint: NSTextField!
    @objc private func resetOne(_ b: NSButton) {
        ShortcutKit.shared.apply(b.tag, accel: nil)
        refreshShortcutButtons()
    }
    @objc private func resetAllShortcuts() {
        ShortcutKit.shared.resetAll()
        refreshShortcutButtons()
        scHint.stringValue = ""
    }
    private func refreshShortcutButtons() { refreshShortcuts() }
    func refreshShortcuts() {
        for s in subviews.compactMap({ $0 as? ShortcutCapButton }) {
            s.refreshTitle()
        }
    }
    @objc private func trayChanged(_ b: NSButton) {
        v.setSettingsBool("close_to_tray", b.state == .on)
    }
    @objc private func loginChanged(_ b: NSButton) {
        v.setSettingsBool("start_at_login", b.state == .on)
        if let err = (NSApp.delegate as? App)?.applyLoginItem() {
            // 注册被系统拒绝（如签名不被接受）→ 回退开关 + 说明
            v.setSettingsBool("start_at_login", false)
            b.state = .off
            loginNote.stringValue = String(
                format: L("无法注册登录项：%@"), err)
        } else if b.state == .on,
                  (NSApp.delegate as? App)?.devLoginStatus == "2" {
            loginNote.stringValue = L(
                "需在「系统设置 › 通用 › 登录项」里批准一次 Harbor。")
        } else {
            loginNote.stringValue = ""
        }
    }
    @objc private func keepChanged(_ b: NSButton) {
        v.setSettingsBool("keep_connected", b.state == .on)
        SleepPolicy.shared.refreshAssertion()
    }
    @objc private func changed(_ s: NSControl) {
        if let p = s as? NSPopUpButton {
            switch p.tag {
            case 1:
                let t = TermTheme.all[max(0, p.indexOfSelectedItem)]
                v.setSetting("term_theme", t.id)
            case 2:
                v.setSetting("term_cursor",
                             String(p.indexOfSelectedItem))
            case 3:
                let th = ["light","dark","system"][max(0,
                    p.indexOfSelectedItem)]
                v.setSetting("ui_theme", th)
                (NSApp.delegate as? App)?.applyTheme()
            default:
                if let t = p.titleOfSelectedItem {
                    v.setSetting("term_font", t)
                }
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

// ---------- 快捷键捕获按钮 ----------
/// 点击进入捕获态：按新组合提交；Esc 取消；Backspace/Delete 禁用
/// （同 linux hb-shortcuts.c 语义）。修饰键顺序 ⌃⌥⇧⌘。
final class ShortcutCapButton: NSButton {
    let idx: Int
    private var monitor: Any?
    init(idx: Int) {
        self.idx = idx
        super.init(frame: .zero)
        target = self; action = #selector(beginCapture)
        refreshTitle()
    }
    required init?(coder: NSCoder) { fatalError() }
    func refreshTitle() {
        let r = ShortcutKit.shared.rows[idx]
        title = ShortcutTable.label(key: r.key, mods: r.mods)
    }
    static let keyNames: [UInt16: String] = [
        48: "tab", 49: "space", 36: "return", 51: "delete",
        117: "forwarddelete", 123: "left", 124: "right",
        125: "down", 126: "up", 115: "home", 119: "end",
        116: "pageup", 121: "pagedown",
        122: "f1", 120: "f2", 99: "f3", 118: "f4", 96: "f5",
        97: "f6", 98: "f7", 100: "f8", 101: "f9", 109: "f10",
        103: "f11", 111: "f12", 105: "f13", 107: "f14",
        113: "f15", 106: "f16", 64: "f17", 79: "f18", 80: "f19",
        90: "f20",
    ]
    @objc private func beginCapture() {
        title = L("按下新的快捷键…")
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) {
            [weak self] e in
            guard let self else { return e }
            let mods = e.modifierFlags.intersection(
                [.command, .shift, .option, .control])
            var m: UInt = 0
            if mods.contains(.command) { m |= SC_M_CMD }
            if mods.contains(.shift)   { m |= SC_M_SHIFT }
            if mods.contains(.option)  { m |= SC_M_OPT }
            if mods.contains(.control) { m |= SC_M_CTRL }
            if e.keyCode == 53 {           // Esc → 取消
                self.endCapture(); return nil
            }
            if e.keyCode == 51 || e.keyCode == 117 {   // ⌫/⌦ → 禁用
                ShortcutKit.shared.commit(self.idx, accel: "")
                self.endCapture(); return nil
            }
            let key = Self.keyNames[e.keyCode]
                ?? e.charactersIgnoringModifiers?.lowercased()
                ?? ""
            guard ShortcutTable.keyName(key) != nil else { return nil }
            var parts = ""
            if m & SC_M_CTRL  != 0 { parts += "ctrl+" }
            if m & SC_M_OPT   != 0 { parts += "opt+" }
            if m & SC_M_SHIFT != 0 { parts += "shift+" }
            if m & SC_M_CMD   != 0 { parts += "cmd+" }
            ShortcutKit.shared.commit(self.idx, accel: parts + key)
            self.endCapture()
            return nil
        }
    }
    private func endCapture() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        // 冲突解绑可能影响其它行 → 全量刷新标题
        (superview as? AppearancePanel)?.refreshShortcuts()
        if superview == nil { refreshTitle() }
    }
}
