import AppKit
import CHarbor
import HarborKit

/// FR-08 Vault「命令片段」页 —— Snippets + Shell History 双栈
/// （对齐 linux/src/hb-snippets-view.c：toolbar 切栈、卡片列表、
/// scope/host/group 编辑、删除可撤销、JSON 导入导出、history 开关）。
final class SnippetsPage: NSView, NSTableViewDataSource,
                          NSTableViewDelegate, NSTextFieldDelegate {
    private enum Mode { case snips, hist }
    private var mode: Mode = .snips
    /// 发送 snippet → MainWindow.sendSnippet（共享变量/broadcast 流程）。
    var onSendSnippet: ((String) -> Void)?
    /// history_enabled 全局开关变化 → MainWindow 应用到所有 session。
    var onHistoryToggle: (() -> Void)?
    /// host filter 标签解析（id → "name (addr)"）。
    var hostLabel: (String) -> String = { $0 }

    private let table = NSTableView()
    private let scroll = NSScrollView()
    private var search: NSSearchField!
    private var items: [OpaquePointer] = []         // snippet obj（需 free）
    private var hist: [HistRow] = []
    private var histFilter: String?                 // nil=全部 ""=仅本地
    private var histHostIds: [String] = []          // popup 里出现过的 host_id
    private var histSwitch: NSSwitch!
    private var histHostPop: NSPopUpButton!
    private var vaultObs: NSObjectProtocol?

    private struct HistRow {
        let id: Int64, ts: Int64, host: String, cmd: String
    }

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.backgroundColor = Theme.page.color.cgColor
        let col = NSTableColumn()
        table.addTableColumn(col)
        table.headerView = nil
        table.dataSource = self
        table.delegate = self
        table.rowHeight = 64
        table.intercellSpacing = NSSize(width: 0, height: 6)
        table.backgroundColor = .clear
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.frame = NSRect(x: 14, y: 0, width: f.width - 28,
                              height: f.height - 46)
        scroll.autoresizingMask = [.width, .height]
        addSubview(scroll)
        buildBar()
        reload()
        vaultObs = NotificationCenter.default.addObserver(
            forName: .hbVaultChanged, object: nil, queue: .main)
        { [weak self] n in
            switch n.userInfo?["type"] as? String {
            case "history":
                self?.reloadHist()
                self?.table.reloadData()
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
        let W = bounds.width
        var x: CGFloat = 14
        func btn(_ t: String, _ a: Selector, _ id: String,
                 w: CGFloat = 0) -> NSButton {
            let b = NSButton(title: t, target: self, action: a)
            b.bezelStyle = .rounded
            b.font = .systemFont(ofSize: 12)
            let bw = w > 0 ? w : max(30, b.intrinsicContentSize.width + 14)
            b.frame = NSRect(x: x, y: bounds.height - 38,
                             width: bw, height: 26)
            b.autoresizingMask = [.minYMargin]
            b.setAccessibilityIdentifier(id)
            addSubview(b)
            x += bw + 8
            return b
        }
        if mode == .snips {
            let nb = btn("+ " + L("New snippet"), #selector(newSnip), "snip-new")
            nb.bezelStyle = .rounded
            let menu = NSPopUpButton(frame: NSRect(
                x: x - 2, y: bounds.height - 38, width: 30, height: 26),
                pullsDown: true)
            menu.addItem(withTitle: "")
            menu.addItem(withTitle: L("新建 Snippet"))
            menu.addItem(withTitle: L("导入 JSON…"))
            menu.addItem(withTitle: L("导出 JSON"))
            menu.target = self
            menu.action = #selector(snipMenu(_:))
            menu.autoresizingMask = [.minYMargin]
            menu.setAccessibilityIdentifier("snip-menu")
            addSubview(menu)
            x += 32
            _ = btn(L("Shell History"), #selector(showHist), "snip-hist", w: 110)
        } else {
            _ = btn("‹ " + L("Snippets"), #selector(showSnips), "hist-back", w: 96)
            let swl = NSTextField(labelWithString: L("记录命令历史"))
            swl.font = .systemFont(ofSize: 12)
            swl.frame = NSRect(x: x, y: bounds.height - 35, width: 86, height: 18)
            swl.autoresizingMask = [.minYMargin]
            addSubview(swl)
            x += 88
            histSwitch = NSSwitch()
            histSwitch.state = historyEnabled() ? .on : .off
            histSwitch.target = self
            histSwitch.action = #selector(histToggled(_:))
            histSwitch.frame = NSRect(x: x, y: bounds.height - 37,
                                      width: 42, height: 24)
            histSwitch.autoresizingMask = [.minYMargin]
            histSwitch.setAccessibilityIdentifier("hist-enabled")
            addSubview(histSwitch)
            x += 48
            // host 过滤
            histHostPop = NSPopUpButton(frame: NSRect(
                x: x, y: bounds.height - 38, width: 130, height: 26))
            histHostPop.target = self
            histHostPop.action = #selector(histHostChanged(_:))
            histHostPop.autoresizingMask = [.minYMargin]
            histHostPop.setAccessibilityIdentifier("hist-host")
            addSubview(histHostPop)
            x += 136
            _ = btn(L("清空历史"), #selector(clearHist), "hist-clear", w: 84)
            // 同步开关（M6 之前禁用，对齐 Linux 的 disabled 行）
            let synl = NSTextField(labelWithString: L("同步"))
            synl.font = .systemFont(ofSize: 12)
            synl.textColor = Theme.termDim.color
            synl.frame = NSRect(x: x, y: bounds.height - 35,
                                width: 30, height: 18)
            synl.autoresizingMask = [.minYMargin]
            addSubview(synl)
            x += 32
            let syn = NSSwitch()
            syn.isEnabled = false
            syn.frame = NSRect(x: x, y: bounds.height - 37,
                               width: 42, height: 24)
            syn.autoresizingMask = [.minYMargin]
            addSubview(syn)
            x += 50
        }
        search = NSSearchField(frame: NSRect(
            x: W - 234, y: bounds.height - 39, width: 220, height: 26))
        search.placeholderString = mode == .snips
            ? L("搜索 Snippets…") : L("搜索历史…")
        search.target = self
        search.action = #selector(searchChanged)
        (search.cell as? NSSearchFieldCell)?.sendsSearchStringImmediately = true
        search.autoresizingMask = [.minXMargin, .minYMargin]
        search.setAccessibilityIdentifier(mode == .snips
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
        table.reloadData()
        table.rowHeight = mode == .snips ? 64 : 30
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

    // MARK: 行
    func numberOfRows(in t: NSTableView) -> Int {
        if mode == .hist { return hist.count }
        applySnipFilter()
        return (snipFiltered ?? items).count
    }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        mode == .snips ? snipRow(snipAt(row)) : histRow(row)
    }
    func tableViewSelectionDidChange(_ n: Notification) {
        guard mode == .snips else { return }
        let r = table.selectedRow
        guard r >= 0, r < numberOfRows(in: table) else { return }
        onSendSnippet?(String(cString: hb_obj_id(snipAt(r))))
        table.deselectRow(r)
    }

    private func snipRow(_ o: OpaquePointer) -> NSView {
        let id = String(cString: hb_obj_id(o))
        let title = hb_obj_get_str(o, "title").str
        let body = hb_obj_get_str(o, "body").str
        let scope = hb_obj_get_str(o, "scope").str
        let row = NSView()
        let tl = NSTextField(labelWithString: title)
        tl.font = .systemFont(ofSize: 13, weight: .medium)
        tl.lineBreakMode = .byTruncatingTail
        tl.frame = NSRect(x: 10, y: 34, width: 320, height: 18)
        row.addSubview(tl)
        let tag = NSTextField(labelWithString:
            " " + SnipKit.scopeLabel(scope) { hostLabel($0) } + " ")
        tag.font = .systemFont(ofSize: 10)
        tag.textColor = Theme.termDim.color
        tag.wantsLayer = true
        tag.layer?.borderWidth = 1
        tag.layer?.borderColor = Theme.line.color.cgColor
        tag.layer?.cornerRadius = 4
        tag.frame = NSRect(x: 10 + min(tl.intrinsicContentSize.width, 320) + 6,
                           y: 34, width: tag.intrinsicContentSize.width,
                           height: 18)
        row.addSubview(tag)
        let bl = NSTextField(labelWithString: body)
        bl.font = .monospacedSystemFont(ofSize: 11.5, weight: .regular)
        bl.textColor = Theme.termDim.color
        bl.lineBreakMode = .byTruncatingTail
        bl.frame = NSRect(x: 10, y: 8, width: bounds.width - 120, height: 20)
        bl.autoresizingMask = [.width]
        row.addSubview(bl)
        func ibtn(_ icon: String, _ aid: String,
                  _ act: Selector, dx: CGFloat) -> NSButton {
            let b = NSButton()
            b.isBordered = false
            b.image = hbIcon(icon, size: 13, tint: Theme.termDim.color)
            b.target = self
            b.action = act
            b.setAccessibilityIdentifier(aid + "-" + id)
            b.tag = items.firstIndex(where: { String(cString: hb_obj_id($0)) == id }) ?? 0
            b.frame = NSRect(x: bounds.width - 76 + dx, y: 32,
                             width: 26, height: 22)
            b.autoresizingMask = [.minXMargin]
            row.addSubview(b)
            return b
        }
        _ = ibtn("wand", "snip-edit", #selector(editRow(_:)), dx: 0)
        _ = ibtn("trash", "snip-del", #selector(delRow(_:)), dx: 30)
        return row
    }

    private func histRow(_ r: Int) -> NSView {
        let e = hist[r]
        let row = NSView()
        let tl = NSTextField(labelWithString: e.cmd)
        tl.font = .monospacedSystemFont(ofSize: 11.5, weight: .regular)
        tl.lineBreakMode = .byTruncatingTail
        tl.frame = NSRect(x: 10, y: 5, width: bounds.width - 330, height: 20)
        tl.autoresizingMask = [.width]
        row.addSubview(tl)
        let host = e.host.isEmpty ? L("本地") : hostLabel(e.host)
        let hl = NSTextField(labelWithString:
            "\(LogFormat.date(e.ts)) · \(host)")
        hl.font = .systemFont(ofSize: 10.5)
        hl.textColor = Theme.termDim.color
        hl.lineBreakMode = .byTruncatingTail
        hl.alignment = .right
        hl.frame = NSRect(x: bounds.width - 320, y: 6,
                          width: 160, height: 18)
        hl.autoresizingMask = [.minXMargin]
        row.addSubview(hl)
        func ibtn(_ icon: String, _ aid: String,
                  _ act: Selector, dx: CGFloat) {
            let b = NSButton()
            b.isBordered = false
            b.image = hbIcon(icon, size: 12, tint: Theme.termDim.color)
            b.target = self
            b.action = act
            b.tag = r
            b.setAccessibilityIdentifier(aid)
            b.frame = NSRect(x: bounds.width - 150 + dx, y: 3,
                             width: 24, height: 22)
            b.autoresizingMask = [.minXMargin]
            row.addSubview(b)
        }
        ibtn("list", "hist-tosnip", #selector(histToSnip(_:)), dx: 0)
        ibtn("copy", "hist-copy", #selector(histCopy(_:)), dx: 28)
        ibtn("trash", "hist-del", #selector(histDel(_:)), dx: 56)
        return row
    }

    // MARK: 动作
    @objc private func searchChanged() {
        reloadHist()
        table.reloadData()
    }

    /// snip 行内过滤：numberOfRows 用 filtered 列表。
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
    private func snipAt(_ r: Int) -> OpaquePointer {
        applySnipFilter()
        return (snipFiltered ?? items)[r]
    }

    @objc private func showHist() { mode = .hist; reload() }
    @objc private func showSnips() { mode = .snips; reload() }

    @objc private func histToggled(_ s: NSSwitch) {
        Vault.shared.setSettingsBool("history_enabled", s.state == .on)
        onHistoryToggle?()
    }
    @objc private func histHostChanged(_ p: NSPopUpButton) {
        switch p.indexOfSelectedItem {
        case 0: histFilter = nil
        case 1: histFilter = ""
        default: histFilter = histHostIds[p.indexOfSelectedItem - 2]
        }
        reloadHist()
        table.reloadData()
    }
    @objc private func histCopy(_ b: NSButton) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(hist[b.tag].cmd, forType: .string)
    }
    @objc private func histDel(_ b: NSButton) {
        guard let v = Vault.shared.vault else { return }
        _ = hb_vault_history_delete(v, hist[b.tag].id)
        reloadHist()
        table.reloadData()
    }
    @objc private func histToSnip(_ b: NSButton) {
        openEditor(existing: nil, prefillBody: hist[b.tag].cmd)
    }
    @objc private func clearHist() {
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

    @objc private func newSnip() { openEditor(existing: nil, prefillBody: nil) }
    @objc private func editRow(_ b: NSButton) {
        openEditor(existing: snipAt(b.tag), prefillBody: nil)
    }
    @objc private func delRow(_ b: NSButton) {
        let o = snipAt(b.tag)
        let id = String(cString: hb_obj_id(o))
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

    @objc private func snipMenu(_ p: NSPopUpButton) {
        defer { p.selectItem(at: 0) }
        switch p.indexOfSelectedItem {
        case 1: newSnip()
        case 2: importJSON()
        case 3: exportJSON()
        default: break
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
        scopeHelp.textColor = Theme.termDim.color

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
