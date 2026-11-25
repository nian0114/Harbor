import AppKit
import CHarbor
import HarborKit

/// 分屏容器（对齐 linux hb-workspace.c）：
///   视图 = [工具栏][find bar][分屏树]；每个会话 tab 恒挂一个 Workspace
///   （n=1 时隐藏 pane-head，外观与原单页一致）。
///   叶子 = TermTab（Local）/ SSHTab；内节点 = NSSplitView（可拖分隔条）。
///   broadcast 开 → 所有 pane 绿框 + 输入镜像；focused → 蓝框；
///   focus 模式 → 焦点 pane 独占 bodyHost，再按一次还原原槽位。

/// 输入镜像事件（对齐 hb_terminal_mirror_event）：
/// 源 pane 的键/字符/粘贴/裸字节 → 广播期转发给其它 pane 的 session。
enum MirrorEv {
    case key(Int32, Int32)            // hb_key raw + mods
    case unichar(UInt32, Int32)
    case paste(String)
    case raw(Data)
}

/// 分隔条厚度（对齐 linux .ws-paned separator 5px）。
private final class WsSplitView: NSSplitView {
    override var dividerThickness: CGFloat { 5 }
}

/// prototype .tbtn：终端 chrome 按钮（bg t-line / border t-border /
/// fg t-title，radius 7，padding 5x11；on=绿、share-on=蓝；sm=pane-head
/// 的 padding 2x8）。颜色随终端主题（light 主题 on 态换浅绿）。
final class TBtn: NSButton {
    enum Mode { case plain, on, share }
    var mode: Mode = .plain { didSet { if mode != oldValue { restyle() } } }
    var mono = false { didSet { rebuildTitle() } }
    /// 自定义前导图（assist 主题页的 sw-dot）；优先于 icon 名。
    var leadingImage: NSImage? { didSet { rebuildTitle() } }
    private(set) var theme = TermTheme.for("harbor")
    private var text: String
    private var icon: String?
    private let sm: Bool
    private var hovering = false

    init(_ text: String, icon: String? = nil, sm: Bool = false,
         target: AnyObject? = nil, action: Selector? = nil) {
        self.text = text; self.icon = icon; self.sm = sm
        super.init(frame: .zero)
        self.target = target; self.action = action
        isBordered = false
        bezelStyle = .regularSquare
        alignment = .left
        wantsLayer = true
        layer?.cornerRadius = 7
        layer?.borderWidth = 1
        translatesAutoresizingMaskIntoConstraints = false
        heightAnchor.constraint(equalToConstant: sm ? 20 : 26).isActive = true
        addTrackingArea(NSTrackingArea(
            rect: .zero,
            options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
            owner: self))
        rebuildTitle()
        restyle()
    }
    required init?(coder: NSCoder) { fatalError() }

    func setContent(_ text: String, icon: String? = nil) {
        self.text = text; self.icon = icon
        rebuildTitle()
        invalidateIntrinsicContentSize()
    }
    var titleText: String { text }
    func applyTheme(_ t: TermTheme) { theme = t; restyle() }

    override var intrinsicContentSize: NSSize {
        var s = super.intrinsicContentSize
        s.width += sm ? 16 : 22
        s.height = sm ? 20 : 26
        return s
    }
    private func colors() -> (bg: Theme.RGBA, fg: Theme.RGBA,
                              bd: Theme.RGBA) {
        switch mode {
        case .on:
            return theme.id == "light"
                ? (.init(0xe6f6ee), .init(0x1a7f4b), .init(0xbfe6d2))
                : (.init(0x12351f), .init(0x6fe09c), .init(0x1f5c33))
        case .share:
            return (.init(0x10304f), .init(0x6cb8ff), .init(0x1d4d7d))
        case .plain:
            return (hovering ? theme.border : theme.line,
                    theme.title, theme.border)
        }
    }
    private func rebuildTitle() {
        let c = colors()
        let s = NSMutableAttributedString()
        if let img = leadingImage {
            let att = NSTextAttachment()
            att.image = img
            s.append(NSAttributedString(attachment: att))
            if !text.isEmpty {
                s.append(NSAttributedString(string: " ",
                                            attributes: [.kern: 4]))
            }
        } else if let icon {
            let att = NSTextAttachment()
            att.image = hbIcon(icon, size: sm ? 11 : 13, tint: c.fg.color)
            s.append(NSAttributedString(attachment: att))
            if !text.isEmpty {
                s.append(NSAttributedString(string: " ",
                                            attributes: [.kern: 4]))
            }
        }
        if !text.isEmpty {
            let f = mono
                ? NSFont.monospacedSystemFont(ofSize: sm ? 11 : 12,
                                              weight: .regular)
                : NSFont.systemFont(ofSize: sm ? 11 : 12)
            s.append(NSAttributedString(string: text, attributes: [
                .font: f, .foregroundColor: c.fg.color]))
        }
        attributedTitle = s
    }
    private func restyle() {
        let c = colors()
        layer?.backgroundColor = c.bg.color.cgColor
        layer?.borderColor = c.bd.color.cgColor
        rebuildTitle()
    }
    override func mouseEntered(with event: NSEvent) {
        hovering = true; restyle()
    }
    override func mouseExited(with event: NSEvent) {
        hovering = false; restyle()
    }
}

final class Workspace: NSObject, NSSplitViewDelegate {
    weak var tab: TermTab?
    let model: WsModel
    /// leafId → 叶子页对象（TermTab/SSHTab）
    private(set) var pages: [UUID: TermTab] = [:]
    private var boxes: [UUID: NSView] = [:]        // leafId → pane box
    private var margins: [UUID: NSView] = [:]      // leafId → 5px 容器
    private var heads: [UUID: NSView] = [:]
    private var headH: [UUID: NSLayoutConstraint] = [:]
    private var headBtns: [UUID: (img: NSImageView, lbl: NSTextField,
                                  reconn: TBtn, watch: TBtn,
                                  share: TBtn,
                                  badge: NSTextField, close: TBtn)] = [:]
    /// leafId → pane margin 四约束（单叶 0 / 多叶 6，对齐 .pane margin）
    private var mCons: [UUID: [NSLayoutConstraint]] = [:]
    /// 内节点 → 渲染出的 splitView（focus 还原 / frac 回收用）
    private var splitOf: [ObjectIdentifier: (sv: NSSplitView,
                                             node: WsModel.Node)] = [:]

    let view = NSView()
    private let bodyHost = NSView()
    private var treeView: NSView?
    private let bar = NSView()
    private let barSep = NSView()
    private let ttIcon = NSImageView()
    private let ttLabel = NSTextField(labelWithString: "")
    private let rightStack = NSStackView()
    private var bcastBtn: TBtn!
    private var focusBtn: TBtn!
    private var shareBtn: TBtn!
    private var watchBtn: TBtn!
    private var searchBtn: TBtn!
    private var reconnBtn: TBtn!
    private var assistBtn: TBtn!
    // term-status（prototype .term-status）
    private let statusView = NSView()
    private let statusSep = NSView()
    private let stOk = NSTextField(labelWithString: "")
    private let stTerm = NSTextField(labelWithString: "xterm-256color")
    private let stEnc = NSTextField(labelWithString: "UTF-8")
    private let stWatch = NSTextField(labelWithString: "")
    private let stShare = NSTextField(labelWithString: "")
    private let stSb = NSTextField(labelWithString: "")
    private let leftStack = NSStackView()
    private var chromeTheme = TermTheme.for("harbor")
    private var themeObs: NSObjectProtocol?
    let findHost = NSView()          // find bar 宿主（⌘F，见 find 补丁）
    private var findH: NSLayoutConstraint?

    var broadcast = false
    var focusMode = false
    /// focus 模式下被独占的 pane box / 原 margin / 原槽位
    private var fmBox: NSView?
    private var fmMargin: NSView?
    private var fmCons: [NSLayoutConstraint]?
    private var fmSlot: (node: WsModel.Node, index: Int, frac: Float)?

    init(seed: TermTab) {
        model = WsModel(firstId: seed.wsLeafId)
        super.init()
        buildChrome()
        seed.wsLeaf = self
        pages[seed.wsLeafId] = seed
        wireLeaf(seed)
        rebuild()
        focus(model.root, grab: false)
        themeObs = NotificationCenter.default.addObserver(
            forName: TerminalView.nTheme, object: nil,
            queue: .main) { [weak self] n in
                guard let self, let t = n.object as? TerminalView,
                      self.allTerms.contains(where: { $0 === t })
                else { return }
                self.applyChrome(t.theme)
            }
        applyChrome(seed.term?.theme ?? TermTheme.for("harbor"))
    }

    deinit {
        if let o = themeObs { NotificationCenter.default.removeObserver(o) }
    }

    // ---------------- 工具栏 + 状态栏（prototype term-toolbar/term-status） ----------------
    private let assistHost = NSView()
    private var assistW: NSLayoutConstraint?
    private var assistPanel: AssistPanel?
    private(set) var assistOn = false

    private func buildChrome() {
        view.wantsLayer = true
        bar.wantsLayer = true
        barSep.wantsLayer = true
        statusView.wantsLayer = true
        statusSep.wantsLayer = true
        ttLabel.font = .systemFont(ofSize: 12.5)
        ttLabel.lineBreakMode = .byTruncatingTail
        func mk(_ title: String, _ icon: String?, _ act: Selector,
                _ aid: String) -> TBtn {
            let b = TBtn(title, icon: icon, target: self, action: act)
            b.setAccessibilityIdentifier(aid)
            return b
        }
        watchBtn = mk(L("监控 关"), "eye", #selector(onWatchBtn), "ws-watch")
        shareBtn = mk(L("共享"), "broadcast", #selector(onShareBtn), "ws-share")
        searchBtn = mk("", "search", #selector(onSearchBtn), "ws-find")
        reconnBtn = mk(L("重连"), "refresh", #selector(onReconnBtn), "ws-reconn")
        bcastBtn = mk(L("Broadcast"), "broadcast",
                      #selector(toggleBroadcast), "ws-broadcast")
        assistBtn = mk(L("助手栏"), "panel", #selector(toggleAssist), "ws-assist")
        focusBtn = mk(L("Focus"), "focus",
                      #selector(toggleFocusMode), "ws-focus")
        rightStack.orientation = .horizontal
        rightStack.spacing = 8
        rightStack.alignment = .centerY
        for l in [stOk, stTerm, stEnc, stWatch, stShare, stSb] {
            l.font = .systemFont(ofSize: 11.5)
        }
        stShare.textColor = Theme.RGBA(0x6cb8ff).color
        leftStack.orientation = .horizontal
        leftStack.spacing = 14
        leftStack.alignment = .centerY
        [bar, barSep, ttIcon, ttLabel, rightStack, statusView, statusSep,
         leftStack, stSb, bodyHost, findHost, assistHost].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false }
        view.addSubview(bar)
        bar.addSubview(barSep); bar.addSubview(ttIcon); bar.addSubview(ttLabel)
        bar.addSubview(rightStack)
        view.addSubview(findHost); view.addSubview(bodyHost)
        view.addSubview(assistHost)
        view.addSubview(statusView)
        statusView.addSubview(statusSep); statusView.addSubview(leftStack)
        statusView.addSubview(stSb)
        [stOk, stTerm, stEnc, stWatch, stShare].forEach {
            leftStack.addArrangedSubview($0) }
        assistW = assistHost.widthAnchor.constraint(equalToConstant: 0)
        findH = findHost.heightAnchor.constraint(equalToConstant: 0)
        findH?.isActive = true
        NSLayoutConstraint.activate([
            bar.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            bar.topAnchor.constraint(equalTo: view.topAnchor),
            bar.heightAnchor.constraint(equalToConstant: 42),
            barSep.leadingAnchor.constraint(equalTo: bar.leadingAnchor),
            barSep.trailingAnchor.constraint(equalTo: bar.trailingAnchor),
            barSep.bottomAnchor.constraint(equalTo: bar.bottomAnchor),
            barSep.heightAnchor.constraint(equalToConstant: 1),
            ttIcon.leadingAnchor.constraint(equalTo: bar.leadingAnchor,
                                            constant: 14),
            ttIcon.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            ttIcon.widthAnchor.constraint(equalToConstant: 15),
            ttIcon.heightAnchor.constraint(equalToConstant: 15),
            ttLabel.leadingAnchor.constraint(
                equalTo: ttIcon.trailingAnchor, constant: 8),
            ttLabel.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            ttLabel.trailingAnchor.constraint(lessThanOrEqualTo:
                rightStack.leadingAnchor, constant: -8),
            rightStack.trailingAnchor.constraint(
                equalTo: bar.trailingAnchor, constant: -14),
            rightStack.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            findHost.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            findHost.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            findHost.topAnchor.constraint(equalTo: bar.bottomAnchor),
            statusView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            statusView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            statusView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            statusView.heightAnchor.constraint(equalToConstant: 25),
            statusSep.leadingAnchor.constraint(equalTo: statusView.leadingAnchor),
            statusSep.trailingAnchor.constraint(equalTo: statusView.trailingAnchor),
            statusSep.topAnchor.constraint(equalTo: statusView.topAnchor),
            statusSep.heightAnchor.constraint(equalToConstant: 1),
            leftStack.leadingAnchor.constraint(
                equalTo: statusView.leadingAnchor, constant: 14),
            leftStack.centerYAnchor.constraint(equalTo: statusView.centerYAnchor),
            leftStack.trailingAnchor.constraint(lessThanOrEqualTo:
                stSb.leadingAnchor, constant: -14),
            stSb.trailingAnchor.constraint(
                equalTo: statusView.trailingAnchor, constant: -14),
            stSb.centerYAnchor.constraint(equalTo: statusView.centerYAnchor),
            bodyHost.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            bodyHost.trailingAnchor.constraint(
                equalTo: assistHost.leadingAnchor),
            bodyHost.topAnchor.constraint(equalTo: findHost.bottomAnchor),
            bodyHost.bottomAnchor.constraint(equalTo: statusView.topAnchor),
            assistHost.trailingAnchor.constraint(
                equalTo: view.trailingAnchor),
            assistHost.topAnchor.constraint(equalTo: findHost.bottomAnchor),
            assistHost.bottomAnchor.constraint(equalTo: statusView.topAnchor),
        ])
        assistW?.isActive = true
        syncBarButtons()
    }

    /// 单叶 = termToolbar（监控/共享/search/重连）；
    /// 多叶 = workspaceView toolbar（Broadcast/助手栏/Focus）。
    private var barKey = ""
    private func syncBarButtons() {
        let dead = paneCount == 1 && focusedLeaf?.wsDead == true
        let key = "\(paneCount)-\(dead)"
        guard key != barKey else { return }
        barKey = key
        var btns: [TBtn] = paneCount == 1
            ? [watchBtn, shareBtn, searchBtn]
            : [bcastBtn, assistBtn, focusBtn]
        if dead { btns.append(reconnBtn) }
        rightStack.arrangedSubviews.forEach {
            rightStack.removeArrangedSubview($0); $0.removeFromSuperview() }
        btns.forEach { rightStack.addArrangedSubview($0) }
    }

    /// term-status 内容（焦点叶状态/监控/共享 + scrollback 设置）。
    private func updateStatus() {
        let leaf = focusedLeaf
        let dead = leaf?.wsDead ?? false
        stOk.stringValue = dead ? L("● 会话已结束") : L("● 已连接")
        stOk.textColor = (dead ? Theme.RGBA(0xe8c66a)
                          : chromeTheme.cursor).color
        stWatch.stringValue = String(
            format: L("监控: %@"),
            (leaf?.watchOn ?? false) ? L("EOF + OSC133 + AI 预设") : L("关"))
        let wTitle = (leaf?.watchOn ?? false) ? L("监控 开") : L("监控 关")
        if watchBtn.titleText != wTitle {
            watchBtn.setContent(wTitle, icon: "eye")
        }
        watchBtn.mode = (leaf?.watchOn ?? false) ? .on : .plain
        if let leaf, leaf.shareOn, let sid = leaf.shareId {
            stShare.isHidden = false
            stShare.stringValue = String(
                format: L("共享中 · %d 人"),
                ShareService.shared.participants(sid).count)
        } else {
            stShare.isHidden = true
        }
        let sb = Int(Vault.shared.setting("scrollback") ?? "10000") ?? 10000
        let f = NumberFormatter()
        f.numberStyle = .decimal
        f.groupingSeparator = ","
        stSb.stringValue = String(
            format: L("scrollback %@ 行"),
            f.string(from: NSNumber(value: sb)) ?? "\(sb)")
    }

    /// 终端主题 → chrome 全量取色（prototype .term-view 的 --t-* 变量）。
    func applyChrome(_ t: TermTheme) {
        chromeTheme = t
        view.layer?.backgroundColor = t.bg.color.cgColor
        bar.layer?.backgroundColor = t.bar.color.cgColor
        barSep.layer?.backgroundColor = t.line.color.cgColor
        ttLabel.textColor = t.title.color
        statusView.layer?.backgroundColor = t.bar.color.cgColor
        statusSep.layer?.backgroundColor = t.line.color.cgColor
        for l in [stTerm, stEnc, stWatch, stSb] { l.textColor = t.muted.color }
        for b in [watchBtn, shareBtn, searchBtn, reconnBtn,
                  bcastBtn, assistBtn, focusBtn] { b?.applyTheme(t) }
        for (id, h) in heads {
            h.layer?.backgroundColor = t.bar.color.cgColor
            guard let hb = headBtns[id] else { continue }
            hb.lbl.textColor = t.title.color
            hb.img.image = hbIcon(pages[id]?.wsIconName ?? "terminal",
                                  size: 12, tint: t.title.color)
            hb.watch.applyTheme(t); hb.share.applyTheme(t)
            hb.reconn.applyTheme(t); hb.close.applyTheme(t)
        }
        assistPanel?.applyTheme(t)
        updateTitle()
        updateStatus()
    }

    /// find bar 展开/收起（find 补丁调用）
    func setFindBar(_ v: NSView?) {
        findHost.subviews.forEach { $0.removeFromSuperview() }
        if let v {
            v.translatesAutoresizingMaskIntoConstraints = false
            findHost.addSubview(v)
            NSLayoutConstraint.activate([
                v.leadingAnchor.constraint(
                    equalTo: findHost.leadingAnchor),
                v.trailingAnchor.constraint(
                    equalTo: findHost.trailingAnchor),
                v.topAnchor.constraint(equalTo: findHost.topAnchor),
                v.bottomAnchor.constraint(equalTo: findHost.bottomAnchor),
            ])
        }
        findH?.constant = v == nil ? 0 : 32
    }

    // ---------------- 叶子管理 ----------------
    private func wireLeaf(_ leaf: TermTab) {
        let id = leaf.wsLeafId
        leaf.onChange = { [weak self] in self?.leafMeta(id) }
        if let t = leaf.term {
            t.onFocusRequest = { [weak self] in
                self?.leafById(id).map { self?.focus($0, grab: false) }
            }
        }
        applyMirror(leaf)
    }

    /// leaf 标题/状态点变化 → pane-head + tab 标题同步
    private func leafMeta(_ id: UUID) {
        if let hb = headBtns[id], let leaf = pages[id] {
            hb.lbl.stringValue = leaf.wsHeadTitle
            hb.reconn.isHidden = !leaf.wsDead
            hb.img.image = hbIcon(leaf.wsIconName, size: 12,
                                  tint: chromeTheme.title.color)
            hb.watch.mode = leaf.watchOn ? .on : .plain
            let bg = leaf.watchBadge
            hb.badge.stringValue =
                bg == "err" ? "×" : bg == "pending" ? "●"
                : bg == "ok" ? "✓" : ""
            hb.badge.textColor =
                bg == "err" ? Theme.red.color
                : bg == "pending" ? Theme.amber.color
                : Theme.green.color
            hb.share.mode = leaf.shareOn ? .share : .plain
        }
        updateTitle()
        updateStatus()
        syncBarButtons()
        syncShareState()
        tab?.syncFromWorkspace()
    }

    func leafById(_ id: UUID) -> WsModel.Node? { model.leaf(id) }
    var paneCount: Int { model.leafCount }
    var focusedLeaf: TermTab? {
        model.focused.leafId.flatMap { pages[$0] } }
    var leavesList: [TermTab] { model.leaves.compactMap {
        $0.leafId.flatMap { pages[$0] } } }
    var allTerms: [TerminalView] { leavesList.compactMap { $0.term } }
    var anyRunning: Bool { leavesList.contains { $0.running } }

    func focus(_ n: WsModel.Node, grab: Bool) {
        model.setFocus(n)
        applyBorders()
        if let id = n.leafId, let leaf = pages[id] {
            if grab { view.window?.makeFirstResponder(leaf.term) }
            leafMeta(id)
        }
        tab?.syncFromWorkspace()
    }

    /// 布局定稿：求解约束并应用 split frac —— 之后叶子 term
    /// 持有真实 frame，startSession() 读到有效 rows/cols。
    func layoutNow() {
        view.layoutSubtreeIfNeeded()
        applyFractions()
        treeView?.layoutSubtreeIfNeeded()
    }

    /// 分屏（对齐 hb_workspace_split_focused）：clone 焦点 pane 插入右侧/下侧。
    @discardableResult
    func splitFocused(horizontal: Bool) -> Bool {
        guard model.canSplit, let leaf = focusedLeaf,
              let clone = leaf.wsClone() else { return false }
        let newLeaf = WsModel.Node(leafId: clone.wsLeafId)
        guard model.split(target: model.focused,
                          newLeaf: newLeaf,
                          horizontal: horizontal) != nil else {
            clone.kill()
            return false
        }
        clone.wsLeaf = self
        pages[clone.wsLeafId] = clone
        wireLeaf(clone)
        rebuild()
        // 布局定稿（约束 + split frac）后再启 session：
        // 先在 0×0/默认 80 列下 spawn 会让登录横幅按假宽度折行。
        layoutNow()
        clone.startSession()
        focus(newLeaf, grab: true)
        return true
    }

    /// 添加外来 pane（Workspace 选择器）——追加到最后一叶的右侧。
    func addPane(_ leaf: TermTab, horizontal: Bool = true,
                 frac: Float = 0.5) {
        leaf.wsLeaf = self
        pages[leaf.wsLeafId] = leaf
        wireLeaf(leaf)
        if let last = model.leaves.last {
            let nl = WsModel.Node(leafId: leaf.wsLeafId)
            _ = model.split(target: last, newLeaf: nl,
                            horizontal: horizontal, frac: frac)
            rebuild()
            focus(nl, grab: true)
        }
    }

    /// 关闭指定叶（pane-head × / ⌘⇧W）：free 会话；单叶 → tab 关闭流程。
    @discardableResult
    func closeLeaf(_ id: UUID) -> Bool {
        guard model.leafCount > 1, let n = model.leaf(id) else {
            if let w = view.window as? MainWindow {
                w.closeTab(w.selTab)
            }
            return false
        }
        if focusMode { toggleFocusMode() }   // 先还原布局再摘
        pages[id]?.kill()
        pages[id] = nil
        boxes[id] = nil
        margins[id] = nil
        heads[id] = nil
        headH[id] = nil
        headBtns[id] = nil
        mCons[id] = nil
        _ = model.removeLeaf(n)
        rebuild()
        if let f = model.leaves.first { focus(f, grab: true) }
        return true
    }

    func closeFocused() {
        if let id = model.focused.leafId { _ = closeLeaf(id) }
    }

    func killAll() { leavesList.forEach { $0.kill() } }

    // ---------------- 广播 ----------------
    @objc func toggleBroadcast() {
        broadcast.toggle()
        applyBroadcast()
    }
    func setBroadcast(_ on: Bool) {
        if broadcast != on { toggleBroadcast() }
    }
    private func applyBroadcast() {
        leavesList.forEach { applyMirror($0) }
        applyBorders()
        bcastBtn.setContent(broadcast ? L("Broadcast 开") : L("Broadcast"),
                            icon: "broadcast")
        bcastBtn.mode = broadcast ? .on : .plain
    }
    /// 装/卸输入镜像：开 → 该 term 的输入转发给其它存活 pane。
    private func applyMirror(_ leaf: TermTab) {
        guard let t = leaf.term else { return }
        let src = leaf.wsLeafId
        t.inputMirror = broadcast ? { [weak self] ev in
            guard let self, self.broadcast else { return }
            for l in self.leavesList where l.wsLeafId != src {
                guard let s = l.session, s.alive else { continue }
                switch ev {
                case .key(let k, let m):
                    hb_session_key(s.sess, hb_key(rawValue: UInt32(k)), m)
                case .unichar(let u, let m):
                    hb_session_unichar(s.sess, u, m)
                case .paste(let str):
                    str.withCString { hb_session_paste(s.sess, $0) }
                case .raw(let d):
                    d.withUnsafeBytes { p in
                        hb_session_write(s.sess, p.baseAddress!
                            .assumingMemoryBound(to: CChar.self), d.count)
                    }
                }
            }
        } : nil
    }
    /// dev type/key 走广播语义（与用户键入一致）。
    func routeWrite(_ data: Data) {
        if broadcast {
            for l in leavesList where l.session?.alive == true {
                data.withUnsafeBytes { p in
                    hb_session_write(l.session!.sess, p.baseAddress!
                        .assumingMemoryBound(to: CChar.self), data.count)
                }
            }
        } else if let s = focusedLeaf?.session {
            data.withUnsafeBytes { p in
                hb_session_write(s.sess, p.baseAddress!
                    .assumingMemoryBound(to: CChar.self), data.count)
            }
        }
    }
    func routeKey(_ k: Int32, _ mods: Int32) {
        if broadcast {
            for l in leavesList where l.session?.alive == true {
                hb_session_key(l.session!.sess,
                               hb_key(rawValue: UInt32(k)), mods)
            }
        } else if let s = focusedLeaf?.session {
            hb_session_key(s.sess, hb_key(rawValue: UInt32(k)), mods)
        }
    }
    func routeUnichar(_ u: UInt32, _ mods: Int32) {
        if broadcast {
            for l in leavesList where l.session?.alive == true {
                hb_session_unichar(l.session!.sess, u, mods)
            }
        } else if let s = focusedLeaf?.session {
            hb_session_unichar(s.sess, u, mods)
        }
    }

    // ---------------- Focus 模式（⌘⇧M） ----------------
    @discardableResult
    private func pinBox(_ box: NSView, in mv: NSView,
                        inset: CGFloat) -> [NSLayoutConstraint] {
        box.translatesAutoresizingMaskIntoConstraints = false
        let cs = [
            box.leadingAnchor.constraint(equalTo: mv.leadingAnchor,
                                         constant: inset),
            box.trailingAnchor.constraint(equalTo: mv.trailingAnchor,
                                          constant: -inset),
            box.topAnchor.constraint(equalTo: mv.topAnchor,
                                     constant: inset),
            box.bottomAnchor.constraint(equalTo: mv.bottomAnchor,
                                        constant: -inset),
        ]
        NSLayoutConstraint.activate(cs)
        return cs
    }

    /// 助手栏（prototype assist 栏）：右侧 Snippets 快捷发送面板。
    @objc func toggleAssist() { setAssist(!assistOn) }
    func setAssist(_ on: Bool) {
        assistOn = on
        assistW?.constant = on ? 250 : 0
        assistBtn?.mode = on ? .on : .plain
        if on && assistPanel == nil {
            let p = AssistPanel()
            p.onSend = { [weak self] id in
                (self?.view.window as? MainWindow)?.sendSnippet(id: id)
            }
            p.onSearch = { [weak self] in self?.openFind() }
            p.applyTheme(chromeTheme)
            p.translatesAutoresizingMaskIntoConstraints = false
            assistHost.addSubview(p)
            NSLayoutConstraint.activate([
                p.leadingAnchor.constraint(equalTo: assistHost.leadingAnchor),
                p.trailingAnchor.constraint(equalTo: assistHost.trailingAnchor),
                p.topAnchor.constraint(equalTo: assistHost.topAnchor),
                p.bottomAnchor.constraint(equalTo: assistHost.bottomAnchor),
            ])
            assistPanel = p
        }
        assistPanel?.isHidden = !on   // NSView 不裁剪，0 宽时须隐藏
        assistPanel?.reload()
        view.layoutSubtreeIfNeeded()
    }

    @objc func toggleFocusMode() {
        if !focusMode && paneCount < 2 { return }
        focusMode.toggle()
        if focusMode {
            guard let fid = model.focused.leafId,
                  let box = boxes[fid], let mv = margins[fid] else {
                focusMode = false; return
            }
            // 记槽位（splitView + 子序号 + frac）→ 摘出 → 独占 bodyHost
            let pn = model.focused.parent
            let sv = pn.flatMap { splitOf[ObjectIdentifier($0)]?.sv }
            let idx = sv?.arrangedSubviews.firstIndex(of: mv) ?? 0
            fmSlot = pn.map { ($0, idx, $0.frac) }
            fmMargin = mv
            mv.removeFromSuperview()
            treeView?.isHidden = true
            bodyHost.addSubview(mv)
            fmCons = pinBox(mv, in: bodyHost, inset: 0)
            fmBox = box
        } else {
            guard fmBox != nil, let mv = fmMargin, let slot = fmSlot,
                  let sv = splitOf[ObjectIdentifier(slot.node)]?.sv
            else { focusMode = false; return }
            fmCons?.forEach { $0.isActive = false }
            fmCons = nil
            mv.removeFromSuperview()
            sv.insertArrangedSubview(mv, at:
                min(slot.index, sv.arrangedSubviews.count))
            slot.node.frac = slot.frac
            applyFrac(sv, node: slot.node)
            treeView?.isHidden = false
            fmBox = nil; fmMargin = nil; fmSlot = nil
        }
        focusBtn.mode = focusMode ? .on : .plain
        tab?.syncFromWorkspace()
    }

    // ---------------- 渲染 ----------------
    private func boxFor(_ n: WsModel.Node) -> NSView? {
        guard let id = n.leafId else { return nil }
        if let b = boxes[id] { return b }
        guard let leaf = pages[id] else { return nil }
        // pane：head + 内容（margin 6、radius 8、border 1.5，对齐 .pane）
        let box = NSView()
        box.wantsLayer = true
        box.layer?.cornerRadius = paneCount == 1 ? 0 : 8
        box.layer?.masksToBounds = true
        box.layer?.borderWidth = 1.5
        box.layer?.borderColor = NSColor.clear.cgColor
        let head = buildHead(id: id, leaf: leaf)
        let content = leaf.view
        [head, content].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false }
        box.addSubview(head); box.addSubview(content)
        let hh = head.heightAnchor.constraint(equalToConstant: 32)
        NSLayoutConstraint.activate([
            head.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            head.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            head.topAnchor.constraint(equalTo: box.topAnchor),
            hh,
            content.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            content.topAnchor.constraint(equalTo: head.bottomAnchor),
            content.bottomAnchor.constraint(equalTo: box.bottomAnchor),
        ])
        head.isHidden = paneCount == 1
        hh.constant = paneCount == 1 ? 0 : 32
        headH[id] = hh
        // margin 容器（splitView arranged 子级）
        let mv = NSView()
        mv.translatesAutoresizingMaskIntoConstraints = false
        mv.addSubview(box)
        mCons[id] = pinBox(box, in: mv, inset: paneCount == 1 ? 0 : 6)
        boxes[id] = box
        margins[id] = mv
        heads[id] = head
        box.setAccessibilityIdentifier("pane-\(id.uuidString.prefix(8))")
        return box
    }

    private func buildHead(id: UUID, leaf: TermTab) -> NSView {
        let h = NSView()
        h.wantsLayer = true
        let ic = NSImageView(image: hbIcon(leaf.wsIconName, size: 12,
                                           tint: chromeTheme.title.color))
        let lbl = NSTextField(labelWithString: leaf.wsHeadTitle)
        lbl.font = .systemFont(ofSize: 12)
        lbl.textColor = chromeTheme.title.color
        lbl.lineBreakMode = .byTruncatingTail
        func headBtn(_ icon: String, _ act: Selector, _ aid: String,
                     _ tip: String) -> TBtn {
            let b = TBtn("", icon: icon, sm: true,
                         target: self, action: act)
            b.toolTip = tip
            b.identifier = NSUserInterfaceItemIdentifier(
                "\(aid)-\(id.uuidString.prefix(8))")
            b.setAccessibilityIdentifier(aid)
            return b
        }
        let shr = headBtn("broadcast", #selector(onHeadShare(_:)),
                          "pane-share", L("共享"))
        let eye = headBtn("eye", #selector(onHeadWatch(_:)),
                          "pane-watch", L("监控设置"))
        let reconn = headBtn("refresh", #selector(onHeadReconn(_:)),
                             "pane-reconn", L("重新连接"))
        reconn.isHidden = !leaf.wsDead
        let x = headBtn("x", #selector(onHeadClose(_:)),
                        "pane-close", L("关闭分屏"))
        eye.mode = leaf.watchOn ? .on : .plain
        shr.mode = leaf.shareOn ? .share : .plain
        // watch 命中角标（对齐 hb_window_set_watch_dot）
        let badge = NSTextField(labelWithString: "")
        badge.font = .systemFont(ofSize: 11, weight: .bold)
        [ic, lbl, badge, shr, eye, reconn, x].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            h.addSubview($0)
        }
        NSLayoutConstraint.activate([
            ic.leadingAnchor.constraint(equalTo: h.leadingAnchor,
                                        constant: 10),
            ic.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            lbl.leadingAnchor.constraint(equalTo: ic.trailingAnchor,
                                         constant: 8),
            lbl.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            lbl.trailingAnchor.constraint(lessThanOrEqualTo:
                badge.leadingAnchor, constant: -4),
            badge.trailingAnchor.constraint(equalTo: shr.leadingAnchor,
                                            constant: -6),
            badge.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            x.trailingAnchor.constraint(equalTo: h.trailingAnchor,
                                        constant: -10),
            x.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            reconn.trailingAnchor.constraint(equalTo: x.leadingAnchor,
                                             constant: -6),
            reconn.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            eye.trailingAnchor.constraint(
                equalTo: reconn.leadingAnchor, constant: -6),
            eye.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            shr.trailingAnchor.constraint(
                equalTo: eye.leadingAnchor, constant: -6),
            shr.centerYAnchor.constraint(equalTo: h.centerYAnchor),
        ])
        // 点击 head 区聚焦该 pane
        h.identifier = NSUserInterfaceItemIdentifier(
            "panehead-\(id.uuidString.prefix(8))")
        h.addGestureRecognizer(NSClickGestureRecognizer(
            target: self, action: #selector(onPaneClick(_:))))
        headBtns[id] = (ic, lbl, reconn, eye, shr, badge, x)
        return h
    }

    @objc private func onPaneClick(_ g: NSClickGestureRecognizer) {
        guard let aid = g.view?.identifier?.rawValue,
              aid.hasPrefix("panehead-") else { return }
        let hex = String(aid.dropFirst(9))
        if let n = model.leaves.first(where: {
            $0.leafId?.uuidString.hasPrefix(hex) == true }) {
            focus(n, grab: true)
        }
    }
    private func leafOfBtn(_ b: NSButton) -> UUID? {
        guard let s = b.identifier?.rawValue,
              let dash = s.lastIndex(of: "-") else { return nil }
        let hex = String(s[s.index(after: dash)...])
        return model.leaves.first(where: {
            $0.leafId?.uuidString.hasPrefix(hex) == true })?.leafId
    }
    @objc private func onHeadWatch(_ b: NSButton) {
        leafOfBtn(b).flatMap { pages[$0] }?.openWatch()
    }
    @objc private func onHeadReconn(_ b: NSButton) {
        leafOfBtn(b).flatMap { pages[$0] }?.wsReconnect()
    }
    @objc private func onHeadClose(_ b: NSButton) {
        leafOfBtn(b).map { _ = closeLeaf($0) }
    }
    @objc private func onSearchBtn() { openFind() }
    @objc private func onShareBtn() {
        guard let leaf = focusedLeaf, let s = leaf.session
        else { return }
        ShareDialog.present(sess: s, title: leaf.shareTitle,
                            host: leaf.shareHost,
                            in: view.window as? MainWindow)
    }
    @objc private func onHeadShare(_ b: NSButton) {
        guard let id = leafOfBtn(b), let leaf = pages[id],
              let s = leaf.session else { return }
        ShareDialog.present(sess: s, title: leaf.shareTitle,
                            host: leaf.shareHost,
                            in: view.window as? MainWindow)
    }
    /// 叶共享态变化 → bar 按钮文案 + tab 蓝点（对齐 wire_refresh）。
    func syncShareState() {
        let on = focusedLeaf?.shareOn == true
        let title = on ? L("共享中") : L("共享")
        if shareBtn.titleText != title {
            shareBtn.setContent(title, icon: "broadcast")
        }
        shareBtn.mode = on ? .share : .plain
        tab?.shareOn = leavesList.contains { $0.shareOn }
        if let id = model.focused.leafId,
           let hb = headBtns[id] {
            hb.share.mode = pages[id]?.shareOn == true ? .share : .plain
        }
        updateStatus()
    }

    /// ⌘F：焦点 pane 的查找栏（findHost 承载，绑定打开时的焦点 term）。
    private lazy var findBar: FindBar = {
        let b = FindBar()
        b.onClose = { [weak self] in self?.closeFind() }
        return b
    }()
    private var findTerm: TerminalView?

    func openFind() {
        guard let t = focusedLeaf?.term else { return }
        findTerm?.findClear(); findTerm?.onFindClose = nil
        findTerm = t
        findBar.bind(t)
        setFindBar(findBar)
        findBar.open()
        t.onFindClose = { [weak self] in self?.closeFind() }
    }
    func closeFind() {
        findTerm?.findClear()
        findTerm?.onFindClose = nil
        findTerm = nil
        setFindBar(nil)
        if let f = focusedLeaf { view.window?.makeFirstResponder(f.term) }
    }
    /// dev hook：find 计数 n/总数
    var findCountText: String { findTerm == nil ? "" : findBar.countText }
    /// dev hook：设 find needle
    func devFindSet(_ s: String) {
        if findTerm == nil { openFind() }
        findBar.devSet(s)
    }
    /// dev hook：find 上/下一条
    func devFindStep(_ dir: Int) { findBar.devStep(dir) }
    /// dev hook：聚焦第 i 个 pane
    func devFocusPane(_ i: Int) {
        let ls = model.leaves
        guard i >= 0 && i < ls.count else { return }
        focus(ls[i], grab: true)
    }
    /// dev hook：工作区状态摘要（panes/focused/broadcast/focusMode）
    var devState: String {
        "panes=\(model.leafCount) focused=\(model.focused.leafId != nil) " +
        "broadcast=\(broadcast ? 1 : 0) focus=\(focusMode ? 1 : 0) " +
        "find=\(findTerm == nil ? "off" : findBar.countText)"
    }

    /// 方向移动焦点（⌘⌥方向，对齐 hb_workspace_move_focus 几何最近）。
    func moveFocus(dir: Int) {   // 0左 1右 2上 3下
        guard !focusMode, let cur = model.focused.leafId,
              let cbox = boxes[cur] else { return }
        let p0 = cbox.convert(
            NSPoint(x: cbox.bounds.midX, y: cbox.bounds.midY), to: view)
        var best: WsModel.Node?
        var bestD = CGFloat.greatestFiniteMagnitude
        for n in model.leaves where n.leafId != cur {
            guard let id = n.leafId, let b = boxes[id] else { continue }
            let p = b.convert(
                NSPoint(x: b.bounds.midX, y: b.bounds.midY), to: view)
            let dx = p.x - p0.x, dy = p.y - p0.y
            // 只取目标方向上的候选；mac 坐标 y 向上 → dir 2上=dy>0
            if (dir == 0 && dx >= -1) || (dir == 1 && dx <= 1) ||
               (dir == 2 && dy <= 1) || (dir == 3 && dy >= -1) { continue }
            let d = dir <= 1 ? abs(dx) + abs(dy) * 2
                             : abs(dy) + abs(dx) * 2
            if d < bestD { bestD = d; best = n }
        }
        if let best { focus(best, grab: true) }
    }

    private func render(_ n: WsModel.Node) -> NSView? {
        if n.isLeaf {
            guard let id = n.leafId else { return nil }
            _ = boxFor(n)                     // 确保 box+margin 已建
            return margins[id]
        }
        guard let a = n.a, let b = n.b,
              let va = render(a), let vb = render(b) else { return nil }
        let sv = WsSplitView()
        sv.isVertical = n.horizontal   // true → 左右排布（split right）
        sv.dividerStyle = .thin
        sv.delegate = self
        sv.addArrangedSubview(va); sv.addArrangedSubview(vb)
        splitOf[ObjectIdentifier(n)] = (sv, n)
        return sv
    }

    private func rebuild() {
        treeView?.removeFromSuperview()
        splitOf.removeAll()
        let tv = render(model.root)
        tv?.translatesAutoresizingMaskIntoConstraints = false
        if let tv {
            bodyHost.addSubview(tv)
            NSLayoutConstraint.activate([
                tv.leadingAnchor.constraint(equalTo: bodyHost.leadingAnchor),
                tv.trailingAnchor.constraint(
                    equalTo: bodyHost.trailingAnchor),
                tv.topAnchor.constraint(equalTo: bodyHost.topAnchor),
                tv.bottomAnchor.constraint(equalTo: bodyHost.bottomAnchor),
            ])
        }
        treeView = tv
        // head 仅多 pane 时显示（对齐 Linux：单页无 pane-head）；
        // 高度一并归零 —— 隐藏视图仍占布局高度，会露出空白条。
        let single = model.leafCount == 1
        for (id, h) in heads {
            h.isHidden = single
            headH[id]?.constant = single ? 0 : 32
            mCons[id]?.forEach { $0.constant = single ? 0 : 6 }
            boxes[id]?.layer?.cornerRadius = single ? 0 : 8
        }
        applyBorders()
        updateTitle()
        syncBarButtons()
        applyChrome(chromeTheme)
        // frac 要等 splitView 有 frame 后才能写
        DispatchQueue.main.async { [weak self] in self?.applyFractions() }
    }

    private func applyFractions() {
        for (_, v) in splitOf { applyFrac(v.sv, node: v.node) }
    }
    private func applyFrac(_ sv: NSSplitView, node: WsModel.Node) {
        guard sv.arrangedSubviews.count == 2 else { return }
        let dim = node.horizontal ? sv.bounds.width : sv.bounds.height
        guard dim > 1 else { return }
        sv.setPosition(dim * CGFloat(node.frac), ofDividerAt: 0)
    }

    // 拖动 → frac 回写模型
    func splitView(_ sv: NSSplitView,
                   didResizeSubviews subviews: [NSView]) {
        for (_, v) in splitOf where v.sv === sv {
            guard sv.arrangedSubviews.count == 2 else { continue }
            let dim = v.node.horizontal ? sv.bounds.width
                                        : sv.bounds.height
            let aDim = v.node.horizontal
                ? sv.arrangedSubviews[0].frame.width
                : sv.arrangedSubviews[0].frame.height
            if dim > 1 { v.node.frac = Float(aDim / dim) }
        }
    }

    private func applyBorders() {
        let single = model.leafCount == 1
        for (id, b) in boxes {
            b.layer?.hbBorderResolver = { [weak self] in
                guard let self, !single else { return nil }
                if self.broadcast {
                    return Theme.RGBA(0x2ea043).color.cgColor
                }
                return (self.model.focused.leafId == id
                    ? Theme.RGBA(0x1f8a4c).color.cgColor
                    : NSColor.clear.cgColor)
            }
        }
    }

    private func updateTitle() {
        if model.leafCount > 1 {
            ttLabel.stringValue = String(format: L("Workspace · %d 分屏"), model.leafCount)
            ttIcon.image = hbIcon("cols", size: 15,
                                  tint: chromeTheme.title.color)
        } else if let f = focusedLeaf {
            ttLabel.stringValue = f.wsBarTitle
            ttIcon.image = hbIcon(f.wsIconName, size: 15,
                                  tint: chromeTheme.title.color)
        } else {
            ttLabel.stringValue = L("Workspace")
        }
    }

    @objc private func onWatchBtn() { focusedLeaf?.openWatch() }
    @objc private func onReconnBtn() { focusedLeaf?.wsReconnect() }
}
