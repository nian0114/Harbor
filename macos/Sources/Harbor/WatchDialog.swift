import AppKit
import CHarbor
import HarborKit

/// 会话监控设置对话框（对齐 hb_watch_ui_open）：
///   启用开关 + 5 信号开关 + 自定义规则 + 聚焦免打扰/仅主机名 +
///   免打扰时段 + [规则测试器] [安装 shell 集成…]。
/// NSAlert + accessoryView 承载 → DialogRegistry 可驱动。
final class WatchDialog: NSObject, NSTextViewDelegate {
    private static var live: [WatchDialog] = []
    private weak var leaf: TermTab?
    private var rulesText: NSTextView!
    private var rulesErr: NSTextField!
    private var sigBtns: [(UInt32, NSButton)] = []

    static func present(leaf: TermTab) {
        let d = WatchDialog()
        d.leaf = leaf
        live.append(d)
        d.show()
    }
    private func release() {
        WatchDialog.live.removeAll { $0 === self }
    }

    private func show() {
        guard let leaf else { release(); return }
        let a = NSAlert()
        a.messageText = L("会话监控 · ") + (leaf.isSSHLeaf ? "SSH"
                                                         : L("Local"))
        a.addButton(withTitle: L("完成"))
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false
        let col = NSStackView()
        col.orientation = .vertical
        col.alignment = .leading
        col.spacing = 7
        col.translatesAutoresizingMaskIntoConstraints = false
        acc.addSubview(col)
        NSLayoutConstraint.activate([
            col.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            col.trailingAnchor.constraint(equalTo: acc.trailingAnchor),
            col.topAnchor.constraint(equalTo: acc.topAnchor),
            col.bottomAnchor.constraint(equalTo: acc.bottomAnchor),
            acc.widthAnchor.constraint(equalToConstant: 470),
        ])

        // -- 启用开关 --
        col.addArrangedSubview(switchRow(
            L("启用完成监控"),
            L("命中完成信号 → Tab 角标 + 通知中心 + 系统通知"),
            on: leaf.watchOn) { [weak self, weak leaf] on in
                leaf?.watchOn = on
                leaf?.applyWatch()
                self?.refreshEye()
            })

        let gl = NSTextField(labelWithString:
            L("完成信号（任一命中即提醒）"))
        gl.font = .systemFont(ofSize: 11, weight: .semibold)
        gl.textColor = Theme.termDim.color
        col.addArrangedSubview(gl)

        // -- 信号开关组（对齐 rows[]） --
        let rows: [(UInt32, String, String)] = [
            (WatchCtl.SIG_EOF, L("会话/进程结束（EOF）"), L("最强信号")),
            (WatchCtl.SIG_OSC133, L("OSC 133 shell 集成"),
             L("含退出码，可区分成败")),
            (WatchCtl.SIG_RULES, L("AI CLI 输出预设"),
             L("aider / 编译 / pytest / 下载等")),
            (WatchCtl.SIG_IDLE, L("空闲启发式（输出≥10s 后静默≥5s）"),
             L("兜底信号，置信度低")),
            (WatchCtl.SIG_TERMNOTIFY, L("终端通知（BEL · OSC 9/777）"),
             L("应用主动请求的通知")),
        ]
        for (bit, t, sub) in rows {
            let (row, btn) = switchRowBtn(
                t, sub, on: (leaf.watchSignals & bit) != 0)
            sigBtns.append((bit, btn))
            btn.action = #selector(sigToggle(_:))
            btn.target = self
            col.addArrangedSubview(row)
        }

        // -- 自定义规则 --
        let rl = NSTextField(labelWithString:
            L("自定义规则（正则，命中后需安静片刻确认）"))
        rl.font = .systemFont(ofSize: 11, weight: .semibold)
        rl.textColor = Theme.termDim.color
        col.addArrangedSubview(rl)
        rulesText = NSTextView()
        rulesText.font = .monospacedSystemFont(ofSize: 11,
                                               weight: .regular)
        rulesText.isRichText = false
        rulesText.delegate = self
        rulesText.setAccessibilityIdentifier("watch-rules")
        rulesText.string = WatchSettings.strv("watch_rules")
            .joined(separator: "\n")
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.borderType = .lineBorder
        scroll.documentView = rulesText
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(equalToConstant: 76)
            .isActive = true
        col.addArrangedSubview(scroll)
        scroll.widthAnchor.constraint(equalTo: col.widthAnchor)
            .isActive = true
        let hint = NSTextField(labelWithString:
            L("每行一条：name: 正则 = 成功 · !name: 正则 = 失败 · " +
              "# 开头为注释"))
        hint.font = .systemFont(ofSize: 10)
        hint.textColor = Theme.termDim.color
        col.addArrangedSubview(hint)
        rulesErr = NSTextField(wrappingLabelWithString: "")
        rulesErr.font = .systemFont(ofSize: 10)
        rulesErr.textColor = Theme.red.color
        rulesErr.isHidden = true
        col.addArrangedSubview(rulesErr)

        // -- 全局开关 --
        col.addArrangedSubview(switchRow(
            L("聚焦本 Tab 时免打扰"),
            L("仅更新角标，不弹系统通知"),
            on: WatchSettings.bool_("watch_focus_dnd", true)) { on in
                WatchSettings.save("watch_focus_dnd", bool: on)
            })
        col.addArrangedSubview(switchRow(
            L("通知正文仅显示主机名"),
            L("隐藏命令与输出内容，防敏感外显"),
            on: WatchSettings.str("notify_body") == "host") { on in
                WatchSettings.save("notify_body",
                                   str: on ? "host" : "full")
            })

        // -- 免打扰时段 --
        let dndRow = NSStackView()
        dndRow.orientation = .horizontal
        dndRow.spacing = 8
        let dl = NSTextField(labelWithString: L("免打扰时段"))
        dl.font = .systemFont(ofSize: 12)
        let df = NSTextField()
        df.placeholderString = "22:00-08:00"
        df.stringValue = WatchSettings.str("watch_dnd") ?? ""
        df.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        df.setAccessibilityIdentifier("watch-dnd")
        let dh = NSTextField(labelWithString:
            L("HH:MM-HH:MM · 空=关"))
        dh.font = .systemFont(ofSize: 10)
        dh.textColor = Theme.termDim.color
        [dl, df, dh].forEach { dndRow.addArrangedSubview($0) }
        df.widthAnchor.constraint(equalToConstant: 96).isActive = true
        col.addArrangedSubview(dndRow)
        NotificationCenter.default.addObserver(
            forName: NSControl.textDidEndEditingNotification,
            object: df, queue: .main) { n in
                let v = (n.object as? NSTextField)?
                    .stringValue.trimmingCharacters(in: .whitespaces) ?? ""
                WatchSettings.save("watch_dnd", str: v)
            }

        // -- footer：测试器 + 安装器 --
        let foot = NSStackView()
        foot.orientation = .horizontal
        foot.spacing = 8
        let tb = NSButton(title: L("规则测试器"), target: self,
                          action: #selector(openTester))
        tb.bezelStyle = .rounded
        tb.setAccessibilityIdentifier("watch-tester")
        let ib = NSButton(title: L("安装 shell 集成…"), target: self,
                          action: #selector(openInstaller))
        ib.bezelStyle = .rounded
        ib.setAccessibilityIdentifier("watch-installer")
        foot.addArrangedSubview(tb)
        foot.addArrangedSubview(ib)
        col.addArrangedSubview(foot)

        a.accessoryView = acc
        DialogRegistry.shared.present(alert: a, kind: "watch",
                                      in: leaf.view.window
                                          as? MainWindow) { [weak self] _ in
            guard let s = self else { return }
            WatchDialog.live.removeAll { $0 === s }
        }
    }
    private func refreshEye() {
        leaf?.onChange?()
    }
    @objc private func sigToggle(_ b: NSButton) {
        guard let leaf,
              let bit = sigBtns.first(where: { $0.1 === b })?.0
        else { return }
        if b.state == .on { leaf.watchSignals |= bit }
        else { leaf.watchSignals &= ~bit }
        leaf.applyWatch()
    }

    // -- 规则文本：合法 → 存 strv + 重挂；非法 → 行号报错不存 --
    func textDidChange(_ n: Notification) {
        guard let tv = n.object as? NSTextView, tv === rulesText
        else { return }
        let text = tv.string
        let (rules, bad) = WatchRules.parseText(text)
        if bad != 0 {
            rulesErr.stringValue = String(format:
                L("第 %d 行不是合法的 \"名称: 正则\"，或正则无法编译 —— " +
                  "未保存"), bad)
            rulesErr.isHidden = false
            return
        }
        _ = rules
        rulesErr.isHidden = true
        WatchSettings.save("watch_rules",
            strv: text.components(separatedBy: "\n")
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty })
        leaf?.applyWatch()
    }

    // ---- 行构造 ----
    private func switchRowBtn(_ t: String, _ sub: String,
                              on: Bool) -> (NSView, NSButton) {
        let row = NSStackView()
        row.orientation = .horizontal
        row.spacing = 8
        let v = NSStackView()
        v.orientation = .vertical
        v.alignment = .leading
        v.spacing = 1
        let tl = NSTextField(labelWithString: t)
        tl.font = .systemFont(ofSize: 12)
        v.addArrangedSubview(tl)
        if !sub.isEmpty {
            let sl = NSTextField(wrappingLabelWithString: sub)
            sl.font = .systemFont(ofSize: 10)
            sl.textColor = Theme.termDim.color
            sl.maximumNumberOfLines = 2
            v.addArrangedSubview(sl)
        }
        let sw = NSButton(checkboxWithTitle: "", target: nil,
                          action: nil)
        sw.state = on ? .on : .off
        sw.setContentHuggingPriority(.required, for: .horizontal)
        row.addArrangedSubview(v)
        row.addArrangedSubview(sw)
        return (row, sw)
    }
    private func switchRow(_ t: String, _ sub: String, on: Bool,
                           apply: @escaping (Bool) -> Void) -> NSView {
        let (row, sw) = switchRowBtn(t, sub, on: on)
        sw.target = self
        sw.action = #selector(swChanged(_:))
        objc_setAssociatedObject(sw, &AssocKey.apply,
                                 apply as AnyObject, .OBJC_ASSOCIATION_RETAIN)
        return row
    }
    private struct AssocKey { static var apply = 0 }
    @objc private func swChanged(_ b: NSButton) {
        if let f = objc_getAssociatedObject(b, &AssocKey.apply)
            as? (Bool) -> Void {
            f(b.state == .on)
        }
    }

    // ---- 规则测试器（对齐 on_tester_run/open_tester） ----
    @objc private func openTester() {
        guard let leaf else { return }
        WatchTester.present(leaf: leaf, signals: leaf.watchSignals)
    }
    // ---- shell 集成安装器 ----
    @objc private func openInstaller() {
        guard let leaf else { return }
        WatchInstaller.present(leaf: leaf)
    }
}

/// 规则测试器（对齐 open_tester + on_tester_run）：
///   粘贴样例 → hb_watch_test_text → 命中规则/未命中。
final class WatchTester: NSObject {
    private static var live: [WatchTester] = []
    private weak var leaf: TermTab?
    private var signals: UInt32 = 0
    private var textV: NSTextView!
    private var result: NSTextField!

    static func present(leaf: TermTab, signals: UInt32) {
        let t = WatchTester()
        t.leaf = leaf
        t.signals = signals
        live.append(t)
        t.show()
    }
    private func show() {
        let a = NSAlert()
        a.messageText = L("规则测试器")
        a.informativeText =
            L("粘贴一段终端输出样例，用当前信号与规则跑一遍引擎。")
        a.addButton(withTitle: L("关闭"))
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false
        textV = NSTextView()
        textV.font = .monospacedSystemFont(ofSize: 11,
                                           weight: .regular)
        textV.isRichText = false
        textV.setAccessibilityIdentifier("watch-test-text")
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.borderType = .lineBorder
        scroll.documentView = textV
        scroll.translatesAutoresizingMaskIntoConstraints = false
        result = NSTextField(wrappingLabelWithString: "")
        result.font = .monospacedSystemFont(ofSize: 11,
                                            weight: .regular)
        result.setAccessibilityIdentifier("watch-test-result")
        let run = NSButton(title: L("测试"), target: self,
                           action: #selector(onRun))
        run.setAccessibilityIdentifier("watch-test-run")
        [scroll, result, run].forEach {
            acc.addSubview($0)
        }
        result.translatesAutoresizingMaskIntoConstraints = false
        run.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            acc.widthAnchor.constraint(equalToConstant: 430),
            scroll.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            scroll.trailingAnchor.constraint(
                equalTo: acc.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: acc.topAnchor),
            scroll.heightAnchor.constraint(equalToConstant: 140),
            run.topAnchor.constraint(equalTo: scroll.bottomAnchor,
                                     constant: 8),
            run.trailingAnchor.constraint(
                equalTo: acc.trailingAnchor),
            result.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            result.trailingAnchor.constraint(equalTo: run.leadingAnchor,
                                             constant: -8),
            result.centerYAnchor.constraint(
                equalTo: run.centerYAnchor),
            result.bottomAnchor.constraint(equalTo: acc.bottomAnchor),
        ])
        a.accessoryView = acc
        DialogRegistry.shared.present(alert: a, kind: "watch-tester",
                                      in: leaf?.view.window
                                          as? MainWindow) { [weak self] _ in
            guard let s = self else { return }
            WatchTester.live.removeAll { $0 === s }
        }
    }
    @objc private func onRun() {
        guard let leaf,
              let cfg = WatchCtl.buildCfg(hostId: leaf.wsHostId,
                                          signals: signals)
        else { return }
        defer { WatchCtl.freeCfg(cfg) }
        var hit = hb_watch_hit()
        let hit_ = hb_watch_test_text(cfg, textV.string, &hit)
        if hit_ {
            result.stringValue = String(format: L("命中：%@ (%@) — %@"),
                hit.rule?.str ?? "?",
                hit.ok ? "ok" : "err",
                hit.line?.str ?? "")
            hb_watch_hit_clear(&hit)
        } else {
            result.stringValue = L("未命中")
        }
    }
}

/// shell 集成安装器（对齐 on_installer/install_local/remote_cmd）：
///   Local → 写 ~/.config/harbor/shell/harbor.<sh> + rc 追加（$HOME 可覆）；
///   SSH  → 确认后 remote_cmd 写进会话。
final class WatchInstaller: NSObject {
    private static var live: [WatchInstaller] = []
    private weak var leaf: TermTab?
    private var popup: NSPopUpButton!
    private var preview: NSTextField!
    private var alert: NSAlert!
    private static let shells = ["bash", "zsh", "fish"]

    static func present(leaf: TermTab) {
        let i = WatchInstaller()
        i.leaf = leaf
        live.append(i)
        i.show()
    }
    /// rc 追加行（对齐 local_rc_line）。
    static func rcLine(_ shell: String) -> String {
        if shell == "fish" {
            return "test -r ~/.config/harbor/shell/harbor.fish; and " +
                   "source ~/.config/harbor/shell/harbor.fish" +
                   "  # Harbor shell integration"
        }
        return "[ -r ~/.config/harbor/shell/harbor.\(shell) ] && " +
               ". ~/.config/harbor/shell/harbor.\(shell)" +
               "  # Harbor shell integration"
    }
    /// rc 文件路径（$HOME 可覆 —— 测试指到 ZDOTDIR/HOME）。
    static func rcPath(_ shell: String) -> String {
        let home = ProcessInfo.processInfo.environment["HOME"]
                   ?? NSHomeDirectory()
        switch shell {
        case "fish":
            return "\(home)/.config/fish/conf.d/harbor.fish"
        case "zsh": return "\(home)/.zshrc"
        default:    return "\(home)/.bashrc"
        }
    }
    /// 远端安装命令（对齐 remote_cmd）。
    static func remoteCmd(_ shell: String) -> String {
        let script = hb_shell_integration_script(shell)
            .map { String(cString: $0) } ?? ""
        let sp = "~/.config/harbor/shell/harbor.\(shell)"
        var cmd = "mkdir -p ~/.config/harbor/shell && " +
                  "cat > \(sp) <<'HARBOR_SI_EOF'\n" +
                  script + "HARBOR_SI_EOF\n"
        if shell == "fish" {
            cmd += "mkdir -p ~/.config/fish/conf.d && printf '%s\\n' " +
                   "'\(rcLine("fish"))' " +
                   "> ~/.config/fish/conf.d/harbor.fish"
        } else {
            cmd += "grep -q 'Harbor shell integration' ~/.${shell}rc " +
                   "2>/dev/null || " +
                   "echo '\(rcLine(shell))' >> ~/.${shell}rc"
        }
        return cmd
    }
    /// 本地安装（对齐 install_local；尊重 $HOME → 测试可用临时 HOME）。
    @discardableResult
    static func installLocal(_ shell: String) -> (Bool, String?) {
        guard let script = hb_shell_integration_script(shell)
            .map({ String(cString: $0) }) else {
            return (false, L("不支持的 shell"))
        }
        let home = ProcessInfo.processInfo.environment["HOME"]
                   ?? NSHomeDirectory()
        let dir = "\(home)/.config/harbor/shell"
        do {
            try FileManager.default.createDirectory(
                atPath: dir, withIntermediateDirectories: true)
        } catch {
            return (false, L("无法创建 ") + dir)
        }
        let sp = "\(dir)/harbor.\(shell)"
        do {
            try script.write(toFile: sp, atomically: true,
                             encoding: .utf8)
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o644], ofItemAtPath: sp)
        } catch {
            return (false, L("无法写入 ") + sp)
        }
        let rp = rcPath(shell)
        let rl = rcLine(shell)
        if shell == "fish" {
            let pd = (rp as NSString).deletingLastPathComponent
            try? FileManager.default.createDirectory(
                atPath: pd, withIntermediateDirectories: true)
            do {
                try (rl + "\n").write(toFile: rp, atomically: true,
                                      encoding: .utf8)
            } catch { return (false, L("无法写入 ") + rp) }
        } else {
            let old = (try? String(contentsOfFile: rp,
                                   encoding: .utf8)) ?? ""
            if !old.contains("Harbor shell integration") {
                do {
                    try (old + rl + "\n").write(
                        toFile: rp, atomically: true, encoding: .utf8)
                } catch { return (false, L("无法写入 ") + rp) }
            }
        }
        return (true, nil)
    }

    private func show() {
        let a = NSAlert()
        alert = a
        a.messageText = L("安装 shell 集成")
        a.informativeText = leaf?.isSSHLeaf == true
            ? L("OSC 133 标记让监控拿到精确的命令边界与退出码。" +
                "将在远端 shell rc 中注入集成脚本（需确认远端在提示符下）。")
            : L("OSC 133 标记让监控拿到精确的命令边界与退出码。" +
                "脚本写入 ~/.config/harbor/shell 并在 rc 中追加一行加载。")
        a.addButton(withTitle: L("安装"))
        a.addButton(withTitle: L("关闭"))
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false
        popup = NSPopUpButton()
        popup.addItems(withTitles: WatchInstaller.shells)
        // 默认 shell：Local 取 $SHELL（macOS 默认 zsh）；SSH 固定 bash
        var sel = "bash"
        if leaf?.isSSHLeaf != true,
           let sh = ProcessInfo.processInfo.environment["SHELL"]?
            .split(separator: "/").last.map(String.init),
           WatchInstaller.shells.contains(sh) { sel = sh }
        popup.selectItem(withTitle: sel)
        popup.target = self
        popup.action = #selector(shellChanged)
        popup.setAccessibilityIdentifier("watch-inst-shell")
        preview = NSTextField(wrappingLabelWithString: "")
        preview.font = .monospacedSystemFont(ofSize: 10,
                                             weight: .regular)
        preview.textColor = Theme.termDim.color
        preview.isSelectable = true
        preview.setAccessibilityIdentifier("watch-inst-preview")
        acc.addSubview(popup); acc.addSubview(preview)
        popup.translatesAutoresizingMaskIntoConstraints = false
        preview.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            acc.widthAnchor.constraint(equalToConstant: 430),
            popup.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            popup.topAnchor.constraint(equalTo: acc.topAnchor),
            preview.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            preview.trailingAnchor.constraint(
                equalTo: acc.trailingAnchor),
            preview.topAnchor.constraint(equalTo: popup.bottomAnchor,
                                         constant: 8),
            preview.bottomAnchor.constraint(equalTo: acc.bottomAnchor),
        ])
        a.accessoryView = acc
        refreshPreview()
        // SSH 且无活会话 → 禁安装
        if leaf?.isSSHLeaf == true,
           !(leaf?.session?.alive ?? false) {
            a.buttons.first?.isEnabled = false
        }
        DialogRegistry.shared.present(alert: a, kind: "watch-install",
                                      in: leaf?.view.window
                                          as? MainWindow)
        { [weak self] ok in
            defer {
                if let s = self {
                    WatchInstaller.live.removeAll { $0 === s }
                }
            }
            guard ok, let self, let leaf = self.leaf else { return }
            self.install(leaf)
        }
    }
    private var shell: String {
        popup?.titleOfSelectedItem ?? "zsh"
    }
    @objc private func shellChanged() { refreshPreview() }
    private func refreshPreview() {
        if leaf?.isSSHLeaf == true {
            preview.stringValue =
                L("远端将执行：\n") + WatchInstaller.remoteCmd(shell)
        } else {
            preview.stringValue = String(format:
                L("将写入 %@ 并向 %@ 追加：\n%@"),
                "~/.config/harbor/shell/harbor.\(shell)",
                WatchInstaller.rcPath(shell),
                WatchInstaller.rcLine(shell))
        }
    }
    private func install(_ leaf: TermTab) {
        if leaf.isSSHLeaf {
            // SSH：先显式确认再把命令写进会话（对齐 on_inst_run）
            let cmd = WatchInstaller.remoteCmd(shell)
            let a = NSAlert()
            a.messageText = L("安装 shell 集成")
            a.informativeText = L("将在远端执行以下命令，" +
                                  "请确认远端处于 shell 提示符：\n\n") +
                              cmd
            a.addButton(withTitle: L("执行"))
            a.addButton(withTitle: L("取消"))
            DialogRegistry.shared.present(alert: a, kind: "inst-confirm",
                in: leaf.view.window as? MainWindow) { ok in
                guard ok, let s = leaf.session, s.alive else { return }
                let nl = cmd + "\n"
                nl.withCString { hb_session_write(s.sess, $0, nl.utf8.count) }
            }
        } else {
            let (ok, err) = WatchInstaller.installLocal(shell)
            if !ok {
                let a = NSAlert()
                a.messageText = L("安装失败")
                a.informativeText = err ?? ""
                DialogRegistry.shared.present(alert: a, kind: "inst-err",
                    in: leaf.view.window as? MainWindow) { _ in }
            }
        }
    }
}
