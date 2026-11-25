import AppKit
import CHarbor
import HarborKit

// ---------- 外观与终端 设置页 ----------
final class AppearancePanel: NSView {
    override var isFlipped: Bool { true }
    private let v = Vault.shared
    private let terminal = TerminalView()
    private let previewWrap = NSView()
    private let previewBar = NSView()
    private let previewStatus = NSView()
    private let previewTitleL = NSTextField(labelWithString: "ssh nian@10.88.2.11")
    private let previewWatchBtn = NSView()
    private let previewWatchL = NSTextField(labelWithString: L("监控 开"))
    private let previewStatusL = NSTextField(labelWithString: "● " + L("已连接"))
    private let previewTermL = NSTextField(labelWithString: "xterm-256color")
    private let previewScrollL = NSTextField(labelWithString: L("scrollback 10,000 行"))
    private let settings = AppearanceSettingsView()
    private let fontSize = NSSlider(value: 14, minValue: 10, maxValue: 20, target: nil, action: nil)
    private let lineHeight = NSSlider(value: 1.15, minValue: 1, maxValue: 2.5, target: nil, action: nil)
    private let sizeLabel = vLabel("")
    private let heightLabel = vLabel("")
    private let autoSw = VSwitch(frame: .zero)
    private var themes: [NSButton] = []
    var onChange: (() -> Void)?
    init() {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.hbBg = "content"
        buildLayout()
    }

    private func buildLayout() {
        let scroll = NSScrollView(frame: bounds)
        scroll.autoresizingMask = [.width, .height]
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        addSubview(scroll)
        let content = VFlippedStack()
        content.orientation = .vertical
        content.alignment = .centerX
        content.spacing = 14
        content.edgeInsets = NSEdgeInsets(top: 14, left: 18, bottom: 20, right: 18)
        content.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = content
        content.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor).isActive = true

        let choices = VPanel()
        choices.append(vPanelTitle(L("终端主题"), icon: "palette"))
        let csub = NSTextField(wrappingLabelWithString: L("实时应用到所有会话与分屏，经 E2EE 同步到其它设备。"))
        csub.font = .systemFont(ofSize: 12)
        csub.hbText = "text3"
        choices.append(csub)
        themes = TermTheme.all.enumerated().map { index, theme in
            let button = AppearanceThemeButton(theme: theme)
            button.tag = index
            button.target = self
            button.action = #selector(selectTheme)
            return button
        }
        for pair in stride(from: 0, to: themes.count, by: 2) {
            let row = NSStackView(views: Array(themes[pair..<min(pair + 2, themes.count)]))
            row.distribution = .fillEqually
            row.spacing = 10
            choices.append(row)
        }
        fontSize.numberOfTickMarks = 11
        fontSize.allowsTickMarkValuesOnly = true
        sizeLabel.font = .monospacedSystemFont(ofSize: 12.5, weight: .bold)
        sizeLabel.hbText = "blue"
        heightLabel.font = .monospacedSystemFont(ofSize: 12.5, weight: .bold)
        heightLabel.hbText = "blue"
        for (title, value, slider) in [(L("字号"), sizeLabel, fontSize),
                                       (L("行高"), heightLabel, lineHeight)] {
            let tl = vLabel(title, head: true)
            tl.hbText = "text2"
            let row = NSStackView(views: [tl, NSView(), value])
            row.spacing = 8
            choices.append(row)
            slider.isContinuous = true
            slider.target = self
            slider.action = #selector(changeMetric)
            slider.setAccessibilityLabel(title)
            choices.append(slider)
        }
        let autoRow = NSStackView()
        autoRow.orientation = .horizontal; autoRow.spacing = 8; autoRow.alignment = .centerY
        let autoTxt = NSStackView()
        autoTxt.orientation = .vertical; autoTxt.spacing = 2; autoTxt.alignment = .leading
        let autoTitle = vLabel("Autocomplete  BETA")
        autoTitle.font = .systemFont(ofSize: 13)
        let autoSub = NSTextField(wrappingLabelWithString: L("基于 Shell History 的本地补全，历史不出设备"))
        autoSub.font = .systemFont(ofSize: 11.5); autoSub.hbText = "text3"
        autoTxt.addArrangedSubview(autoTitle); autoTxt.addArrangedSubview(autoSub)
        autoSw.isOn = v.settingsBool("autocomplete", false)
        autoSw.target = self; autoSw.action = #selector(toggleAuto)
        autoSw.translatesAutoresizingMaskIntoConstraints = false
        autoSw.widthAnchor.constraint(equalToConstant: 38).isActive = true
        autoSw.heightAnchor.constraint(equalToConstant: 22).isActive = true
        autoRow.addArrangedSubview(autoTxt); autoRow.addArrangedSubview(NSView()); autoRow.addArrangedSubview(autoSw)
        choices.append(autoRow)

        let reset = HBtn(L("恢复默认"), sm: true, icon: "refresh")
        reset.onTap = { [weak self] in
            guard let self else { return }
            self.v.setSetting("theme", "harbor")
            self.v.setSetting("font_size", "14")
            self.v.setSetting("line_height", "1.15")
            self.refreshPreview()
            self.onChange?()
        }
        choices.append(NSStackView(views: [reset, NSView()]))

        let preview = VPanel()
        preview.append(vPanelTitle(L("实时预览"), icon: "terminal"))
        let psub = NSTextField(wrappingLabelWithString: L("拖动左侧滑杆 / 切换主题，立即生效。"))
        psub.font = .systemFont(ofSize: 12); psub.hbText = "text3"
        preview.append(psub)

        previewWrap.wantsLayer = true
        previewWrap.layer?.cornerRadius = 11
        previewWrap.layer?.borderWidth = 1
        previewWrap.layer?.hbBorder = "line"
        previewWrap.layer?.masksToBounds = true
        previewBar.wantsLayer = true
        let uico = NSImageView(image: hbIcon("ubuntu", size: 13, tint: Theme.ubuntu.color))
        previewTitleL.font = .systemFont(ofSize: 12)
        previewWatchBtn.wantsLayer = true
        previewWatchBtn.layer?.cornerRadius = 6
        previewWatchBtn.layer?.borderWidth = 1
        let wico = NSImageView(image: hbIcon("eye", size: 12, tint: Theme.green.color))
        previewWatchL.font = .systemFont(ofSize: 11.5, weight: .medium)
        let wstack = NSStackView(views: [wico, previewWatchL])
        wstack.spacing = 4; wstack.edgeInsets = NSEdgeInsets(top: 3, left: 8, bottom: 3, right: 8)
        wstack.translatesAutoresizingMaskIntoConstraints = false
        previewWatchBtn.addSubview(wstack)
        NSLayoutConstraint.activate([
            wstack.leadingAnchor.constraint(equalTo: previewWatchBtn.leadingAnchor),
            wstack.trailingAnchor.constraint(equalTo: previewWatchBtn.trailingAnchor),
            wstack.topAnchor.constraint(equalTo: previewWatchBtn.topAnchor),
            wstack.bottomAnchor.constraint(equalTo: previewWatchBtn.bottomAnchor),
        ])
        let barRow = NSStackView(views: [uico, previewTitleL, NSView(), previewWatchBtn])
        barRow.spacing = 6; barRow.alignment = .centerY
        barRow.translatesAutoresizingMaskIntoConstraints = false
        previewBar.addSubview(barRow)
        NSLayoutConstraint.activate([
            barRow.leadingAnchor.constraint(equalTo: previewBar.leadingAnchor, constant: 12),
            barRow.trailingAnchor.constraint(equalTo: previewBar.trailingAnchor, constant: -12),
            barRow.topAnchor.constraint(equalTo: previewBar.topAnchor, constant: 6),
            barRow.bottomAnchor.constraint(equalTo: previewBar.bottomAnchor, constant: -6),
        ])

        previewStatus.wantsLayer = true
        previewStatusL.font = .systemFont(ofSize: 11)
        previewTermL.font = .systemFont(ofSize: 11)
        previewScrollL.font = .systemFont(ofSize: 11)
        let stRow = NSStackView(views: [previewStatusL, previewTermL, NSView(), previewScrollL])
        stRow.spacing = 12; stRow.alignment = .centerY
        stRow.translatesAutoresizingMaskIntoConstraints = false
        previewStatus.addSubview(stRow)
        NSLayoutConstraint.activate([
            stRow.leadingAnchor.constraint(equalTo: previewStatus.leadingAnchor, constant: 12),
            stRow.trailingAnchor.constraint(equalTo: previewStatus.trailingAnchor, constant: -12),
            stRow.topAnchor.constraint(equalTo: previewStatus.topAnchor, constant: 5),
            stRow.bottomAnchor.constraint(equalTo: previewStatus.bottomAnchor, constant: -5),
        ])

        terminal.heightAnchor.constraint(equalToConstant: 220).isActive = true
        terminal.setAccessibilityLabel(L("实时预览"))
        if let vt = hb_vt_new(8, 72, 0) {
            let sample = "\u{1b}[2mWelcome to Ubuntu 24.04 LTS\u{1b}[0m\r\n"
                + "\u{1b}[32mdemo@host\u{1b}[0m:~$ make test\r\n"
                + "\u{1b}[2mRunning tests…\u{1b}[0m\r\n"
                + "\u{1b}[32m✔ All tests passed\u{1b}[0m\r\n"
                + "\u{1b}[32mdemo@host\u{1b}[0m:~$ "
            sample.withCString { hb_vt_feed(vt, $0, sample.utf8.count) }
            terminal.extVt = vt
        }
        let pstack = VFlippedStack(views: [previewBar, terminal, previewStatus])
        pstack.orientation = .vertical; pstack.spacing = 0
        pstack.translatesAutoresizingMaskIntoConstraints = false
        previewWrap.addSubview(pstack)
        NSLayoutConstraint.activate([
            pstack.leadingAnchor.constraint(equalTo: previewWrap.leadingAnchor),
            pstack.trailingAnchor.constraint(equalTo: previewWrap.trailingAnchor),
            pstack.topAnchor.constraint(equalTo: previewWrap.topAnchor),
            pstack.bottomAnchor.constraint(equalTo: previewWrap.bottomAnchor),
            previewBar.widthAnchor.constraint(equalTo: pstack.widthAnchor),
            terminal.widthAnchor.constraint(equalTo: pstack.widthAnchor),
            previewStatus.widthAnchor.constraint(equalTo: pstack.widthAnchor),
        ])
        preview.append(previewWrap)
        preview.append(vHintBox(L("外观设置属于同步范围「外观与设置」（FR-11）；字号 / 行距 / 主题在 macOS · Linux · Android 三端一致。")))
        let panels = VPanelColumns(left: choices, right: preview)
        let more = VPanel()
        more.append(vPanelTitle(L("外观与终端"), icon: "palette"))
        buildSettings()
        settings.heightAnchor.constraint(equalToConstant: settings.frame.height).isActive = true
        more.append(settings)
        for section in [panels, more] {
            section.translatesAutoresizingMaskIntoConstraints = false
            content.addArrangedSubview(section)
            section.widthAnchor.constraint(equalTo: content.widthAnchor, constant: -36).isActive = true
        }
        refreshPreview()
    }

    deinit {
        if let vt = terminal.extVt {
            terminal.extVt = nil
            hb_vt_free(vt)
        }
    }

    private func refreshPreview() {
        let theme = TermTheme.for(v.setting("theme"))
        terminal.fontName = v.setting("font_family") ?? "Menlo"
        terminal.fontSize = CGFloat(Double(v.setting("font_size") ?? "") ?? 14)
        terminal.lineHeight = CGFloat(Double(v.setting("line_height") ?? "") ?? 1.15)
        terminal.cursorStyle = Int(v.setting("cursor_style") ?? "0") ?? 0
        terminal.theme = theme
        previewBar.layer?.backgroundColor = theme.bar.color.cgColor
        previewStatus.layer?.backgroundColor = theme.bar.color.cgColor
        previewTitleL.textColor = theme.title.color
        previewTermL.textColor = theme.muted.color
        previewScrollL.textColor = theme.muted.color
        previewStatusL.textColor = theme.cursor.color
        let isLight = theme.id == "light"
        previewWatchBtn.layer?.backgroundColor =
            (isLight ? Theme.greenPillBg.color : Theme.tabTermBg.color).cgColor
        previewWatchBtn.layer?.borderColor =
            (isLight ? Theme.green.color.withAlphaComponent(0.35) : Theme.tabTermLine.color).cgColor
        previewWatchL.textColor = isLight ? Theme.green.color : Theme.tabTermFg.color
        autoSw.isOn = v.settingsBool("autocomplete", false)
        fontSize.doubleValue = Double(terminal.fontSize)
        lineHeight.doubleValue = Double(terminal.lineHeight)
        sizeLabel.stringValue = String(format: "%g px", Double(terminal.fontSize))
        heightLabel.stringValue = String(format: "%.2f", Double(terminal.lineHeight))
        for (button, choice) in zip(themes, TermTheme.all) {
            button.state = theme.id == choice.id ? .on : .off
            button.needsDisplay = true
        }
    }

    @objc private func toggleAuto() {
        v.setSettingsBool("autocomplete", autoSw.isOn)
        onChange?()
    }

    @objc private func selectTheme(_ button: NSButton) {
        v.setSetting("theme", TermTheme.all[button.tag].id)
        refreshPreview()
        onChange?()
    }

    @objc private func changeMetric(_ slider: NSSlider) {
        if slider === fontSize {
            v.setSetting("font_size", String(slider.integerValue))
        } else {
            v.setSetting("line_height", String(format: "%.2f", slider.doubleValue))
        }
        refreshPreview()
        onChange?()
    }

    private func buildSettings() {
        var y: CGFloat = 14
        func row(_ label: String, _ ctrl: NSView) {
            let l = NSTextField(labelWithString: label)
            l.font = .systemFont(ofSize: 12)
            l.frame = NSRect(x: 24, y: y + 2, width: 140, height: 20)
            ctrl.frame = NSRect(x: 170, y: y, width: 200, height: 24)
            settings.addSubview(l); settings.addSubview(ctrl)
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
        fonts.selectItem(withTitle: v.setting("font_family") ?? "Menlo")
        fonts.target = self; fonts.action = #selector(changed)
        row(L("终端字体"), fonts)
        let sb = NSTextField()
        sb.stringValue = v.setting("scrollback") ?? "10000"
        sb.tag = 6
        sb.target = self; sb.action = #selector(changed)
        row(L("回滚行数"), sb)
        let cursor = NSPopUpButton()
        cursor.tag = 2
        cursor.addItems(withTitles: [L("块"), L("下划线"), L("竖线")])
        cursor.selectItem(at: Int(v.setting("cursor_style") ?? "0") ?? 0)
        cursor.target = self; cursor.action = #selector(changed)
        row(L("光标样式"), cursor)
        let meta = NSButton(checkboxWithTitle: L("⌥ 键作为 Meta"),
                            target: self, action: #selector(changed))
        meta.state = (v.setting("option_meta") ?? "0") == "1"
            ? .on : .off
        meta.frame = NSRect(x: 24, y: y, width: 220, height: 22)
        settings.addSubview(meta)
        y += 30
        let conc = NSTextField()
        conc.stringValue = v.setting("sftp_concurrency") ?? "3"
        conc.target = self; conc.action = #selector(changed)
        conc.tag = 5
        row(L("SFTP 并发传输数"), conc)
        y += 6

        // ---- 系统集成（设备本地，不同步）----
        let sect = NSTextField(labelWithString: L("系统集成"))
        sect.font = .systemFont(ofSize: 12, weight: .semibold)
        sect.hbText = "termDim"
        sect.frame = NSRect(x: 24, y: y, width: 200, height: 20)
        settings.addSubview(sect)
        y += 28
        let tray = NSButton(checkboxWithTitle:
            L("关闭窗口时留在菜单栏（会话保持运行）"),
            target: self, action: #selector(trayChanged))
        tray.state = v.settingsBool("close_to_tray", true) ? .on : .off
        tray.frame = NSRect(x: 24, y: y, width: 380, height: 22)
        settings.addSubview(tray)
        y += 30
        login = NSButton(checkboxWithTitle: L("登录时启动 Harbor"),
                         target: self, action: #selector(loginChanged))
        login.state = v.settingsBool("start_at_login", false)
            ? .on : .off
        login.frame = NSRect(x: 24, y: y, width: 380, height: 22)
        settings.addSubview(login)
        y += 24
        loginNote = NSTextField(wrappingLabelWithString: "")
        loginNote.font = .systemFont(ofSize: 11)
        loginNote.hbText = "amber"
        loginNote.frame = NSRect(x: 44, y: y, width: 400, height: 30)
        settings.addSubview(loginNote)
        y += 36
        let keep = NSButton(checkboxWithTitle:
            L("保持连接（防止休眠断言）"),
            target: self, action: #selector(keepChanged))
        keep.state = v.settingsBool("keep_connected", false) ? .on : .off
        keep.frame = NSRect(x: 24, y: y, width: 380, height: 22)
        settings.addSubview(keep)
        y += 34

        // ---- 快捷键（shortcuts_macos 覆盖，随 E2EE 同步到其它 Mac）----
        y += 8
        let sc = NSTextField(labelWithString: L("快捷键"))
        sc.font = .systemFont(ofSize: 12, weight: .semibold)
        sc.hbText = "termDim"
        sc.frame = NSRect(x: 24, y: y, width: 200, height: 20)
        settings.addSubview(sc)
        let rstAll = NSButton(title: L("全部恢复默认"), target: self,
                              action: #selector(resetAllShortcuts))
        rstAll.bezelStyle = .rounded
        rstAll.controlSize = .small
        rstAll.frame = NSRect(x: 300, y: y - 2, width: 120, height: 22)
        settings.addSubview(rstAll)
        y += 26
        scHint = NSTextField(wrappingLabelWithString: "")
        scHint.font = .systemFont(ofSize: 11)
        scHint.hbText = "amber"
        scHint.frame = NSRect(x: 24, y: y, width: 430, height: 16)
        settings.addSubview(scHint)
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
            settings.addSubview(l)
            let b = ShortcutCapButton(idx: i)
            b.frame = NSRect(x: 230, y: y, width: 150, height: 24)
            b.bezelStyle = .rounded
            settings.addSubview(b)
            let rst = NSButton(title: L("恢复默认"), target: self,
                               action: #selector(resetOne(_:)))
            rst.tag = i
            rst.bezelStyle = .rounded
            rst.controlSize = .small
            rst.frame = NSRect(x: 388, y: y, width: 80, height: 24)
            settings.addSubview(rst)
            y += 30
        }
        y += 14

        // ---- 数据备份（.harbor-backup 加密导出/导入）----
        let bk = NSTextField(labelWithString: L("数据备份"))
        bk.font = .systemFont(ofSize: 12, weight: .semibold)
        bk.hbText = "termDim"
        bk.frame = NSRect(x: 24, y: y, width: 200, height: 20)
        settings.addSubview(bk)
        y += 26
        let ex = NSButton(title: L("导出加密备份…"), target: self,
                          action: #selector(exportBackup))
        ex.bezelStyle = .rounded
        ex.frame = NSRect(x: 24, y: y, width: 150, height: 24)
        ex.setAccessibilityIdentifier("bk-export")
        settings.addSubview(ex)
        let im = NSButton(title: L("导入加密备份…"), target: self,
                          action: #selector(importBackup))
        im.bezelStyle = .rounded
        im.frame = NSRect(x: 184, y: y, width: 150, height: 24)
        im.setAccessibilityIdentifier("bk-import")
        settings.addSubview(im)
        y += 40
        settings.frame.size.height = y
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
        for s in settings.subviews.compactMap({ $0 as? ShortcutCapButton }) {
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
            case 2:
                v.setSetting("cursor_style",
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
                    v.setSetting("font_family", t)
                }
            }
        } else if let b = s as? NSButton {
            v.setSetting("option_meta", b.state == .on ? "1" : "0")
        } else if let t = s as? NSTextField {
            if t.tag == 5 {
                if let n = Int(t.stringValue), (1...8).contains(n) {
                    v.setSetting("sftp_concurrency", String(n))
                }
            } else {
                if let n = Int(t.stringValue), n >= 0 {
                    v.setSetting("scrollback", t.stringValue)
                } else {
                    t.stringValue = v.setting("scrollback") ?? "10000"
                }
            }
        }
        refreshPreview()
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

private final class AppearanceSettingsView: NSView {
    override var isFlipped: Bool { true }
}

private final class AppearanceThemeButton: NSButton {
    private let theme: TermTheme
    override var isFlipped: Bool { false }
    override var intrinsicContentSize: NSSize { NSSize(width: 136, height: 65) }

    init(theme: TermTheme) {
        self.theme = theme
        super.init(frame: .zero)
        title = theme.name
        isBordered = false
        setButtonType(.onOff)
        setAccessibilityLabel(theme.name)
        heightAnchor.constraint(equalToConstant: 65).isActive = true
    }

    required init?(coder: NSCoder) { nil }

    override func draw(_ dirtyRect: NSRect) {
        let on = state == .on
        let outline = NSBezierPath(roundedRect: bounds.insetBy(dx: 1, dy: 1), xRadius: 11, yRadius: 11)
        Theme.card.color.setFill()
        outline.fill()
        outline.lineWidth = on ? 1.5 : 1
        (on ? Theme.blue : Theme.line).color.setStroke()
        outline.stroke()
        let sample = NSRect(x: 7, y: 26, width: max(0, bounds.width - 14), height: 33)
        theme.bg.color.setFill()
        NSBezierPath(roundedRect: sample, xRadius: 7, yRadius: 7).fill()
        let mono = NSFont.monospacedSystemFont(ofSize: 10, weight: .regular)
        ("demo@host:~$" as NSString).draw(at: NSPoint(x: 13, y: 43),
                                         withAttributes: [.font: mono, .foregroundColor: theme.cursor.color])
        ("✔ Tests passed" as NSString).draw(at: NSPoint(x: 13, y: 29),
                                           withAttributes: [.font: mono, .foregroundColor: theme.dim.color])
        let fg = on ? Theme.blue.color : Theme.text.color
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 12, weight: .semibold),
            .foregroundColor: fg,
        ]
        (theme.name as NSString).draw(at: NSPoint(x: 10, y: 5), withAttributes: attrs)
        if on {
            let nw = (theme.name as NSString).size(withAttributes: attrs).width
            hbIcon("check", size: 12, tint: fg).draw(
                in: NSRect(x: 14 + nw, y: 7, width: 12, height: 12))
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
