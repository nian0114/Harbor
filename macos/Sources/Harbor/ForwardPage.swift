import AppKit
import CHarbor
import HarborKit

/// FR-06 Vault「端口转发」页 —— 对齐 linux/src/hb-forward-view.c：
/// 工具栏（新建分裂键 + ▾ 菜单 + 搜索），行 = 类型 tag | 监听 |
/// 目标 | 经由 | 流量 | 状态 pill | 开关/编辑/删除；双击编辑，
/// 删除确认 + 9s 撤销。mgr "changed" 单行刷新（STATS ≤1/s 不重建）。
/// documentView 翻转坐标：内容少于视口时贴顶（否则 NSStackView 沉底）。
private final class FlippedStack: NSStackView {
    override var isFlipped: Bool { true }
}

final class ForwardPage: NSView {
    private var objs: [OpaquePointer] = []        // "forward" 对象（owned）
    private var filtered: [OpaquePointer]?
    private var rowsBox: NSStackView!
    private var rows: [FwdRow] = []
    private var search: NSSearchField!
    private var emptyL: NSTextField!
    private var vaultObs: NSObjectProtocol?
    private var tick: Timer?
    private let mgr = ForwardMgr.shared

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.hbBg = "page"
        buildBar()
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.frame = NSRect(x: 0, y: 0, width: f.width,
                              height: f.height - 46)
        scroll.autoresizingMask = [.width, .height]
        rowsBox = FlippedStack()
        rowsBox.orientation = .vertical
        rowsBox.spacing = 0
        rowsBox.alignment = .leading
        rowsBox.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = rowsBox
        rowsBox.widthAnchor.constraint(
            equalTo: scroll.contentView.widthAnchor).isActive = true
        addSubview(scroll)
        rowsBox.addArrangedSubview(makeHeader())
        emptyL = NSTextField(labelWithString: "")
        emptyL.textColor = .secondaryLabelColor
        emptyL.font = .systemFont(ofSize: 12.5)
        emptyL.alignment = .center
        addSubview(emptyL)
        emptyL.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            emptyL.centerXAnchor.constraint(equalTo: centerXAnchor),
            emptyL.topAnchor.constraint(equalTo: topAnchor,
                                        constant: 120),
        ])
        mgr.onChange = { [weak self] id in self?.onMgrChange(id) }
        vaultObs = NotificationCenter.default.addObserver(
            forName: .hbVaultChanged, object: nil, queue: .main)
        { [weak self] n in
            let t = n.userInfo?["type"] as? String
            if t == "forward" || t == "host" { self?.fillRows() }
        }
        fillRows()
    }
    required init?(coder: NSCoder) { fatalError() }
    deinit {
        mgr.onChange = nil
        if let o = vaultObs {
            NotificationCenter.default.removeObserver(o)
        }
        freeObjs()
        tick?.invalidate()
    }
    private func freeObjs() {
        objs.forEach { hb_obj_free($0) }
        objs = []
    }

    // ---------- 工具栏 ----------
    private func buildBar() {
        var x: CGFloat = 14
        let newB = NSButton(title: L("＋ 新建转发"), target: self,
                            action: #selector(onNew))
        newB.bezelStyle = .rounded
        newB.font = .systemFont(ofSize: 12)
        newB.frame = NSRect(x: x, y: bounds.height - 38,
                            width: max(30,
                                newB.intrinsicContentSize.width + 14),
                            height: 26)
        newB.autoresizingMask = [.minYMargin]
        newB.setAccessibilityIdentifier("fwd-new")
        addSubview(newB)
        x += newB.frame.width
        let dd = NSPopUpButton(frame: NSRect(x: x, y: bounds.height - 38,
                                             width: 30, height: 26),
                               pullsDown: true)
        dd.bezelStyle = .rounded
        dd.autoresizingMask = [.minYMargin]
        let menu = NSMenu()
        for (t, a) in [
            (L("New forwarding"), #selector(onNew)),
            (L("全部启动"), #selector(onStartAll)),
            (L("全部停止"), #selector(onStopAll)),
            (L("导出规则 JSON"), #selector(onExport)),
        ] as [(String, Selector)] {
            let mi = NSMenuItem(title: t, action: a,
                                keyEquivalent: "")
            mi.target = self
            menu.addItem(mi)
        }
        dd.menu = menu
        dd.setAccessibilityIdentifier("fwd-menu")
        addSubview(dd)
        x += 38
        search = NSSearchField(frame: NSRect(
            x: bounds.width - 250, y: bounds.height - 38,
            width: 236, height: 26))
        search.placeholderString = L("搜索监听 / 目标 / 主机…")
        search.font = .systemFont(ofSize: 12)
        search.autoresizingMask = [.minXMargin, .minYMargin]
        search.target = self
        search.action = #selector(onSearch)
        search.setAccessibilityIdentifier("fwd-search")
        addSubview(search)
    }
    private func makeHeader() -> NSView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.spacing = 8
        row.edgeInsets = NSEdgeInsets(top: 6, left: 14,
                                      bottom: 6, right: 10)
        for (t, w) in [(L("类型"), 60.0), (L("监听"), -1.0),
                       (L("目标"), -1.0), (L("经由 SSH 主机"), 140.0),
                       (L("流量"), 170.0), (L("状态"), 110.0),
                       ("", 96.0)] {
            let l = NSTextField(labelWithString: t)
            l.font = .systemFont(ofSize: 11)
            l.textColor = .secondaryLabelColor
            if w > 0 {
                l.widthAnchor.constraint(
                    equalToConstant: w).isActive = true
            } else {
                l.widthAnchor.constraint(
                    greaterThanOrEqualToConstant: 110).isActive = true
                l.setContentHuggingPriority(.init(1),
                                            for: .horizontal)
            }
            row.addArrangedSubview(l)
        }
        let line = NSView()
        line.wantsLayer = true
        line.layer?.hbBg = "line"
        let wrap = NSStackView(views: [row, line])
        wrap.orientation = .vertical
        wrap.spacing = 0
        line.heightAnchor.constraint(equalToConstant: 1).isActive = true
        line.widthAnchor.constraint(
            equalTo: wrap.widthAnchor).isActive = true
        return wrap
    }

    // ---------- 行 ----------
    private final class FwdRow: NSObject {
        let id: String
        weak var page: ForwardPage?
        let view = NSStackView()
        let tag = NSTextField(labelWithString: "")
        let bind = NSTextField(labelWithString: "")
        let dest = NSTextField(labelWithString: "")
        let via = NSTextField(labelWithString: "")
        let traffic = NSTextField(labelWithString: "—")
        let pill = NSTextField(labelWithString: "")
        let sw = NSSwitch()
        var swHold = false
        init(id: String, page: ForwardPage) {
            self.id = id; self.page = page
        }
    }

    private func buildRow(_ o: OpaquePointer) -> FwdRow {
        let id = hb_obj_id(o).str
        let r = FwdRow(id: id, page: self)
        let ks = hb_obj_get_str(o, "kind").str
        let kind = FwdKit.Kind(rawValue: ks) ?? .local
        r.view.orientation = .horizontal
        r.view.spacing = 8
        r.view.edgeInsets = NSEdgeInsets(top: 5, left: 14,
                                       bottom: 5, right: 10)
        r.view.heightAnchor.constraint(
            greaterThanOrEqualToConstant: 30).isActive = true
        r.tag.stringValue = kind.tag
        r.tag.font = .systemFont(ofSize: 11)
        r.tag.widthAnchor.constraint(equalToConstant: 60).isActive = true
        for (l, w) in [(r.bind, -1.0), (r.dest, -1.0),
                       (r.via, 140.0), (r.traffic, 170.0)] {
            l.lineBreakMode = .byTruncatingTail
            if w > 0 {
                l.widthAnchor.constraint(
                    equalToConstant: w).isActive = true
            } else {
                l.widthAnchor.constraint(
                    greaterThanOrEqualToConstant: 110).isActive = true
                l.setContentHuggingPriority(.init(1),
                                            for: .horizontal)
            }
        }
        r.bind.stringValue = FwdKit.bindText(kind: kind,
            addr: hb_obj_get_str(o, "bind_addr").str,
            port: Int(hb_obj_get_int(o, "bind_port", 0)))
        r.bind.font = .monospacedSystemFont(ofSize: 11.5,
                                            weight: .regular)
        r.dest.stringValue = FwdKit.destText(kind: kind,
            addr: hb_obj_get_str(o, "dest_addr").str,
            port: Int(hb_obj_get_int(o, "dest_port", 0)))
        r.dest.font = .monospacedSystemFont(ofSize: 11.5,
                                            weight: .regular)
        r.via.stringValue = viaText(hb_obj_get_str(o, "via").str)
        r.via.lineBreakMode = .byTruncatingTail
        // 状态 pill：圆角着色标签（同 logs 行徽标）
        r.pill.alignment = .center
        r.pill.font = .systemFont(ofSize: 11)
        r.pill.wantsLayer = true
        r.pill.layer?.cornerRadius = 8
        r.pill.widthAnchor.constraint(equalToConstant: 110)
            .isActive = true
        r.view.addArrangedSubview(r.tag)
        r.view.addArrangedSubview(r.bind)
        r.view.addArrangedSubview(r.dest)
        r.view.addArrangedSubview(r.via)
        r.view.addArrangedSubview(r.traffic)
        r.traffic.font = .monospacedSystemFont(ofSize: 11,
                                               weight: .regular)
        r.view.addArrangedSubview(r.pill)
        r.sw.controlSize = .small
        r.sw.target = self
        r.sw.action = #selector(onSw(_:))
        r.sw.setAccessibilityIdentifier("fwd-sw-\(id)")
        r.view.addArrangedSubview(r.sw)
        let eb = NSButton(title: "✎", target: self,
                          action: #selector(onEdit(_:)))
        eb.bezelStyle = .inline
        eb.font = .systemFont(ofSize: 12)
        eb.toolTip = L("编辑")
        eb.identifier = .init(id)
        eb.setAccessibilityIdentifier("fwd-edit-\(id)")
        r.view.addArrangedSubview(eb)
        let db = NSButton(title: "🗑", target: self,
                          action: #selector(onDel(_:)))
        db.bezelStyle = .inline
        db.font = .systemFont(ofSize: 12)
        db.toolTip = L("删除规则")
        db.identifier = .init(id)
        db.setAccessibilityIdentifier("fwd-del-\(id)")
        r.view.addArrangedSubview(db)
        // 双击编辑：行空白区手势
        let g = NSClickGestureRecognizer(target: self,
                                         action: #selector(onDbl(_:)))
        g.numberOfClicksRequired = 2
        g.delaysPrimaryMouseButtonEvents = false
        r.view.addGestureRecognizer(g)
        return r
    }

    private func viaText(_ via: String) -> String {
        if via.isEmpty { return L("（未设置）") }
        if let h = Vault.shared.get(via), !hb_obj_tombstone(h) {
            let nm = hb_obj_get_str(h, "name").str
            hb_obj_free(h)
            return nm.isEmpty ? via : nm
        }
        return L("（已删除）")
    }

    // ---------- 刷新 ----------
    private func setPill(_ r: FwdRow, _ t: String,
                         _ fg: NSColor, _ bg: NSColor) {
        r.pill.stringValue = t
        r.pill.textColor = fg
        r.pill.layer?.backgroundColor = bg.cgColor
    }
    private func updateRow(_ r: FwdRow) {
        let st = mgr.status(r.id)
        if let o = objs.first(where: { hb_obj_id($0).str == r.id }),
           let kind = FwdKit.Kind(rawValue:
                hb_obj_get_str(o, "kind").str) {
            r.bind.stringValue = FwdKit.bindText(kind: kind,
                addr: hb_obj_get_str(o, "bind_addr").str,
                port: Int(hb_obj_get_int(o, "bind_port", 0)),
                actual: st.boundPort)
        }
        r.traffic.stringValue = FwdKit.trafficText(
            running: st.state != .stopped, up: st.up, down: st.down,
            active: st.active)
        switch st.state {
        case .listening:
            setPill(r, L("监听中"),
                    Theme.greenPillFg.color, Theme.greenPillBg.color)
            r.view.toolTip = st.message
        case .connecting:
            setPill(r, L("连接中…"), Theme.pillRunFg.color,
                    Theme.pillRunBg.color)
            r.view.toolTip = nil
        case .reconnecting:
            setPill(r, L("断开 · %ds 后重连").fmt(st.retryInS),
                    Theme.pillWaitFg.color, Theme.pillWaitBg.color)
            r.view.toolTip = st.message ?? L("连接断开，自动重连中")
            ensureTick()
        case .failed:
            setPill(r, L("失败"), Theme.pillErrFg.color,
                    Theme.pillErrBg.color)
            r.view.toolTip = st.message ?? L("启动失败")
        case .stopped:
            setPill(r, L("已停止"), Theme.termDim.color,
                    Theme.line.color.withAlphaComponent(0.35))
            r.view.toolTip = st.message
        }
        r.swHold = true
        r.sw.state = st.running ? .on : .off
        r.swHold = false
    }
    /// 1s 倒计时 tick：只刷 RECONNECTING 行 pill 文本（linux tick_cb）。
    private func ensureTick() {
        if tick != nil { return }
        tick = Timer.scheduledTimer(withTimeInterval: 1,
                                    repeats: true) { [weak self] t in
            guard let self else { t.invalidate(); return }
            var any = false
            for r in self.rows {
                let st = self.mgr.status(r.id)
                if st.state == .reconnecting {
                    r.pill.stringValue =
                        L("断开 · %ds 后重连").fmt(st.retryInS)
                    any = true
                }
            }
            if !any { t.invalidate(); self.tick = nil }
        }
    }
    private func onMgrChange(_ id: String?) {
        if let id {
            if let r = rows.first(where: { $0.id == id }) {
                updateRow(r)
            }
        } else {
            rows.forEach { updateRow($0) }
        }
        if rows.contains(where: {
            mgr.status($0.id).state == .reconnecting }) {
            ensureTick()
        }
    }
    private func match(_ o: OpaquePointer, _ q: String) -> Bool {
        if q.isEmpty { return true }
        let ks = hb_obj_get_str(o, "kind").str
        let kind = FwdKit.Kind(rawValue: ks) ?? .local
        let s = "\(ks) \(kind.tag) " +
            FwdKit.bindText(kind: kind,
                addr: hb_obj_get_str(o, "bind_addr").str,
                port: Int(hb_obj_get_int(o, "bind_port", 0))) + " " +
            FwdKit.destText(kind: kind,
                addr: hb_obj_get_str(o, "dest_addr").str,
                port: Int(hb_obj_get_int(o, "dest_port", 0))) + " " +
            viaText(hb_obj_get_str(o, "via").str)
        return s.lowercased().contains(q)
    }
    private func fillRows() {
        freeObjs()
        objs = Vault.shared.list("forward")
        objs.sort { hb_obj_get_int($0, "order", 0) <
                    hb_obj_get_int($1, "order", 0) }
        let q = search?.stringValue.trimmingCharacters(
            in: .whitespaces).lowercased() ?? ""
        filtered = objs.filter { match($0, q) }
        rowsBox.arrangedSubviews.dropFirst().forEach {
            rowsBox.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }
        rows.removeAll()
        for o in filtered ?? [] {
            let r = buildRow(o)
            rows.append(r)
            rowsBox.addArrangedSubview(r.view)
            updateRow(r)
        }
        emptyL.stringValue = objs.isEmpty
            ? L("还没有转发规则。点击「＋ 新建转发」创建第一条。")
            : L("无匹配规则")
        emptyL.isHidden = !(filtered?.isEmpty ?? true)
    }
    @objc private func onSearch() { fillRows() }

    // ---------- 动作 ----------
    @objc private func onSw(_ sw: NSSwitch) {
        guard let r = rows.first(where: { $0.sw === sw }),
              !r.swHold else { return }
        if sw.state == .on { mgr.start(r.id) } else { mgr.stop(r.id) }
    }
    @objc private func onDbl(_ g: NSClickGestureRecognizer) {
        guard let v = g.view as? NSStackView,
              let r = rows.first(where: { $0.view === v }),
              let o = objs.first(where: { hb_obj_id($0).str == r.id })
        else { return }
        openForm(obj: o)
    }
    @objc private func onEdit(_ b: NSButton) {
        guard let id = b.identifier?.rawValue,
              let o = objs.first(where: { hb_obj_id($0).str == id })
        else { return }
        openForm(obj: o)
    }
    @objc private func onDel(_ b: NSButton) {
        guard let id = b.identifier?.rawValue,
              let o = objs.first(where: { hb_obj_id($0).str == id })
        else { return }
        let kind = FwdKit.Kind(rawValue:
            hb_obj_get_str(o, "kind").str) ?? .local
        let desc = FwdKit.ruleDesc(kind: kind,
            bindAddr: hb_obj_get_str(o, "bind_addr").str,
            bindPort: Int(hb_obj_get_int(o, "bind_port", 0)),
            destAddr: hb_obj_get_str(o, "dest_addr").str,
            destPort: Int(hb_obj_get_int(o, "dest_port", 0)))
        let wasEnabled = hb_obj_get_bool(o, "enabled", false)
        let a = NSAlert()
        a.messageText = L("删除转发规则")
        a.informativeText = L("确定删除 %@？" +
            "监听将立即停止。删除后 9 秒内可撤销。").fmt(desc)
        a.addButton(withTitle: L("删除"))
        a.addButton(withTitle: L("取消"))
        a.buttons.first?.hasDestructiveAction = true
        let w = window
        a.beginSheetModal(for: w!) { [weak self] resp in
            guard resp == .alertFirstButtonReturn, let self
            else { return }
            self.mgr.stop(id)
            Vault.shared.delete(id)
            let t = SftpToast(text: L("已删除规则 %@").fmt(desc),
                action: L("撤销"), ttl: 9, onAction: { [weak self] in
                    Vault.shared.undelete(id)
                    self?.fillRows()
                    if wasEnabled { self?.mgr.start(id) }
                })
            self.showToast(t)
        }
    }
    private func showToast(_ t: SftpToast) {
        t.translatesAutoresizingMaskIntoConstraints = false
        addSubview(t)
        NSLayoutConstraint.activate([
            t.centerXAnchor.constraint(equalTo: centerXAnchor),
            t.bottomAnchor.constraint(equalTo: bottomAnchor,
                                      constant: -14),
        ])
    }
    @objc private func onNew() { openForm(obj: nil) }
    @objc private func onStartAll() {
        for o in objs { mgr.start(hb_obj_id(o).str) }
    }
    @objc private func onStopAll() {
        for o in objs { mgr.stop(hb_obj_id(o).str) }
    }
    @objc private func onExport() {
        guard let v = Vault.shared.vault else { return }
        var out: UnsafeMutablePointer<CChar>?
        guard hb_vault_export_json(v, "forward", &out) == HB_OK,
              let out else { return }
        let json = String(cString: out)
        free(out)
        let sp = NSSavePanel()
        sp.title = L("导出转发规则")
        sp.nameFieldStringValue = "harbor-forwards.json"
        sp.begin { [weak self] resp in
            guard resp == .OK, let url = sp.url else { return }
            do {
                try json.write(to: url, atomically: true,
                               encoding: .utf8)
                self?.showToast(SftpToast(
                    text: L("转发规则已导出")))
            } catch {
                self?.showToast(SftpToast(text: L("导出失败")))
            }
        }
    }
    private func openForm(obj: OpaquePointer?) {
        guard let w = window else { return }
        let sheet = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 470),
            styleMask: [.titled], backing: .buffered, defer: false)
        sheet.title = obj == nil ? L("New forwarding") : L("Edit forwarding")
        sheet.isReleasedWhenClosed = false
        let form = ForwardForm(frame: sheet.contentView!.bounds)
        if let obj { form.fill(obj) }
        form.onDone = { start, out in
            if let out {
                Vault.shared.put(out)
                if start {
                    ForwardMgr.shared.start(hb_obj_id(out).str)
                }
                hb_obj_free(out)
            }
            w.endSheet(sheet)
        }
        sheet.contentView = form
        w.beginSheet(sheet) { _ in }
    }

    // ---------- dev ----------
    var devState: String {
        var s = "rules=\(objs.count) shown=\(filtered?.count ?? 0)\n"
        for o in filtered ?? objs {
            let id = hb_obj_id(o).str
            let st = mgr.status(id)
            s += "\(id): \(hb_obj_get_str(o, "kind").str) " +
                 "bind=\(hb_obj_get_str(o, "bind_addr").str):" +
                 "\(hb_obj_get_int(o, "bind_port", 0)) " +
                 "dest=\(hb_obj_get_str(o, "dest_addr").str):" +
                 "\(hb_obj_get_int(o, "dest_port", 0)) " +
                 "via=\(hb_obj_get_str(o, "via").str) " +
                 "en=\(hb_obj_get_bool(o, "enabled", false) ? 1 : 0) " +
                 "auto=\(hb_obj_get_bool(o, "autostart", false) ? 1 : 0) " +
                 "state=\(st.state) port=\(st.boundPort) " +
                 "up=\(st.up) down=\(st.down) act=\(st.active) " +
                 "retry=\(st.retryInS) " +
                 "msg=\(st.message ?? "-")\n"
        }
        return s
    }
    func devSwitch(_ id: String, _ on: Bool) {
        if on { mgr.start(id) } else { mgr.stop(id) }
    }
    func devOpenForm(_ id: String?) {
        if let id,
           let o = objs.first(where: { hb_obj_id($0).str == id }) {
            openForm(obj: o)
        } else { openForm(obj: nil) }
    }
}
