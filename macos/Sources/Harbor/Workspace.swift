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

final class Workspace: NSObject, NSSplitViewDelegate {
    weak var tab: TermTab?
    let model: WsModel
    /// leafId → 叶子页对象（TermTab/SSHTab）
    private(set) var pages: [UUID: TermTab] = [:]
    private var boxes: [UUID: NSView] = [:]        // leafId → pane box
    private var margins: [UUID: NSView] = [:]      // leafId → 5px 容器
    private var heads: [UUID: NSView] = [:]
    private var headBtns: [UUID: (img: NSImageView, lbl: NSTextField,
                                  reconn: NSButton)] = [:]
    /// 内节点 → 渲染出的 splitView（focus 还原 / frac 回收用）
    private var splitOf: [ObjectIdentifier: (sv: NSSplitView,
                                             node: WsModel.Node)] = [:]

    let view = NSView()
    private let bodyHost = NSView()
    private var treeView: NSView?
    private let ttLabel = NSTextField(labelWithString: "")
    private var bcastBtn: NSButton!
    private var focusBtn: NSButton!
    let findHost = NSView()          // find bar 宿主（⌘F，见 find 补丁）
    private var findH: NSLayoutConstraint?

    var broadcast = false
    var focusMode = false
    /// focus 模式下被独占的 pane box / 原 margin / 原槽位
    private var fmBox: NSView?
    private var fmMargin: NSView?
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
    }

    // ---------------- 工具栏（对齐 hb_workspace_init 的 bar） ----------------
    private func buildChrome() {
        view.wantsLayer = true
        view.layer?.backgroundColor = Theme.card.color.cgColor
        let bar = NSView()
        bar.wantsLayer = true
        bar.layer?.backgroundColor = Theme.subtle.color.cgColor
        let ic = NSImageView(image: hbIcon("cols", size: 15,
                                           tint: Theme.termDim.color))
        ttLabel.font = .systemFont(ofSize: 12.5)
        ttLabel.textColor = Theme.termDim.color
        let search = NSButton()
        search.bezelStyle = .texturedRounded
        search.isBordered = false
        search.image = hbIcon("search", size: 13, tint: Theme.termDim.color)
        search.target = self
        search.action = #selector(onSearchBtn)
        search.setAccessibilityIdentifier("ws-find")
        bcastBtn = NSButton(title: L("Broadcast"), target: self,
                            action: #selector(toggleBroadcast))
        bcastBtn.bezelStyle = .texturedRounded
        bcastBtn.font = .systemFont(ofSize: 11.5)
        bcastBtn.setAccessibilityIdentifier("ws-broadcast")
        let fb = NSButton(title: L("Focus"), target: self,
                          action: #selector(toggleFocusMode))
        fb.bezelStyle = .texturedRounded
        fb.font = .systemFont(ofSize: 11.5)
        fb.setAccessibilityIdentifier("ws-focus")
        focusBtn = fb
        [bar, ic, ttLabel, search, bcastBtn, focusBtn, bodyHost,
         findHost].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false }
        view.addSubview(bar)
        bar.addSubview(ic); bar.addSubview(ttLabel)
        bar.addSubview(search); bar.addSubview(bcastBtn)
        bar.addSubview(focusBtn)
        view.addSubview(findHost); view.addSubview(bodyHost)
        findH = findHost.heightAnchor.constraint(equalToConstant: 0)
        findH?.isActive = true
        NSLayoutConstraint.activate([
            bar.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            bar.topAnchor.constraint(equalTo: view.topAnchor),
            bar.heightAnchor.constraint(equalToConstant: 36),
            ic.leadingAnchor.constraint(equalTo: bar.leadingAnchor,
                                        constant: 14),
            ic.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            ttLabel.leadingAnchor.constraint(
                equalTo: ic.trailingAnchor, constant: 7),
            ttLabel.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            focusBtn.trailingAnchor.constraint(
                equalTo: bar.trailingAnchor, constant: -10),
            focusBtn.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            bcastBtn.trailingAnchor.constraint(
                equalTo: focusBtn.leadingAnchor, constant: -6),
            bcastBtn.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            search.trailingAnchor.constraint(
                equalTo: bcastBtn.leadingAnchor, constant: -6),
            search.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            search.widthAnchor.constraint(equalToConstant: 28),
            findHost.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            findHost.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            findHost.topAnchor.constraint(equalTo: bar.bottomAnchor),
            bodyHost.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            bodyHost.trailingAnchor.constraint(
                equalTo: view.trailingAnchor),
            bodyHost.topAnchor.constraint(equalTo: findHost.bottomAnchor),
            bodyHost.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
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
            hb.lbl.stringValue = leaf.wsTitle
            hb.reconn.isHidden = !leaf.wsDead
            hb.img.image = hbIcon(leaf.wsIconName, size: 12,
                                  tint: Theme.termDim.color)
        }
        updateTitle()
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
        headBtns[id] = nil
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
        bcastBtn.title = broadcast ? L("Broadcast 开") : L("Broadcast")
        bcastBtn.contentTintColor = broadcast ? Theme.green.color : nil
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
    private func pinBox(_ box: NSView, in mv: NSView, inset: CGFloat) {
        box.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            box.leadingAnchor.constraint(equalTo: mv.leadingAnchor,
                                         constant: inset),
            box.trailingAnchor.constraint(equalTo: mv.trailingAnchor,
                                          constant: -inset),
            box.topAnchor.constraint(equalTo: mv.topAnchor,
                                     constant: inset),
            box.bottomAnchor.constraint(equalTo: mv.bottomAnchor,
                                        constant: -inset),
        ])
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
            box.removeFromSuperview()
            treeView?.isHidden = true
            bodyHost.addSubview(box)
            pinBox(box, in: bodyHost, inset: 5)
            fmBox = box
        } else {
            guard let box = fmBox, let mv = fmMargin, let slot = fmSlot,
                  let sv = splitOf[ObjectIdentifier(slot.node)]?.sv
            else { focusMode = false; return }
            box.removeFromSuperview()
            mv.addSubview(box)
            pinBox(box, in: mv, inset: 5)
            sv.insertArrangedSubview(mv, at:
                min(slot.index, sv.arrangedSubviews.count))
            slot.node.frac = slot.frac
            applyFrac(sv, node: slot.node)
            treeView?.isHidden = false
            fmBox = nil; fmMargin = nil; fmSlot = nil
        }
        focusBtn.contentTintColor = focusMode ? Theme.blue.color : nil
        tab?.syncFromWorkspace()
    }

    // ---------------- 渲染 ----------------
    private func boxFor(_ n: WsModel.Node) -> NSView? {
        guard let id = n.leafId else { return nil }
        if let b = boxes[id] { return b }
        guard let leaf = pages[id] else { return nil }
        // pane：head + 内容（margin 5、radius 9、border 1.5，对齐 .pane）
        let box = NSView()
        box.wantsLayer = true
        box.layer?.cornerRadius = 9
        box.layer?.borderWidth = 1.5
        box.layer?.borderColor = NSColor.clear.cgColor
        let head = buildHead(id: id, leaf: leaf)
        let content = leaf.view
        [head, content].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false }
        box.addSubview(head); box.addSubview(content)
        NSLayoutConstraint.activate([
            head.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            head.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            head.topAnchor.constraint(equalTo: box.topAnchor),
            head.heightAnchor.constraint(equalToConstant: 27),
            content.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            content.topAnchor.constraint(equalTo: head.bottomAnchor),
            content.bottomAnchor.constraint(equalTo: box.bottomAnchor),
        ])
        head.isHidden = paneCount == 1
        // margin 容器（splitView arranged 子级）
        let mv = NSView()
        mv.translatesAutoresizingMaskIntoConstraints = false
        mv.addSubview(box)
        pinBox(box, in: mv, inset: 5)
        boxes[id] = box
        margins[id] = mv
        heads[id] = head
        box.setAccessibilityIdentifier("pane-\(id.uuidString.prefix(8))")
        return box
    }

    private func buildHead(id: UUID, leaf: TermTab) -> NSView {
        let h = NSView()
        h.wantsLayer = true
        h.layer?.backgroundColor = Theme.subtle.color.cgColor
        let ic = NSImageView(image: hbIcon(leaf.wsIconName, size: 12,
                                           tint: Theme.termDim.color))
        let lbl = NSTextField(labelWithString: leaf.wsTitle)
        lbl.font = .systemFont(ofSize: 11.5)
        lbl.textColor = Theme.termDim.color
        lbl.lineBreakMode = .byTruncatingTail
        func headBtn(_ icon: String, _ act: Selector, _ aid: String,
                     _ tip: String) -> NSButton {
            let b = NSButton()
            b.bezelStyle = .texturedRounded
            b.isBordered = false
            b.image = hbIcon(icon, size: 11, tint: Theme.termDim.color)
            b.target = self; b.action = act
            b.toolTip = tip
            b.identifier = NSUserInterfaceItemIdentifier(
                "\(aid)-\(id.uuidString.prefix(8))")
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(equalToConstant: 22).isActive = true
            return b
        }
        let reconn = headBtn("refresh", #selector(onHeadReconn(_:)),
                             "pane-reconn", L("重新连接"))
        reconn.isHidden = !leaf.wsDead
        let x = headBtn("x", #selector(onHeadClose(_:)),
                        "pane-close", L("关闭分屏"))
        [ic, lbl, reconn, x].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            h.addSubview($0)
        }
        NSLayoutConstraint.activate([
            ic.leadingAnchor.constraint(equalTo: h.leadingAnchor,
                                        constant: 10),
            ic.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            lbl.leadingAnchor.constraint(equalTo: ic.trailingAnchor,
                                         constant: 6),
            lbl.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            lbl.trailingAnchor.constraint(lessThanOrEqualTo:
                x.leadingAnchor, constant: -4),
            x.trailingAnchor.constraint(equalTo: h.trailingAnchor,
                                        constant: -6),
            x.centerYAnchor.constraint(equalTo: h.centerYAnchor),
            reconn.trailingAnchor.constraint(equalTo: x.leadingAnchor,
                                             constant: -2),
            reconn.centerYAnchor.constraint(equalTo: h.centerYAnchor),
        ])
        // 点击 head 区聚焦该 pane
        h.identifier = NSUserInterfaceItemIdentifier(
            "panehead-\(id.uuidString.prefix(8))")
        h.addGestureRecognizer(NSClickGestureRecognizer(
            target: self, action: #selector(onPaneClick(_:))))
        headBtns[id] = (ic, lbl, reconn)
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
    @objc private func onHeadReconn(_ b: NSButton) {
        leafOfBtn(b).flatMap { pages[$0] }?.wsReconnect()
    }
    @objc private func onHeadClose(_ b: NSButton) {
        leafOfBtn(b).map { _ = closeLeaf($0) }
    }
    @objc private func onSearchBtn() { openFind() }

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
        // head 仅多 pane 时显示（对齐 Linux：单页无 pane-head）
        for (_, h) in heads { h.isHidden = model.leafCount == 1 }
        applyBorders()
        updateTitle()
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
        for (id, b) in boxes {
            let focused = model.focused.leafId == id
            if broadcast {
                b.layer?.borderColor =
                    Theme.green.color.cgColor            // 广播：全 pane 绿框
            } else {
                b.layer?.borderColor = focused
                    ? Theme.blue.color.cgColor           // 焦点：蓝框
                    : NSColor.clear.cgColor
            }
        }
    }

    private func updateTitle() {
        if model.leafCount > 1 {
            ttLabel.stringValue = L("Workspace · \(model.leafCount) 分屏")
        } else {
            ttLabel.stringValue = focusedLeaf?.wsTitle ?? L("Workspace")
        }
    }
}
