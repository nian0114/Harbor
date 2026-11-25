import AppKit
import CHarbor
import HarborKit

/// FR-08 Vault「命令片段」页 —— 对齐 prototype snippetsView：
/// toolbar（New snippet 分裂键 + Shell History ghost + 搜索），
/// .list-plain 卡片列表（sc-title + scope tag + 行内编辑/删除，
/// sc-code 灰底 mono 块，整卡点击发送）；History 为真实功能子栈。
final class SnippetsPage: NSView, NSTableViewDataSource,
                          NSTableViewDelegate {
    private enum Mode { case snips, hist }
    private var mode: Mode = .snips
    /// 发送 snippet → MainWindow.sendSnippet（共享变量/broadcast 流程）。
    var onSendSnippet: ((String) -> Void)?
    /// history_enabled 全局开关变化 → MainWindow 应用到所有 session。
    var onHistoryToggle: (() -> Void)?
    /// host filter 标签解析（id → "name (addr)"）。
    var hostLabel: (String) -> String = { $0 }

    private let scroll = NSScrollView()
    private let box = VFlippedStack()
    private var search: VField!
    private var items: [OpaquePointer] = []         // snippet obj（需 free）
    private var hist: [HistRow] = []
    private var histFilter: String?                 // nil=全部 ""=仅本地
    private var histHostIds: [String] = []          // popup 里出现过的 host_id
    private var histHostPop: NSPopUpButton!
    private let histTable = NSTableView()
    private var histCard: NSView?
    private var histHC: NSLayoutConstraint?
    private var histEmpty: NSTextField?
    private var vaultObs: NSObjectProtocol?

    private struct HistRow {
        let id: Int64, ts: Int64, host: String, cmd: String
    }

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.hbBg = "content"
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.wantsLayer = true
        scroll.layer?.hbBg = "content"
        scroll.frame = NSRect(x: 0, y: 0, width: f.width,
                              height: f.height - 56)
        scroll.autoresizingMask = [.width, .height]
        box.orientation = .vertical
        box.spacing = 10
        box.alignment = .leading
        box.edgeInsets = NSEdgeInsets(top: 4, left: 18,
                                      bottom: 20, right: 18)
        box.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = box
        box.widthAnchor.constraint(
            equalTo: scroll.contentView.widthAnchor).isActive = true
        addSubview(scroll)
        let col = NSTableColumn()
        col.resizingMask = .autoresizingMask
        histTable.addTableColumn(col)
        histTable.headerView = nil
        histTable.dataSource = self
        histTable.delegate = self
        histTable.rowHeight = 30
        histTable.intercellSpacing = .zero
        histTable.selectionHighlightStyle = .none
        histTable.backgroundColor = Theme.dyn("card")
        histTable.setAccessibilityIdentifier("hist-table")
        buildBar()
        reload()
        vaultObs = NotificationCenter.default.addObserver(
            forName: .hbVaultChanged, object: nil, queue: .main)
        { [weak self] n in
            switch n.userInfo?["type"] as? String {
            case "history":
                self?.reloadHist()
                self?.rebuildContent()
            case "snippet":
                self?.reload()
            default: break
            }
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    deinit {
        if let o = vaultObs { NotificationCenter.default.removeObserver(o) }
    }

    // MARK: toolbar（两种模式各一套）
    private func buildBar() {
        subviews.filter { $0 !== scroll }.forEach { $0.removeFromSuperview() }
        histHostPop = nil
        var x: CGFloat = 18
        func place(_ v: NSView, w: CGFloat, h: CGFloat = 33) {
            v.frame = NSRect(x: x, y: bounds.height - 45 + (33 - h) / 2,
                             width: w, height: h)
            v.autoresizingMask = [.minYMargin]
            addSubview(v)
            x += w + 8
        }
        if mode == .snips {
            let split = VSplit(L("New snippet"), icon: "plus")
            split.entries = [
                VMenuEntry(L("新建 Snippet")) { [weak self] in
                    self?.newSnip()
                },
                VMenuEntry(L("导入 JSON…")) { [weak self] in
                    self?.importJSON()
                },
                VMenuEntry(L("导出 JSON")) { [weak self] in
                    self?.exportJSON()
                },
            ]
            split.size()
            split.main.setAccessibilityIdentifier("snip-new")
            split.dd.setAccessibilityIdentifier("snip-menu")
            place(split, w: split.frame.width)
            let hb = HBtn(L("Shell History"), style: .ghost, icon: "clock")
            hb.onTap = { [weak self] in self?.showHist() }
            hb.setAccessibilityIdentifier("snip-hist")
            place(hb, w: hb.intrinsicContentSize.width + 28)
        } else {
            let back = HBtn("‹ " + L("Snippets"))
            back.onTap = { [weak self] in self?.showSnips() }
            back.setAccessibilityIdentifier("hist-back")
            place(back, w: back.intrinsicContentSize.width + 28)
            let swl = vLabel(L("记录命令历史"))
            place(swl, w: swl.intrinsicContentSize.width, h: 18)
            x -= 4
            let sw = VSwitch(frame: .zero)
            sw.isOn = historyEnabled()
            sw.target = self
            sw.action = #selector(histToggled(_:))
            sw.setAccessibilityIdentifier("hist-enabled")
            place(sw, w: 38, h: 22)
            histHostPop = NSPopUpButton(frame: .zero)
            histHostPop.target = self
            histHostPop.action = #selector(histHostChanged(_:))
            histHostPop.setAccessibilityIdentifier("hist-host")
            place(histHostPop, w: 150, h: 26)
            let cl = HBtn(L("清空历史"))
            cl.onTap = { [weak self] in self?.clearHist() }
            cl.setAccessibilityIdentifier("hist-clear")
            place(cl, w: cl.intrinsicContentSize.width + 28)
            // 同步开关（M6 之前禁用，对齐 Linux 的 disabled 行）
            let synl = vLabel(L("同步"), dim: true)
            place(synl, w: synl.intrinsicContentSize.width, h: 18)
            x -= 4
            let syn = VSwitch(frame: .zero)
            syn.alphaValue = 0.45
            place(syn, w: 38, h: 22)
        }
        let oldQ = search?.stringValue ?? ""
        search = VField(placeholder: mode == .snips
                        ? L("搜索 Snippets…") : L("搜索历史…"), width: 220)
        search.stringValue = oldQ
        search.frame.origin = NSPoint(x: bounds.width - 238,
                                      y: bounds.height - 45)
        search.autoresizingMask = [.minXMargin, .minYMargin]
        search.onChange = { [weak self] in self?.searchChanged() }
        search.entry.setAccessibilityIdentifier(mode == .snips
                                                ? "snip-search" : "hist-search")
        addSubview(search)
    }

    private func historyEnabled() -> Bool {
        Vault.shared.settingsBool("history_enabled", true)
    }

    // MARK: 数据
    func reload() {
        for o in items { hb_obj_free(o) }
        items = Vault.shared.list("snippet")
        reloadHist()
        buildBar()          // host popup 重建也在这
        rebuildHostPop()
        rebuildContent()
    }

    private func reloadHist() {
        hist = []
        guard let v = Vault.shared.vault else { return }
        let q = search?.stringValue ?? ""
        let cb: hb_history_cb = { e, ud in
            guard let e, let ud else { return true }
            let p = Unmanaged<SnippetsPage>.fromOpaque(ud)
                .takeUnretainedValue()
            p.hist.append(HistRow(
                id: e.pointee.id, ts: e.pointee.ts,
                host: e.pointee.host_id.map { String(cString: $0) } ?? "",
                cmd: e.pointee.cmd.map { String(cString: $0) } ?? ""))
            return true
        }
        let selfp = Unmanaged.passUnretained(self).toOpaque()
        let qq: String? = q.isEmpty ? nil : q
        (qq ?? "").withCString { qs in
            if let hf = histFilter {
                hf.withCString { hs in
                    _ = hb_vault_history_foreach(
                        v, qq == nil ? nil : qs, hs, cb, selfp)
                }
            } else {
                _ = hb_vault_history_foreach(
                    v, qq == nil ? nil : qs, nil, cb, selfp)
            }
        }
        // host popup 选项 = 现有条目去重 host_id（保持顺序）
        var seen = Set<String>()
        histHostIds = hist.compactMap { r in
            guard !r.host.isEmpty, !seen.contains(r.host) else { return nil }
            seen.insert(r.host)
            return r.host
        }
        // 无过滤时再取一遍全量来填 popup（过滤后 popup 仍要列全）
        if histFilter != nil || !q.isEmpty {
            var all: [String] = []
            let cb2: hb_history_cb = { e, ud in
                guard let e, let ud else { return true }
                let arr = Unmanaged<NSMutableArray>.fromOpaque(ud)
                    .takeUnretainedValue()
                arr.add(e.pointee.host_id.map { String(cString: $0) } ?? "")
                return true
            }
            let arr = NSMutableArray()
            _ = hb_vault_history_foreach(v, nil, nil, cb2,
                Unmanaged.passUnretained(arr).toOpaque())
            var s2 = Set(histHostIds)
            for h in arr.compactMap({ $0 as? String }) where !h.isEmpty {
                if !s2.contains(h) { s2.insert(h); all.append(h) }
            }
            histHostIds.append(contentsOf: all)
        }
    }

    private func rebuildHostPop() {
        guard let p = histHostPop else { return }
        p.removeAllItems()
        p.addItem(withTitle: L("全部主机"))
        p.addItem(withTitle: L("本地"))
        for h in histHostIds { p.addItem(withTitle: hostLabel(h)) }
        let idx: Int
        if let f = histFilter {
            idx = f.isEmpty ? 1
                : (histHostIds.firstIndex(of: f).map { $0 + 2 } ?? 0)
        } else { idx = 0 }
        p.selectItem(at: idx)
    }

    // MARK: 内容（snips 卡片列表 / hist 表格卡片）
    private func rebuildContent() {
        box.arrangedSubviews.forEach {
            box.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }
        histCard = nil; histHC = nil; histEmpty = nil
        if mode == .snips {
            applySnipFilter()
            let rows = snipFiltered ?? items
            if rows.isEmpty {
                let wrap = NSView()
                let e = vEmptyState(icon: "search",
                                    title: L("无匹配 Snippet"))
                e.translatesAutoresizingMaskIntoConstraints = false
                wrap.addSubview(e)
                NSLayoutConstraint.activate([
                    e.centerXAnchor.constraint(equalTo: wrap.centerXAnchor),
                    e.topAnchor.constraint(equalTo: wrap.topAnchor,
                                           constant: 30),
                    e.bottomAnchor.constraint(equalTo: wrap.bottomAnchor,
                                              constant: -30),
                ])
                addContent(wrap)
            } else {
                for o in rows {
                    let c = SnippetCard(
                        title: hb_obj_get_str(o, "title").str,
                        body: hb_obj_get_str(o, "body").str,
                        scope: SnipKit.scopeLabel(
                            hb_obj_get_str(o, "scope").str)
                            { [weak self] in self?.hostLabel($0) })
                    let id = String(cString: hb_obj_id(o))
                    c.onSend = { [weak self] in self?.onSendSnippet?(id) }
                    c.onEdit = { [weak self] in self?.editSnip(id: id) }
                    c.onDelete = { [weak self] in self?.delSnip(id: id) }
                    c.setAccessibilityIdentifier("snip-card-\(id)")
                    c.editBtn.setAccessibilityIdentifier("snip-edit-\(id)")
                    c.delBtn.setAccessibilityIdentifier("snip-del-\(id)")
                    addContent(c)
                }
            }
        } else {
            let card = NSView()
            card.wantsLayer = true
            card.layer?.cornerRadius = 10
            card.layer?.masksToBounds = true
            card.layer?.hbBg = "card"
            histTable.translatesAutoresizingMaskIntoConstraints = false
            card.addSubview(histTable)
            NSLayoutConstraint.activate([
                histTable.topAnchor.constraint(equalTo: card.topAnchor),
                histTable.leadingAnchor.constraint(
                    equalTo: card.leadingAnchor),
                histTable.trailingAnchor.constraint(
                    equalTo: card.trailingAnchor),
                histTable.bottomAnchor.constraint(
                    equalTo: card.bottomAnchor),
            ])
            let hc = card.heightAnchor.constraint(
                equalToConstant: max(65, CGFloat(hist.count) * 30))
            hc.isActive = true
            histHC = hc
            let el = NSTextField(labelWithString: L("暂无命令历史"))
            el.font = .systemFont(ofSize: 12.5)
            el.hbText = "text3"
            el.alignment = .center
            el.isHidden = !hist.isEmpty
            el.translatesAutoresizingMaskIntoConstraints = false
            card.addSubview(el)
            NSLayoutConstraint.activate([
                el.centerXAnchor.constraint(equalTo: card.centerXAnchor),
                el.centerYAnchor.constraint(equalTo: card.centerYAnchor),
            ])
            histEmpty = el
            histCard = card
            histTable.reloadData()
            addContent(card)
        }
    }
    private func addContent(_ v: NSView) {
        v.translatesAutoresizingMaskIntoConstraints = false
        box.addArrangedSubview(v)
        v.widthAnchor.constraint(equalTo: box.widthAnchor).isActive = true
    }

    // MARK: hist 行
    func numberOfRows(in t: NSTableView) -> Int { hist.count }
    func tableView(_ t: NSTableView, rowViewForRow row: Int)
        -> NSTableRowView? { VTableRowView() }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let e = hist[row]
        let r = NSStackView()
        r.orientation = .horizontal
        r.spacing = 10
        r.edgeInsets = NSEdgeInsets(top: 5, left: 14,
                                    bottom: 5, right: 14)
        let cmd = vLabel(e.cmd, mono: true)
        cmd.setContentHuggingPriority(.init(1), for: .horizontal)
        cmd.setContentCompressionResistancePriority(.init(1),
                                                    for: .horizontal)
        r.addArrangedSubview(cmd)
        let host = e.host.isEmpty ? L("本地") : hostLabel(e.host)
        let hl = vLabel("\(LogFormat.date(e.ts)) · \(host)", dim: true)
        hl.font = .systemFont(ofSize: 10.5)
        hl.alignment = .right
        hl.translatesAutoresizingMaskIntoConstraints = false
        hl.widthAnchor.constraint(equalToConstant: 190).isActive = true
        r.addArrangedSubview(hl)
        let acts = NSStackView()
        acts.orientation = .horizontal
        acts.spacing = 2
        let sb = VIconBtn(icon: "list")
        sb.toolTip = L("存为 Snippet")
        sb.onTap = { [weak self] in
            self?.openEditor(existing: nil, prefillBody: e.cmd)
        }
        sb.setAccessibilityIdentifier("hist-tosnip")
        acts.addArrangedSubview(sb)
        let cp = VIconBtn(icon: "copy")
        cp.toolTip = L("复制")
        cp.onTap = {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(e.cmd, forType: .string)
        }
        cp.setAccessibilityIdentifier("hist-copy")
        acts.addArrangedSubview(cp)
        let db = VIconBtn(icon: "trash", kind: .del)
        db.toolTip = L("删除")
        db.onTap = { [weak self] in self?.histDel(id: e.id) }
        db.setAccessibilityIdentifier("hist-del")
        acts.addArrangedSubview(db)
        r.addArrangedSubview(acts)
        return r
    }

    // MARK: 动作
    private func searchChanged() {
        if mode == .hist {
            reloadHist()
            rebuildContent()
        } else {
            rebuildContent()
        }
    }

    /// snip 行内过滤。
    private var snipFiltered: [OpaquePointer]? = nil
    private func applySnipFilter() {
        let q = (search?.stringValue ?? "").lowercased()
        snipFiltered = q.isEmpty ? nil : items.filter {
            let t = hb_obj_get_str($0, "title").str.lowercased()
            let b = hb_obj_get_str($0, "body").str.lowercased()
            let s = hb_obj_get_str($0, "scope").str.lowercased()
            return t.contains(q) || b.contains(q) || s.contains(q)
        }
    }
    private func snipObj(id: String) -> OpaquePointer? {
        items.first { String(cString: hb_obj_id($0)) == id }
    }

    private func showHist() { mode = .hist; reload() }
    private func showSnips() { mode = .snips; reload() }

    @objc private func histToggled(_ s: VSwitch) {
        Vault.shared.setSettingsBool("history_enabled", s.isOn)
        onHistoryToggle?()
    }
    @objc private func histHostChanged(_ p: NSPopUpButton) {
        switch p.indexOfSelectedItem {
        case 0: histFilter = nil
        case 1: histFilter = ""
        default: histFilter = histHostIds[p.indexOfSelectedItem - 2]
        }
        reloadHist()
        rebuildContent()
    }
    private func histDel(id: Int64) {
        guard let v = Vault.shared.vault else { return }
        _ = hb_vault_history_delete(v, id)
        reloadHist()
        rebuildContent()
    }
    private func clearHist() {
        let a = NSAlert()
        a.messageText = L("清空命令历史")
        a.informativeText = L("确定删除全部 Shell History？此操作不可撤销。")
        a.addButton(withTitle: L("清空"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: "hist-clear",
                                      in: window) { ok in
            guard ok, let v = Vault.shared.vault else { return }
            _ = hb_vault_history_clear(v)
            self.reload()
        }
    }

    private func newSnip() { openEditor(existing: nil, prefillBody: nil) }
    private func editSnip(id: String) {
        guard let o = snipObj(id: id) else { return }
        openEditor(existing: o, prefillBody: nil)
    }
    private func delSnip(id: String) {
        guard let o = snipObj(id: id) else { return }
        let t = hb_obj_get_str(o, "title").str
        Vault.shared.delete(id)
        reload()
        let a = NSAlert()
        a.messageText = String(format: L("已删除 %@"), t)
        a.addButton(withTitle: L("撤销"))
        a.addButton(withTitle: L("关闭"))
        DialogRegistry.shared.present(alert: a, kind: "undo-del",
                                      in: window) { [weak self] ok in
            if ok { Vault.shared.undelete(id); self?.reload() }
        }
    }

    // MARK: 编辑器（NSAlert accessory：title/body/scope/run）
    private func openEditor(existing o: OpaquePointer?,
                            prefillBody: String?) {
        let isEdit = o != nil
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false
        let title = NSTextField()
        title.placeholderString = L("标题")
        title.setAccessibilityIdentifier("snip-title")
        let bodySV = NSScrollView()
        let body = NSTextView()
        body.isRichText = false
        body.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        body.setAccessibilityIdentifier("snip-body")
        bodySV.documentView = body
        bodySV.hasVerticalScroller = true
        bodySV.borderType = .bezelBorder
        let scopePop = NSPopUpButton()
        scopePop.addItems(withTitles: [L("全局"), L("指定主机"), L("指定分组")])
        scopePop.setAccessibilityIdentifier("snip-scope")
        let hostPop = NSPopUpButton()
        hostPop.setAccessibilityIdentifier("snip-host")
        for h in Vault.shared.list("host") {
            defer { hb_obj_free(h) }
            hostPop.addItem(withTitle: hostLabel(String(cString: hb_obj_id(h))))
        }
        let groupF = NSTextField()
        groupF.placeholderString = L("分组名")
        groupF.setAccessibilityIdentifier("snip-group")
        let run = NSButton(checkboxWithTitle: L("发送后回车运行"),
                           target: nil, action: nil)
        run.setAccessibilityIdentifier("snip-run")
        let scopeHelp = NSTextField(labelWithString:
            L("变量形如 {{name}} 或 {{name=默认}}"))
        scopeHelp.font = .systemFont(ofSize: 10.5)
        scopeHelp.hbText = "termDim"

        if let o {
            title.stringValue = hb_obj_get_str(o, "title").str
            body.string = prefillBody ?? hb_obj_get_str(o, "body").str
            let sc = hb_obj_get_str(o, "scope").str
            if sc.hasPrefix("host:") {
                scopePop.selectItem(at: 1)
                let hid = String(sc.dropFirst(5))
                let hosts = Vault.shared.list("host")
                defer { for h in hosts { hb_obj_free(h) } }
                let idx = hosts.firstIndex(where: {
                    String(cString: hb_obj_id($0)) == hid })
                if let idx { hostPop.selectItem(at: idx) }
            } else if sc.hasPrefix("group:") {
                scopePop.selectItem(at: 2)
                groupF.stringValue = String(sc.dropFirst(6))
            }
            run.state = hb_obj_get_bool(o, "run", false) ? .on : .off
        } else if let prefillBody {
            body.string = prefillBody
            title.stringValue = String(prefillBody.prefix(24))
        }

        for v: NSView in [title, bodySV, scopePop, hostPop, groupF,
                          run, scopeHelp] {
            v.translatesAutoresizingMaskIntoConstraints = false
            acc.addSubview(v)
        }
        NSLayoutConstraint.activate([
            acc.widthAnchor.constraint(equalToConstant: 380),
            title.topAnchor.constraint(equalTo: acc.topAnchor),
            title.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            title.trailingAnchor.constraint(equalTo: acc.trailingAnchor),
            bodySV.topAnchor.constraint(equalTo: title.bottomAnchor,
                                        constant: 8),
            bodySV.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            bodySV.trailingAnchor.constraint(equalTo: acc.trailingAnchor),
            bodySV.heightAnchor.constraint(equalToConstant: 120),
            scopePop.topAnchor.constraint(equalTo: bodySV.bottomAnchor,
                                          constant: 8),
            scopePop.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            hostPop.centerYAnchor.constraint(equalTo: scopePop.centerYAnchor),
            hostPop.leadingAnchor.constraint(
                equalTo: scopePop.trailingAnchor, constant: 8),
            hostPop.widthAnchor.constraint(equalToConstant: 160),
            groupF.centerYAnchor.constraint(
                equalTo: scopePop.centerYAnchor),
            groupF.leadingAnchor.constraint(
                equalTo: scopePop.trailingAnchor, constant: 8),
            groupF.trailingAnchor.constraint(equalTo: acc.trailingAnchor),
            run.topAnchor.constraint(equalTo: scopePop.bottomAnchor,
                                     constant: 8),
            run.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            scopeHelp.topAnchor.constraint(equalTo: run.bottomAnchor,
                                           constant: 6),
            scopeHelp.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            scopeHelp.bottomAnchor.constraint(equalTo: acc.bottomAnchor),
        ])
        let a = NSAlert()
        a.messageText = isEdit ? L("编辑 Snippet") : L("New snippet")
        a.addButton(withTitle: L("保存"))
        a.addButton(withTitle: L("取消"))
        a.accessoryView = acc
        DialogRegistry.shared.present(alert: a, kind: "snip-edit",
                                      in: window) { [weak self] ok in
            guard ok, let self else { return }
            let obj = o ?? Vault.shared.newObj("snippet")
            guard let obj else { return }
            hb_obj_set_str(obj, "title",
                           title.stringValue.isEmpty
                               ? "snippet" : title.stringValue)
            hb_obj_set_str(obj, "body", body.string)
            var scope = "global"
            switch scopePop.indexOfSelectedItem {
            case 1:
                let hosts = Vault.shared.list("host")
                defer { for h in hosts { hb_obj_free(h) } }
                let i = hostPop.indexOfSelectedItem
                if i >= 0 && i < hosts.count {
                    scope = "host:" + String(cString: hb_obj_id(hosts[i]))
                }
            case 2:
                if !groupF.stringValue.isEmpty {
                    scope = "group:" + groupF.stringValue
                }
            default: break
            }
            hb_obj_set_str(obj, "scope", scope)
            hb_obj_set_bool(obj, "run", run.state == .on)
            Vault.shared.put(obj)
            hb_obj_free(obj)
            self.reload()
        }
    }

    // MARK: 导入导出
    func importJSON(path: String? = nil) {
        var p = path
        if p == nil {
            let op = NSOpenPanel()
            op.allowedContentTypes = [.json]
            guard op.runModal() == .OK, let u = op.url else { return }
            p = u.path
        }
        guard let p, let txt = try? String(contentsOfFile: p,
                                           encoding: .utf8),
              let v = Vault.shared.vault else { return }
        var n = 0
        let ok = txt.withCString {
            hb_vault_import_json(v, "snippet", $0, &n)
        }
        reload()
        let a = NSAlert()
        a.messageText = ok == HB_OK
            ? String(format: L("已导入 %zu 条 Snippet"), n)
            : L("导入失败")
        DialogRegistry.shared.present(alert: a, kind: "toast",
                                      in: window) { _ in }
    }
    func exportJSON() {
        let sp = NSSavePanel()
        sp.nameFieldStringValue = "harbor-snippets.json"
        sp.allowedContentTypes = [.json]
        sp.beginSheetModal(for: window!) { r in
            guard r == .OK, let url = sp.url,
                  let v = Vault.shared.vault else { return }
            var out: UnsafeMutablePointer<CChar>?
            guard hb_vault_export_json(v, "snippet", &out) == HB_OK,
                  let out else { return }
            defer { free(out) }
            try? String(cString: out).write(to: url, atomically: true,
                                            encoding: .utf8)
        }
    }

    // MARK: dev
    func devMode(_ m: String) {
        mode = (m == "hist") ? .hist : .snips
        reload()
    }
    func devOpenEditor() {
        openEditor(existing: nil, prefillBody: nil)
    }
    var devState: String {
        var arr: [[String: Any]] = []
        if mode == .snips {
            applySnipFilter()
            for o in snipFiltered ?? items {
                arr.append([
                    "id": String(cString: hb_obj_id(o)),
                    "title": hb_obj_get_str(o, "title").str,
                    "scope": hb_obj_get_str(o, "scope").str])
            }
        } else {
            for e in hist {
                arr.append(["id": e.id, "host": e.host, "cmd": e.cmd])
            }
        }
        let d = try? JSONSerialization.data(withJSONObject: arr)
        return (d.flatMap { String(data: $0, encoding: .utf8) } ?? "[]")
    }
}

/// prototype .snippet-card：白卡 radius 10，hover 边框 cardHover；
/// sc-title（13.5 w600 + scope tag + 行内编辑/删除）+ sc-code
/// （Menlo 12 text2，subtle 底 radius 7，pre-wrap）。整卡点击发送。
private final class SnippetCard: NSView {
    var onSend: (() -> Void)?
    var onEdit: (() -> Void)?
    var onDelete: (() -> Void)?
    let editBtn = VIconBtn(icon: "wand")
    let delBtn = VIconBtn(icon: "trash", kind: .del)
    private var hovered = false

    init(title: String, body: String, scope: String) {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.borderWidth = 1
        layer?.hbBg = "card"
        restyleBorder()
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        let stack = VFlippedStack()
        stack.orientation = .vertical
        stack.spacing = 8
        stack.alignment = .leading
        stack.edgeInsets = NSEdgeInsets(top: 14, left: 16,
                                        bottom: 14, right: 16)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        let head = NSStackView()
        head.orientation = .horizontal
        head.spacing = 8
        let tl = vLabel(title, bold: true)
        tl.font = .systemFont(ofSize: 13.5, weight: .semibold)
        head.addArrangedSubview(tl)
        head.addArrangedSubview(hbTagLabel(scope))
        let spacer = NSView()
        spacer.translatesAutoresizingMaskIntoConstraints = false
        spacer.setContentHuggingPriority(.init(1), for: .horizontal)
        spacer.setContentCompressionResistancePriority(.init(1),
                                                       for: .horizontal)
        head.addArrangedSubview(spacer)
        editBtn.toolTip = L("编辑")
        editBtn.onTap = { [weak self] in self?.onEdit?() }
        head.addArrangedSubview(editBtn)
        delBtn.toolTip = L("删除")
        delBtn.onTap = { [weak self] in self?.onDelete?() }
        head.addArrangedSubview(delBtn)
        head.translatesAutoresizingMaskIntoConstraints = false
        stack.addArrangedSubview(head)
        head.widthAnchor.constraint(
            equalTo: stack.widthAnchor).isActive = true
        let code = NSView()
        code.wantsLayer = true
        code.layer?.cornerRadius = 7
        code.layer?.hbBg = "subtle"
        let cl = NSTextField(wrappingLabelWithString: body)
        cl.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        cl.hbText = "text2"
        cl.translatesAutoresizingMaskIntoConstraints = false
        cl.preferredMaxLayoutWidth = 900
        code.addSubview(cl)
        NSLayoutConstraint.activate([
            cl.topAnchor.constraint(equalTo: code.topAnchor, constant: 8),
            cl.leadingAnchor.constraint(equalTo: code.leadingAnchor,
                                        constant: 11),
            cl.trailingAnchor.constraint(equalTo: code.trailingAnchor,
                                         constant: -11),
            cl.bottomAnchor.constraint(equalTo: code.bottomAnchor,
                                       constant: -8),
        ])
        code.translatesAutoresizingMaskIntoConstraints = false
        stack.addArrangedSubview(code)
        code.widthAnchor.constraint(
            equalTo: stack.widthAnchor).isActive = true
        toolTip = L("点击发送到当前终端")
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; restyleBorder()
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; restyleBorder()
    }
    override func mouseDown(with e: NSEvent) { onSend?() }
    override func accessibilityPerformPress() -> Bool {
        onSend?(); return true
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        restyleBorder()
    }
    private func restyleBorder() {
        layer?.hbBorderResolver = { [weak self] in
            guard let self, self.hovered else {
                return NSColor.clear.cgColor
            }
            return Theme.cardHover.color.cgColor
        }
    }
}