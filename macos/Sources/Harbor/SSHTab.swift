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
    private var logAttr = NSMutableAttributedString()
    private var logCard: NSView?
    private var hideBtn: NSButton?
    private var stageFoot: [NSButton] = []
    private var dots: [NSTextField] = []
    private static let stages = [L("解析"), "TCP", L("密钥交换"),
                                 L("认证"), "shell"]
    private var statusBar: NSView?
    private var ended = false
    private var failed = false
    private var hostColorIdx = 0
    private var iconTile: NSView?
    private var iconImg: NSImageView?
    private var errDet: NSTextField?
    private var errDetH: NSLayoutConstraint?
    private var expBtn: NSButton?

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
        // 头部：OS 图标瓦片 + 名称 + 地址（对齐 Linux hb-connect-view）
        let tile = osTile(conn.hostOS)
        let icon = NSView()
        icon.wantsLayer = true
        icon.layer?.cornerRadius = 10
        icon.layer?.backgroundColor = tile.bg.color.cgColor
        icon.translatesAutoresizingMaskIntoConstraints = false
        iconTile = icon
        let img = NSImageView(image: hbIcon(tile.icon, size: 22,
                                            tint: .white))
        img.translatesAutoresizingMaskIntoConstraints = false
        icon.addSubview(img)
        iconImg = img
        NSLayoutConstraint.activate([
            img.centerXAnchor.constraint(equalTo: icon.centerXAnchor),
            img.centerYAnchor.constraint(equalTo: icon.centerYAnchor),
        ])
        let nameL = NSTextField(labelWithString: title)
        nameL.font = .boldSystemFont(ofSize: 15)
        nameL.translatesAutoresizingMaskIntoConstraints = false
        // 副标题 "SSH host:port"（对齐 Linux 头部）
        let sub = NSTextField(labelWithString: "SSH \(conn.targetDesc)")
        sub.font = .systemFont(ofSize: 11)
        sub.textColor = .secondaryLabelColor
        sub.translatesAutoresizingMaskIntoConstraints = false
        let hideB = NSButton(title: L("隐藏日志"), target: self,
                             action: #selector(toggleLog))
        hideB.bezelStyle = .rounded
        hideB.translatesAutoresizingMaskIntoConstraints = false
        hideB.setAccessibilityIdentifier("conn-hidelog")
        v.addSubview(icon); v.addSubview(nameL); v.addSubview(sub)
        v.addSubview(hideB)
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
        logCard = card
        hideBtn = hideB
        // 底部按钮
        let close = NSButton(title: L("关闭"), target: self,
                             action: #selector(closeSelf))
        close.bezelStyle = .rounded
        let over = NSButton(title: L("重新开始"), target: self,
                            action: #selector(retry))
        over.bezelStyle = .rounded
        over.setAccessibilityIdentifier("conn-restart")
        stageFoot = [close, over]
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
            nameL.centerYAnchor.constraint(equalTo: icon.centerYAnchor,
                                           constant: -8),
            sub.leadingAnchor.constraint(equalTo: nameL.leadingAnchor),
            sub.topAnchor.constraint(equalTo: nameL.bottomAnchor,
                                     constant: 2),
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
    private func flipErrDetails() {
        guard let det = errDet else { return }
        det.isHidden.toggle()
        errDetH?.isActive = det.isHidden
    }
    @objc private func toggleErrDetails(_ b: NSButton) {
        flipErrDetails()
    }
    // 点击整行（标签）：同步三角钮的开合状态再翻页
    @objc private func toggleErrDetailsRow(_ g: NSClickGestureRecognizer) {
        if let b = expBtn { b.state = b.state == .on ? .off : .on }
        flipErrDetails()
    }
    @objc private func closeSelf() {
        // 关闭当前 tab（经窗口）
        if let w = view.window as? MainWindow {
            w.closeTab(w.selTab)
        }
    }

    // ---------- ConnDelegate ----------
    func conn(_ c: ConnCtl, stage idx: Int, line: String, mark: Character) {
        stageLines += line + "\n"
        // 错误行标红（对齐 Linux add_log_line 的 hb-red）
        logAttr.append(NSAttributedString(
            string: line + "\n",
            attributes: [
                .font: NSFont.monospacedSystemFont(ofSize: 11,
                                                   weight: .regular),
                .foregroundColor: mark == "e"
                    ? NSColor.systemRed : NSColor.labelColor,
            ]))
        logLabel?.attributedStringValue = logAttr
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
            case HB_SESS_EV_WATCH:
                if let h = ev.watch { self.handleWatchHit(h.pointee) }
            case HB_SESS_EV_EXIT:
                self.statusDot = Theme.dotErr
                self.showEnded(code: ev.code)
            default: break
            }
        }
        term?.session = s
        applyWatch()
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
        // Linux 对齐：整个连接视图（头部+步骤+日志）保留，错误卡片插到
        // 日志卡片之下；隐藏 隐藏日志 与底部 关闭/重新开始 行。
        hideBtn?.isHidden = true
        stageFoot.forEach { $0.isHidden = true }
        let card = NSView()
        card.wantsLayer = true
        card.layer?.cornerRadius = 8
        card.layer?.backgroundColor = Theme.dangerBg.color.cgColor
        card.translatesAutoresizingMaskIntoConstraints = false
        let host = stageView ?? view   // stageView 在 → 卡其下；否则叠底
        host.addSubview(card)
        if let lc = logCard, host === stageView {
            NSLayoutConstraint.activate([
                card.topAnchor.constraint(equalTo: lc.bottomAnchor,
                                          constant: 12),
            ])
        } else {
            NSLayoutConstraint.activate([
                card.centerYAnchor.constraint(equalTo: host.centerYAnchor),
            ])
        }
        NSLayoutConstraint.activate([
            card.centerXAnchor.constraint(equalTo: host.centerXAnchor),
            card.widthAnchor.constraint(equalToConstant: 430),
        ])
        let tl = NSTextField(labelWithString: t)
        tl.font = .boldSystemFont(ofSize: 13)
        tl.translatesAutoresizingMaskIntoConstraints = false
        tl.setAccessibilityIdentifier("conn-error-title")
        card.addSubview(tl)
        // 人类可读提示行（对齐 Linux；libssh2 原文只在 ▸技术详情 里）
        var hintText: String?; var hintRed = false
        switch UInt32(bitPattern: Int32(code)) {
        case HB_SSH_ERR_AUTH.rawValue:
            hintText = L("服务器拒绝了凭据。检查用户名、密码或密钥后重试。")
        case HB_SSH_ERR_HOSTKEY_CHANGED.rawValue:
            hintText = L("为安全起见已阻断连接 —— 主机密钥与本地记录不符。")
            hintRed = true
        case HB_SSH_ERR_REFUSED.rawValue:
            hintText = L("目标端口拒绝连接。确认服务已启动、端口正确。")
        case HB_SSH_ERR_UNREACHABLE.rawValue, HB_SSH_ERR_RESOLVE.rawValue:
            hintText = L("无法到达主机。检查地址和网络连接。")
        case HB_SSH_ERR_TIMEOUT.rawValue:
            hintText = L("连接等待超时。检查网络或稍后重试。")
        default: break
        }
        var anchor: NSView = tl
        if let ht = hintText {
            let hl = NSTextField(wrappingLabelWithString: ht)
            hl.font = .systemFont(ofSize: 11)
            hl.textColor = hintRed ? .systemRed : .secondaryLabelColor
            hl.translatesAutoresizingMaskIntoConstraints = false
            card.addSubview(hl)
            NSLayoutConstraint.activate([
                hl.topAnchor.constraint(equalTo: tl.bottomAnchor,
                                        constant: 6),
                hl.leadingAnchor.constraint(equalTo: tl.leadingAnchor),
                hl.trailingAnchor.constraint(equalTo: card.trailingAnchor,
                                             constant: -16),
            ])
            anchor = hl
        }
        // ▸ 技术详情：三角 disclosure 钮 + 标签（对齐 Linux expander），
        // libssh2 原文只在展开后显示；标题不进按钮，避免渲染出杂散字形。
        let expB = NSButton(title: "", target: self,
                            action: #selector(toggleErrDetails))
        expB.bezelStyle = .disclosure
        expB.setButtonType(.pushOnPushOff)
        expB.translatesAutoresizingMaskIntoConstraints = false
        expB.setAccessibilityIdentifier("conn-error-exp")
        expBtn = expB
        let expL = NSTextField(labelWithString: L("技术详情"))
        expL.font = .systemFont(ofSize: 11)
        expL.textColor = .secondaryLabelColor
        expL.translatesAutoresizingMaskIntoConstraints = false
        let expRow = NSView()
        expRow.translatesAutoresizingMaskIntoConstraints = false
        expRow.addSubview(expB); expRow.addSubview(expL)
        expRow.addGestureRecognizer(NSClickGestureRecognizer(
            target: self, action: #selector(toggleErrDetailsRow(_:))))
        NSLayoutConstraint.activate([
            expB.leadingAnchor.constraint(equalTo: expRow.leadingAnchor),
            expB.topAnchor.constraint(equalTo: expRow.topAnchor),
            expB.bottomAnchor.constraint(equalTo: expRow.bottomAnchor),
            expL.leadingAnchor.constraint(equalTo: expB.trailingAnchor),
            expL.centerYAnchor.constraint(equalTo: expRow.centerYAnchor),
            expL.trailingAnchor.constraint(equalTo: expRow.trailingAnchor),
        ])
        card.addSubview(expRow)
        let det = NSTextField(wrappingLabelWithString: details ?? stageLines)
        det.font = .systemFont(ofSize: 11)
        det.textColor = .secondaryLabelColor
        det.isHidden = true
        det.translatesAutoresizingMaskIntoConstraints = false
        det.setAccessibilityIdentifier("conn-error-details")
        // 收起时 hidden 仍占布局空间 → 用高度约束压成 0，展开时放开。
        let detH = det.heightAnchor.constraint(equalToConstant: 0)
        detH.isActive = true
        errDet = det; errDetH = detH
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
        // 重试 = 蓝色主按钮（对齐 HostsPage primaryBtn 的画法：
        // bezelColor 在这套 macOS 上不生效，必须自绘 layer）。
        retry.isBordered = false
        retry.wantsLayer = true
        retry.layer?.cornerRadius = 6
        retry.layer?.backgroundColor = Theme.blue.color.cgColor
        retry.attributedTitle = NSAttributedString(
            string: L("重试"), attributes: [
                .foregroundColor: NSColor.white,
                .font: NSFont.systemFont(ofSize: 12, weight: .medium)])
        retry.keyEquivalent = "\r"
        card.addSubview(det)
        close.setAccessibilityIdentifier("conn-close")
        edit.setAccessibilityIdentifier("conn-edit-host")
        retry.setAccessibilityIdentifier("conn-retry")
        NSLayoutConstraint.activate([
            tl.topAnchor.constraint(equalTo: card.topAnchor, constant: 14),
            tl.leadingAnchor.constraint(equalTo: card.leadingAnchor,
                                        constant: 16),
            expRow.topAnchor.constraint(equalTo: anchor.bottomAnchor,
                                        constant: 6),
            expRow.leadingAnchor.constraint(equalTo: tl.leadingAnchor),
            det.topAnchor.constraint(equalTo: expRow.bottomAnchor,
                                     constant: 4),
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
    func connOSInfo(_ c: ConnCtl, id: String, pretty: String) {
        // 探测到的系统写回主机对象 → 主机页 OS 图标（与 Linux 一致）；
        // 头部瓦片同步换色（连接视图存活期间）。
        if !id.isEmpty {
            let t = osTile(id)
            iconTile?.layer?.backgroundColor = t.bg.color.cgColor
            iconImg?.image = hbIcon(t.icon, size: 22, tint: .white)
        }
        guard !id.isEmpty, let hostId = conn.hostId,
              let o = Vault.shared.get(hostId) else { return }
        if hb_obj_get_str(o, "os").str != id {
            hb_obj_set_str(o, "os", id)
            Vault.shared.put(o)
            if let w = view.window as? MainWindow { w.refreshHosts() }
        }
        hb_obj_free(o)
    }

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
        logAttr = NSMutableAttributedString()
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

    // ---- Workspace 叶钩子 ----
    override var isSSHLeaf: Bool { true }
    override var wsIconName: String { osTile(conn.hostOS).icon }
    override var wsHostId: String? { conn.hostId }
    override var wsDead: Bool {
        ended || failed || (session != nil && !session!.alive)
    }
    /// 分屏克隆：同 host（或 quick target）新连接（对齐 clone_page）。
    override func wsClone() -> TermTab? {
        let t = SSHTab(hostId: conn.hostId, quickTarget: conn.quickTarget,
                       window: view.window, title: title)
        t.connect()
        return t
    }
}

extension NSView {
    func findPage<T: NSView>(_ t: T.Type) -> T? {
        for s in subviews { if let m = s as? T { return m }
            if let r = s.findPage(t) { return r } }
        return nil
    }
}
