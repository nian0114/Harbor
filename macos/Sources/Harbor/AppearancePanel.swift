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
        let lang = NSPopUpButton()
        lang.tag = 4
        lang.addItems(withTitles: [L("跟随系统"), "简体中文", "English"])
        lang.selectItem(at: ["","zh","en"]
            .firstIndex(of: v.setting("language") ?? "") ?? 0)
        lang.target = self; lang.action = #selector(changed)
        row(L("语言"), lang)

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

        // ---- 数据备份（.harbor-backup 加密导出/导入）----
        let bk = NSTextField(labelWithString: L("数据备份"))
        bk.font = .systemFont(ofSize: 12, weight: .semibold)
        bk.hbText = "termDim"
        bk.frame = NSRect(x: 24, y: y, width: 200, height: 20)
        addSubview(bk)
        y += 26
        let ex = NSButton(title: L("导出加密备份…"), target: self,
                          action: #selector(exportBackup))
        ex.bezelStyle = .rounded
        ex.frame = NSRect(x: 24, y: y, width: 150, height: 24)
        ex.setAccessibilityIdentifier("bk-export")
        addSubview(ex)
        let im = NSButton(title: L("导入加密备份…"), target: self,
                          action: #selector(importBackup))
        im.bezelStyle = .rounded
        im.frame = NSRect(x: 184, y: y, width: 150, height: 24)
        im.setAccessibilityIdentifier("bk-import")
        addSubview(im)
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
            case 4:
                let lg = ["","zh","en"][max(0, p.indexOfSelectedItem)]
                if lg != (v.setting("language") ?? "") {
                    v.setSetting("language", lg)
                    // 镜像到 UserDefaults：下次启动在主窗口创建前
                    // 装表（启动期不能碰 Vault）。
                    UserDefaults.standard.set(lg,
                        forKey: "harbor.language")
                    offerRestart()
                }
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

    /// 语言变更：重启后生效（翻译表在启动时装配，同 linux 的
    /// LANGUAGE 环境装配时机），同 linux 提供一键重启。
    private func offerRestart() {
        let a = NSAlert()
        a.messageText = L("重启 Harbor 后生效")
        a.addButton(withTitle: L("重启"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: "restart",
                                      in: window) { ok in
            guard ok else { return }
            let cfg = NSWorkspace.OpenConfiguration()
            NSWorkspace.shared.openApplication(
                at: Bundle.main.bundleURL,
                configuration: cfg) { _, _ in NSApp.terminate(nil) }
        }
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

// ---------- 加密备份（FR-15，docs/api/backup-format.md）----------
/// 导出：文件面板 → 口令+确认 → worker 里 hb_backup_export + 写盘。
/// 口令走 [CChar] 缓冲，用完 memzero；字段同时清空。
/// 导入：读文件 → 口令 → worker preview（不写库）→ 展示对象数确认 →
/// 主线程 import（最后一步写库）。口令错/损坏 = HB_ERR_INVAL，库不变。
extension AppearancePanel {
    @objc func exportBackup() {
        let sp = NSSavePanel()
        sp.nameFieldStringValue = "harbor.harbor-backup"
        guard sp.runModal() == .OK, let url = sp.url else { return }
        askBackupPassword(confirm: true) { [weak self] pw in
            guard let self, var pw else { return }
            DispatchQueue.global().async {
                var out: UnsafeMutablePointer<UInt8>?
                var len = 0
                var msg = L("导出失败")
                defer { for i in pw.indices { pw[i] = 0 } }
                let path = url.path
                var err = HB_ERR_INVALID
                if let v = self.v.vault {
                    err = pw.withUnsafeMutableBufferPointer { bp in
                        hb_backup_export(v, bp.baseAddress, &out, &len)
                    }
                }
                if err == HB_OK, let o = out {
                    let d = Data(bytes: o, count: len)
                    for i in 0..<len { o[i] = 0 }
                    free(o)
                    do {
                        try d.write(to: URL(fileURLWithPath: path))
                        msg = L("加密备份已导出")
                    } catch { msg = L("导出失败") }
                }
                DispatchQueue.main.async { self.bkAlert(msg) }
            }
        }
    }

    @objc func importBackup() {
        let op = NSOpenPanel()
        guard op.runModal() == .OK, let u = op.url,
              let data = try? Data(contentsOf: u) else {
            bkAlert(L("无法读取备份文件")); return
        }
        askBackupPassword(confirm: false) { [weak self] pw in
            guard let self, var pw else { return }
            DispatchQueue.global().async {
                var st = hb_backup_stat()
                var perr = HB_ERR_INVALID
                data.withUnsafeBytes { raw in
                    pw.withUnsafeMutableBufferPointer { bp in
                        perr = hb_backup_preview(
                            raw.baseAddress!.assumingMemoryBound(
                                to: UInt8.self),
                            raw.count, bp.baseAddress, &st)
                    }
                }
                DispatchQueue.main.async {
                    guard perr == HB_OK else {
                        for i in pw.indices { pw[i] = 0 }
                        self.bkAlert(L("口令错误或文件已损坏"))
                        return
                    }
                    // 移交所有权：置空本帧副本，box 独持存储才能真擦除
                    let box = PWBox(pw); pw = []
                    self.confirmImport(data: data,
                                       pw: box, total: st.total)
                }
            }
        }
    }

    /// [CChar] 所有权盒：让数组存储唯一持有，清零才真写回原内存。
    final class PWBox { var buf: [CChar]; init(_ b: [CChar]) { buf = b } }
    private func confirmImport(data: Data,
                               pw box: PWBox, total: Int) {
        let a = NSAlert()
        a.messageText = L("导入加密备份")
        a.informativeText = String(format:
            L("备份文件校验通过，包含 %d 个对象。"
              + "同 id 对象与密钥将被覆盖，此操作不可撤销。"), total)
        a.addButton(withTitle: L("导入"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: "bk-import",
                                      in: window) { [weak self] ok in
            guard let self else { return }
            defer { for i in box.buf.indices { box.buf[i] = 0 } }
            guard ok else { return }
            var st = hb_backup_stat()
            var err = HB_ERR_INVALID
            data.withUnsafeBytes { raw in
                box.buf.withUnsafeMutableBufferPointer { bp in
                    if let v = self.v.vault {
                        err = hb_backup_import(v,
                            raw.baseAddress!.assumingMemoryBound(
                                to: UInt8.self),
                            raw.count, bp.baseAddress, &st)
                    }
                }
            }
            self.bkAlert(err == HB_OK
                ? String(format: L("已导入：新增 %d，覆盖 %d"),
                         st.created, st.overwritten)
                : L("口令错误或文件已损坏"))
        }
    }

    /// 口令对话框：导出=新口令+确认，导入=解密口令。
    /// 完成回调给 [CChar]（含 NUL 结尾），用毕由调用方清零。
    private func askBackupPassword(confirm: Bool,
                                   cb: @escaping ([CChar]?) -> Void) {
        let a = NSAlert()
        a.messageText = confirm ? L("导出加密备份") : L("导入加密备份")
        a.informativeText = confirm ? L("设置备份密码")
                                      : L("输入该备份的导出密码：")
        a.addButton(withTitle: L("确定")); a.addButton(withTitle: L("取消"))
        let p1 = NSSecureTextField(frame: NSRect(x: 0, y: 28,
                                               width: 240, height: 22))
        p1.placeholderString = L("备份密码")
        p1.setAccessibilityIdentifier("bk-pw")
        let vv = NSView(frame: NSRect(x: 0, y: 0, width: 240,
                                      height: confirm ? 56 : 30))
        var p2: NSSecureTextField?
        if confirm {
            let f = NSSecureTextField(frame: NSRect(x: 0, y: 0,
                                                    width: 240, height: 22))
            f.placeholderString = L("确认备份密码")
            f.setAccessibilityIdentifier("bk-pw2")
            p2 = f; vv.addSubview(f)
        }
        vv.addSubview(p1)
        a.accessoryView = vv
        DialogRegistry.shared.present(alert: a, kind: "bk-pw",
                                      in: window) { ok in
            defer { p1.stringValue = ""; p2?.stringValue = "" }
            guard ok else { cb(nil); return }
            let s1 = p1.stringValue
            guard !s1.isEmpty,
                  p2 == nil || p2!.stringValue == s1 else {
                if p2 != nil {
                    let b = NSAlert()
                    b.messageText = L("两次输入的密码不一致")
                    DialogRegistry.shared.present(alert: b, kind: "bk-err",
                                                  in: self.window) { _ in }
                }
                cb(nil); return
            }
            cb(Array(s1.utf8CString))
        }
    }

    private func bkAlert(_ t: String) {
        let a = NSAlert()
        a.messageText = t
        DialogRegistry.shared.present(alert: a, kind: "bk-toast",
                                      in: window) { _ in }
    }
}
