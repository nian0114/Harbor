import AppKit
import CHarbor
import HarborKit

/// SSH 标签页：连接进度 → 终端；断线 → 状态条+自动重连；结束 → 重连条。
/// 布局对齐 linux hb-connect-view.c：头部图标+名称、步骤节点行、
/// 日志卡片、关闭/重新开始按钮；失败时红色卡片（标题/详情/编辑主机/重试）。
final class SSHTab: TermTab, ConnDelegate {
    let conn: ConnCtl
    private var stageView: NSView?
    private var stageLines = ""
    private var logLabel: NSTextField?
    private var dots: [NSTextField] = []
    private static let stages = [L("解析"), "TCP", L("密钥交换"),
                                 L("认证"), "shell"]
    private var statusBar: NSView?
    private var ended = false
    private var failed = false
    private var hostColorIdx = 0

    init(hostId: String?, quickTarget: String?, window: NSWindow?,
         title: String) {
        conn = ConnCtl(mode: .shell, hostId: hostId,
                       quickTarget: quickTarget, window: window)
        super.init(title: title, view: NSView())
        conn.delegate = self
        let term = TerminalView()
        term.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(term)
        NSLayoutConstraint.activate([
            term.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            term.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            term.topAnchor.constraint(equalTo: view.topAnchor),
            term.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        term.isHidden = true
        self.term = term
        buildStageView()
    }

    func connect() {
        statusDot = Theme.dotPend
        guard let term else { return }
        conn.start(rows: Int32(term.rows()), cols: Int32(term.cols()))
    }

    // ---------- 连接进度视图 ----------
    private func buildStageView() {
        let v = NSView()
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.page.color.cgColor
        v.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(v)
        NSLayoutConstraint.activate([
            v.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            v.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            v.topAnchor.constraint(equalTo: view.topAnchor),
            v.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        // 头部：OS 图标 + 名称 + 地址
        let icon = NSView()
        icon.wantsLayer = true
        icon.layer?.cornerRadius = 10
        icon.layer?.backgroundColor = Theme.dotPend.color.cgColor
        icon.translatesAutoresizingMaskIntoConstraints = false
        let nameL = NSTextField(labelWithString: title)
        nameL.font = .boldSystemFont(ofSize: 15)
        nameL.translatesAutoresizingMaskIntoConstraints = false
        let hideB = NSButton(title: L("隐藏日志"), target: self,
                             action: #selector(toggleLog))
        hideB.bezelStyle = .rounded
        hideB.translatesAutoresizingMaskIntoConstraints = false
        hideB.setAccessibilityIdentifier("conn-hidelog")
        v.addSubview(icon); v.addSubview(nameL); v.addSubview(hideB)
        // 步骤节点行
        let steps = NSView()
        steps.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(steps)
        dots = []
        for (i, s) in SSHTab.stages.enumerated() {
            let d = NSTextField(labelWithString: "○\n" + s)
            d.alignment = .center
            d.font = .systemFont(ofSize: 10)
            d.setAccessibilityIdentifier("stage-\(i)")
            steps.addSubview(d); dots.append(d)
        }
        // 日志卡片
        let card = NSView()
        card.wantsLayer = true
        card.layer?.cornerRadius = 8
        card.layer?.backgroundColor = Theme.card.color.cgColor
        card.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(card)
        let l = NSTextField(wrappingLabelWithString: "")
        l.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        l.translatesAutoresizingMaskIntoConstraints = false
        l.setAccessibilityIdentifier("stage-log")
        card.addSubview(l)
        logLabel = l
        // 底部按钮
        let close = NSButton(title: L("关闭"), target: self,
                             action: #selector(closeSelf))
        close.bezelStyle = .rounded
        let over = NSButton(title: L("重新开始"), target: self,
                            action: #selector(retry))
        over.bezelStyle = .rounded
        over.setAccessibilityIdentifier("conn-restart")
        [close, over].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            v.addSubview($0)
        }
        NSLayoutConstraint.activate([
            icon.leadingAnchor.constraint(equalTo: v.leadingAnchor,
                                          constant: 24),
            icon.topAnchor.constraint(equalTo: v.topAnchor, constant: 24),
            icon.widthAnchor.constraint(equalToConstant: 40),
            icon.heightAnchor.constraint(equalToConstant: 40),
            nameL.leadingAnchor.constraint(equalTo: icon.trailingAnchor,
                                           constant: 12),
            nameL.centerYAnchor.constraint(equalTo: icon.centerYAnchor),
            hideB.trailingAnchor.constraint(equalTo: v.trailingAnchor,
                                            constant: -20),
            hideB.centerYAnchor.constraint(equalTo: icon.centerYAnchor),
            steps.topAnchor.constraint(equalTo: icon.bottomAnchor,
                                       constant: 18),
            steps.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            steps.widthAnchor.constraint(equalToConstant: 420),
            steps.heightAnchor.constraint(equalToConstant: 36),
            card.topAnchor.constraint(equalTo: steps.bottomAnchor,
                                      constant: 12),
            card.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            card.widthAnchor.constraint(equalToConstant: 460),
            card.heightAnchor.constraint(equalToConstant: 170),
            l.topAnchor.constraint(equalTo: card.topAnchor, constant: 10),
            l.leadingAnchor.constraint(equalTo: card.leadingAnchor,
                                       constant: 12),
            l.trailingAnchor.constraint(equalTo: card.trailingAnchor,
                                        constant: -12),
            close.topAnchor.constraint(equalTo: card.bottomAnchor,
                                       constant: 14),
            close.trailingAnchor.constraint(equalTo: over.leadingAnchor,
                                            constant: -10),
            over.trailingAnchor.constraint(equalTo: card.trailingAnchor),
            over.centerYAnchor.constraint(equalTo: close.centerYAnchor),
        ])
        var x: CGFloat = 0
        for (i, d) in dots.enumerated() {
            d.frame = NSRect(x: x, y: 0, width: 80, height: 36)
            if i > 0 {
                let ln = NSView()
                ln.wantsLayer = true
                ln.layer?.backgroundColor = Theme.blue.color.cgColor
                ln.frame = NSRect(x: x - 14, y: 24, width: 10, height: 2)
                steps.addSubview(ln)
            }
            x += 84
        }
        stageView = v
    }
    @objc private func toggleLog() { logLabel?.isHidden.toggle() }
    @objc private func closeSelf() {
        // 关闭当前 tab（经窗口）
        if let w = view.window as? MainWindow {
            w.closeTab(w.selTab)
        }
    }

    // ---------- ConnDelegate ----------
    func conn(_ c: ConnCtl, stage idx: Int, line: String, mark: Character) {
        stageLines += line + "\n"
        logLabel?.stringValue = stageLines
        if idx >= 0 && idx < dots.count {
            for i in 0...idx {
                dots[i].stringValue = "●\n" + SSHTab.stages[i]
            }
        }
    }
    func connSession(_ c: ConnCtl, _ s: Session) {
        session = s
        s.onEvent = { [weak self, weak term = self.term] ev in
            guard let self else { return }
            var e = ev
            if self.conn.handleEvent(&e) { return }
            switch ev.type {
            case HB_SESS_EV_OUTPUT: term?.poke()
            case HB_SESS_EV_TITLE:
                let t = ev.text.str
                if !t.isEmpty { self.title = t }
            case HB_SESS_EV_BELL: term?.bell()
            case HB_SESS_EV_EXIT:
                self.statusDot = Theme.dotErr
                self.showEnded(code: ev.code)
            default: break
            }
        }
        term?.session = s
        s.drain()
    }
    func connConnected(_ c: ConnCtl) {
        statusDot = Theme.dotOk
        ended = false
        stageView?.removeFromSuperview(); stageView = nil
        statusBar?.removeFromSuperview(); statusBar = nil
        term?.isHidden = false
        term?.poke()
        view.window?.makeFirstResponder(term)
    }
    func connFailed(_ c: ConnCtl, code: Int, title t: String,
                    details: String?) {
        statusDot = Theme.dotErr
        failed = true
        statusBar?.removeFromSuperview(); statusBar = nil
        stageView?.removeFromSuperview(); stageView = nil
        view.layer?.backgroundColor = Theme.page.color.cgColor
        view.wantsLayer = true
        // 红色失败卡片（与 Linux 一致：标题 + ▸技术详情 + 关闭/编辑主机/重试）
        let card = NSView()
        card.wantsLayer = true
        card.layer?.cornerRadius = 8
        card.layer?.backgroundColor = Theme.dangerBg.color.cgColor
        card.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(card)
        NSLayoutConstraint.activate([
            card.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            card.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            card.widthAnchor.constraint(equalToConstant: 420),
        ])
        let tl = NSTextField(labelWithString: t)
        tl.font = .boldSystemFont(ofSize: 13)
        tl.translatesAutoresizingMaskIntoConstraints = false
        tl.setAccessibilityIdentifier("conn-error-title")
        let det = NSTextField(wrappingLabelWithString:
            L("▸ 技术详情") + "  " + (details ?? stageLines))
        det.font = .systemFont(ofSize: 11)
        det.textColor = .secondaryLabelColor
        det.translatesAutoresizingMaskIntoConstraints = false
        det.setAccessibilityIdentifier("conn-error-details")
        let close = NSButton(title: L("关闭"), target: self,
                             action: #selector(closeSelf))
        let edit = NSButton(title: L("编辑主机"), target: self,
                            action: #selector(editHost))
        let retry = NSButton(title: L("重试"), target: self,
                             action: #selector(retry))
        [close, edit, retry].forEach {
            $0.bezelStyle = .rounded
            $0.translatesAutoresizingMaskIntoConstraints = false
            card.addSubview($0)
        }
        retry.keyEquivalent = "\r"
        card.addSubview(tl); card.addSubview(det)
        close.setAccessibilityIdentifier("conn-close")
        edit.setAccessibilityIdentifier("conn-edit-host")
        retry.setAccessibilityIdentifier("conn-retry")
        NSLayoutConstraint.activate([
            tl.topAnchor.constraint(equalTo: card.topAnchor, constant: 14),
            tl.leadingAnchor.constraint(equalTo: card.leadingAnchor,
                                        constant: 16),
            det.topAnchor.constraint(equalTo: tl.bottomAnchor, constant: 6),
            det.leadingAnchor.constraint(equalTo: tl.leadingAnchor),
            det.trailingAnchor.constraint(equalTo: card.trailingAnchor,
                                          constant: -16),
            retry.trailingAnchor.constraint(equalTo: card.trailingAnchor,
                                            constant: -16),
            retry.bottomAnchor.constraint(equalTo: card.bottomAnchor,
                                          constant: -14),
            retry.topAnchor.constraint(equalTo: det.bottomAnchor,
                                       constant: 12),
            edit.trailingAnchor.constraint(equalTo: retry.leadingAnchor,
                                           constant: -8),
            edit.centerYAnchor.constraint(equalTo: retry.centerYAnchor),
            close.trailingAnchor.constraint(equalTo: edit.leadingAnchor,
                                            constant: -8),
            close.centerYAnchor.constraint(equalTo: retry.centerYAnchor),
        ])
        stageView = card
    }
    func connDisconnected(_ c: ConnCtl) {
        statusDot = Theme.dotPend
        showStatus(L("连接已断开，正在自动重连…"))
    }
    func connExited(_ c: ConnCtl, code: Int, reason: String) {
        statusDot = Theme.dotErr
        if failed { return }              // 失败卡片已覆盖
        if c.reconnectPending { return }  // 自动重连条已显示
        showEnded(code: Int32(code))
    }
    func connOSInfo(_ c: ConnCtl, id: String, pretty: String) {}

    /// SSH 会话结束：重连条（不是 Local 的 已结束 条）。
    private func showEnded(code: Int32) {
        guard !ended else { return }
        ended = true
        showStatus(L("会话已结束") + " (\(code)) · " + L("连接已断开"),
                   button: L("重新连接"), action: #selector(retry))
    }
    private func showStatus(_ text: String, button bt: String? = nil,
                            action: Selector? = nil) {
        statusBar?.removeFromSuperview()
        let b = NSView()
        b.wantsLayer = true
        b.layer?.backgroundColor = Theme.amber.color.cgColor
        b.translatesAutoresizingMaskIntoConstraints = false
        let l = NSTextField(labelWithString: text)
        l.translatesAutoresizingMaskIntoConstraints = false
        l.setAccessibilityIdentifier("reconnect-status")
        b.addSubview(l)
        view.addSubview(b)
        var cons = [
            b.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            b.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            b.topAnchor.constraint(equalTo: view.topAnchor),
            b.heightAnchor.constraint(equalToConstant: 26),
            l.centerYAnchor.constraint(equalTo: b.centerYAnchor),
        ]
        if let bt, let action {
            let bb = NSButton(title: bt, target: self, action: action)
            bb.bezelStyle = .rounded
            bb.translatesAutoresizingMaskIntoConstraints = false
            b.addSubview(bb)
            cons += [
                bb.trailingAnchor.constraint(equalTo: b.trailingAnchor,
                                             constant: -8),
                bb.centerYAnchor.constraint(equalTo: b.centerYAnchor),
                l.trailingAnchor.constraint(equalTo: bb.leadingAnchor,
                                            constant: -8),
                l.leadingAnchor.constraint(equalTo: b.leadingAnchor,
                                           constant: 12),
            ]
        } else {
            cons.append(l.centerXAnchor.constraint(equalTo: b.centerXAnchor))
        }
        NSLayoutConstraint.activate(cons)
        statusBar = b
    }

    @objc private func editHost() {
        guard let hid = connHostId else { retry(); return }
        // 打开主机编辑表单
        if let hp = view.window?.contentView?.findPage(HostsPage.self),
           let h = hp.store.hosts.first(where: { $0.id == hid }) {
            hp.editHost(h)
        } else { retry() }
    }
    private var connHostId: String? {
        // hostId 存于 ConnCtl 内部；通过 vault 反查 title 不可靠，
        // 这里直接给 ConnCtl 暴露的 id。
        return conn.hostId
    }
    @objc private func retry() {
        stageView?.removeFromSuperview(); stageView = nil
        statusBar?.removeFromSuperview(); statusBar = nil
        ended = false
        failed = false
        stageLines = ""
        buildStageView()
        conn.reconnectNow()
    }

    override var running: Bool { session?.alive ?? false }
    @objc override func restart() { retry() }
    override func kill() {
        conn.cancel()
        conn.join()
        super.kill()
    }
}

extension NSView {
    func findPage<T: NSView>(_ t: T.Type) -> T? {
        for s in subviews { if let m = s as? T { return m }
            if let r = s.findPage(t) { return r } }
        return nil
    }
}
