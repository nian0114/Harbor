import AppKit
import CHarbor
import HarborKit

/// 主机库：vault "host" 对象 ↔ HostRec。
final class HostStore {
    var hosts: [HostRec] = []
    var objs: [String: OpaquePointer] = [:]   // id → live obj（编辑后 put）
    func reload() {
        objs.values.forEach { hb_obj_free($0) }
        objs.removeAll(); hosts.removeAll()
        for o in Vault.shared.list("host") {
            let id = hb_obj_id(o).str
            var h = HostRec(id: id)
            h.name = hb_obj_get_str(o, "name").str
            h.proto = hb_obj_get_str(o, "proto").str
            h.address = hb_obj_get_str(o, "address").str
            h.port = Int(hb_obj_get_int(o, "port", 22))
            h.user = hb_obj_get_str(o, "user").str
            h.group = hb_obj_get_str(o, "group").str
            h.color = hb_obj_get_str(o, "color").str
            h.os = hb_obj_get_str(o, "os").str
            h.auth = hb_obj_get_str(o, "auth").str
            h.keyId = hb_obj_get_str(o, "key_id").str
            h.identityFile = hb_obj_get_str(o, "identity_file").str
            h.keepalive = Int(hb_obj_get_int(o, "keepalive", -1))
            h.snippetId = hb_obj_get_str(o, "snippet").str
            h.lastConnected = hb_obj_get_int(o, "last_connected", 0)
            var sv: UnsafeMutablePointer<UnsafePointer<CChar>?>?
            let nt = hb_obj_get_strv(o, "tags", &sv)
            for i in 0..<nt { if let t = sv?[i] { h.tags.append(t.str) } }
            free(sv)
            let nj = hb_obj_get_strv(o, "jump", &sv)
            for i in 0..<nj { if let j = sv?[i] { h.jumps.append(j.str) } }
            free(sv)
            hosts.append(h); objs[id] = o
        }
    }
    /// 把 HostRec 写回其对象（编辑或新建）。
    func apply(_ h: HostRec) -> OpaquePointer {
        let o = objs[h.id] ?? hb_obj_new("host")!
        hb_obj_set_str(o, "name", h.name)
        hb_obj_set_str(o, "proto", h.proto.isEmpty ? "ssh" : h.proto)
        hb_obj_set_str(o, "address", h.address)
        hb_obj_set_int(o, "port", Int64(h.port))
        hb_obj_set_str(o, "user", h.user)
        hb_obj_set_str(o, "group", h.group)
        hb_obj_set_str(o, "color", h.color)
        hb_obj_set_str(o, "os", h.os)
        hb_obj_set_str(o, "auth", h.auth)
        hb_obj_set_str(o, "key_id", h.keyId)
        hb_obj_set_str(o, "identity_file", h.identityFile)
        hb_obj_set_int(o, "keepalive", Int64(h.keepalive))
        hb_obj_set_str(o, "snippet", h.snippetId)
        let tags = h.tags.map { strdup($0) }
        defer { tags.forEach { free($0) } }
        hb_obj_set_strv(o, "tags", tags.map { UnsafePointer($0) },
                            tags.count)
        let jumps = h.jumps.map { strdup($0) }
        defer { jumps.forEach { free($0) } }
        hb_obj_set_strv(o, "jump", jumps.map { UnsafePointer($0) },
                            jumps.count)
        Vault.shared.put(o)
        objs[hb_obj_id(o).str] = o
        return o
    }
    deinit { objs.values.forEach { hb_obj_free($0) } }
}

/// 主机页：搜索/快速连接/卡片+列表/分组/标签/排序/拖放/右键/抽屉/表单/导入导出。
final class HostsPage: NSView, NSTableViewDataSource, NSTableViewDelegate {
    let store = HostStore()
    private var filtered: [HostRec] = []
    private var query = ""
    private var cardMode = false
    private var sortKey = "order"      // order|name|group|last
    private var expandedGroups: Set<String> = []
    private var rows: [(kind: Int, host: HostRec?)] = []  // kind 0=组头 1=host

    var onConnect: ((HostRec) -> Void)?
    var onQuick: ((String) -> Void)?

    private let search = NSSearchField()
    private let quick = NSTextField()
    private let table = NSTableView()
    private let scroll = NSScrollView()
    private var drawer: NSView?
    private var drawerHost: HostRec?
    private let seg = NSSegmentedControl()

    override init(frame: NSRect) {
        super.init(frame: frame)
        build()
        reload()
    }
    required init?(coder: NSCoder) { fatalError() }

    private func build() {
        // 顶栏：搜索 + 快速连接 + 视图切换 + 排序 + 新建 + 导入导出
        wantsLayer = true
        layer?.backgroundColor = Theme.page.color.cgColor
        let bar = NSView(frame: NSRect(x: 0, y: frame.height - 40,
                                       width: frame.width, height: 40))
        bar.autoresizingMask = [.width, .minYMargin]
        bar.wantsLayer = true
        bar.layer?.backgroundColor = Theme.card.color.cgColor
        bar.layer?.borderColor = Theme.line.color.cgColor
        bar.layer?.borderWidth = 1
        addSubview(bar)
        search.placeholderString = L("搜索主机…")
        search.frame = NSRect(x: 8, y: 8, width: 180, height: 24)
        search.target = self; search.action = #selector(searchChanged)
        search.setAccessibilityIdentifier("hosts-search")
        bar.addSubview(search)
        quick.placeholderString = L("快速连接 user@host[:port]")
        quick.frame = NSRect(x: 196, y: 8, width: 220, height: 24)
        quick.target = self; quick.action = #selector(quickConnect)
        quick.setAccessibilityIdentifier("hosts-quick")
        bar.addSubview(quick)
        seg.segmentCount = 2
        seg.setLabel(L("列表"), forSegment: 0)
        seg.setLabel(L("卡片"), forSegment: 1)
        seg.frame = NSRect(x: 424, y: 8, width: 100, height: 24)
        seg.target = self; seg.action = #selector(toggleMode)
        seg.setAccessibilityIdentifier("hosts-mode")
        bar.addSubview(seg)
        let newB = NSButton(title: L("+ 新建"), target: self,
                            action: #selector(newHost))
        newB.bezelStyle = .rounded
        newB.frame = NSRect(x: 532, y: 8, width: 70, height: 24)
        newB.setAccessibilityIdentifier("hosts-new")
        bar.addSubview(newB)
        let imp = NSPopUpButton(frame: NSRect(x: 610, y: 8, width: 110, height: 24),
                                pullsDown: true)
        imp.addItem(withTitle: L("导入/导出"))
        imp.addItem(withTitle: L("导入 SSH 配置"))
        imp.addItem(withTitle: L("导入 JSON"))
        imp.addItem(withTitle: L("导出 JSON"))
        imp.target = self; imp.action = #selector(importExport(_:))
        imp.setAccessibilityIdentifier("hosts-import")
        bar.addSubview(imp)
        let sort = NSPopUpButton(frame: NSRect(x: 728, y: 8, width: 110, height: 24),
                                 pullsDown: true)
        sort.addItem(withTitle: L("排序"))
        sort.addItem(withTitle: L("名称"))
        sort.addItem(withTitle: L("分组"))
        sort.addItem(withTitle: L("最近连接"))
        sort.target = self; sort.action = #selector(sortChanged(_:))
        sort.setAccessibilityIdentifier("hosts-sort")
        bar.addSubview(sort)

        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.frame = NSRect(x: 0, y: 0, width: frame.width,
                              height: frame.height - 40)
        scroll.autoresizingMask = [.width, .height]
        addSubview(scroll)
        let col = NSTableColumn(identifier: .init("h"))
        col.title = L("主机")
        table.addTableColumn(col)
        table.headerView = nil
        table.dataSource = self; table.delegate = self
        table.doubleAction = #selector(doubleClick)
        table.target = self
        table.registerForDraggedTypes([.string])
        table.menu = ctxMenu()
        table.setAccessibilityIdentifier("hosts-table")
    }

    func reload() {
        store.reload()
        refilter()
    }
    /// dev hook：设置查询并触发过滤（用于测延迟）
    func devSearch(_ q: String) {
        search.stringValue = q
        query = q
        refilter()
        table.reloadData()
    }
    /// dev hook：选中某主机的行并滚动到它
    func devSelect(id: String) -> Bool {
        guard let ri = rows.firstIndex(where: { $0.host?.id == id })
        else { return false }
        table.selectRowIndexes(IndexSet(integer: ri),
                               byExtendingSelection: false)
        table.scrollRowToVisible(ri)
        return true
    }
    private func refilter() {
        var hs = store.hosts.filter { $0.match(query) }
        switch sortKey {
        case "name": hs.sort { $0.label.localizedCompare($1.label) == .orderedAscending }
        case "group": hs.sort { ($0.group, $0.label) < ($1.group, $1.label) }
        case "last": hs.sort { $0.lastConnected > $1.lastConnected }
        default: break
        }
        filtered = hs
        rows = []
        var lastGroup: String?
        for h in hs {
            if !h.group.isEmpty, h.group != lastGroup {
                rows.append((0, h)); lastGroup = h.group
            } else if h.group.isEmpty { lastGroup = nil }
            rows.append((1, h))
        }
        table.reloadData()
    }

    // MARK: actions
    @objc private func searchChanged() { query = search.stringValue; refilter() }
    @objc private func quickConnect() {
        let s = quick.stringValue.trimmingCharacters(in: .whitespaces)
        guard !s.isEmpty else { return }
        onQuick?(s); quick.stringValue = ""
    }
    @objc private func toggleMode() {
        cardMode = seg.selectedSegment == 1; table.reloadData()
    }
    @objc private func sortChanged(_ s: NSPopUpButton) {
        sortKey = ["order", "name", "group", "last"][max(0, s.indexOfSelectedItem)]
        refilter()
    }
    func tableViewSelectionDidChange(_ n: Notification) {
        if let h = selHost() { showDrawer(h) }
        else { drawer?.removeFromSuperview(); drawer = nil }
    }
    @objc private func doubleClick() {
        let r = table.clickedRow
        guard r >= 0, r < rows.count, rows[r].kind == 1,
              let h = rows[r].host else { return }
        onConnect?(h)
    }
    @objc private func newHost() { editHost(nil) }

    // MARK: 上下文菜单
    private func ctxMenu() -> NSMenu {
        let m = NSMenu()
        m.addItem(withTitle: L("连接"), action: #selector(ctxConnect), keyEquivalent: "")
        let sf = m.addItem(withTitle: "SFTP", action: nil, keyEquivalent: "")
        sf.isEnabled = false              // M3
        m.addItem(withTitle: L("编辑"), action: #selector(ctxEdit), keyEquivalent: "")
        m.addItem(withTitle: L("复制"), action: #selector(ctxDup), keyEquivalent: "")
        m.addItem(.separator())
        m.addItem(withTitle: L("删除"), action: #selector(ctxDel), keyEquivalent: "")
        m.addItem(withTitle: L("详情"), action: #selector(ctxInfo), keyEquivalent: "")
        m.items.forEach { $0.target = self }
        return m
    }
    private func selHost() -> HostRec? {
        let r = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow
        guard r >= 0, r < rows.count, rows[r].kind == 1 else { return nil }
        return rows[r].host
    }
    @objc private func ctxConnect() { selHost().map { onConnect?($0) } }
    @objc private func ctxEdit() { selHost().map { editHost($0) } }
    @objc private func ctxInfo() { selHost().map { showDrawer($0) } }
    @objc private func ctxDup() {
        guard var h = selHost() else { return }
        h.id = ""; h.name += L(" 副本")
        if let o = store.apply(h) as OpaquePointer? {
            _ = o
        reload()
        }
    }
    @objc private func ctxDel() {
        guard let h = selHost() else { return }
        Vault.shared.delete(h.id)
        reload()
        // undo：5 秒撤销提示
        NSSound.beep()
        let a = NSAlert()
        a.messageText = String(format: L("已删除 %@"), h.label)
        a.addButton(withTitle: L("撤销"))
        a.addButton(withTitle: L("关闭"))
        if let w = window {
            a.beginSheetModal(for: w) { r in
                if r == .alertFirstButtonReturn {
                    Vault.shared.undelete(h.id); self.reload()
                }
            }
        }
    }

    // MARK: 导入导出
    @objc private func importExport(_ s: NSPopUpButton) {
        defer { s.selectItem(at: 0) }
        switch s.indexOfSelectedItem {
        case 1: importSSHConfig()
        case 2: importJSON()
        case 3: exportJSON()
        default: break
        }
    }
    func importSSHConfig(path: String? = nil) {
        var p = path
        if p == nil {
            let op = NSOpenPanel(); op.allowsMultipleSelection = false
            guard op.runModal() == .OK, let u = op.url else { return }
            p = u.path
        }
        guard let p, let vv = Vault.shared.vault else { return }
        var cfg = hb_sshcfg()
        guard hb_sshcfg_parse_file(p, &cfg) == HB_OK else {
            errAlert(L("无法解析 SSH 配置文件")); return
        }
        var r = hb_import_result()
        let e = hb_hosts_import_sshcfg(vv, &cfg, &r)
        let n = Int(r.created) + Int(r.jumps_created)
        hb_import_result_free(&r); hb_sshcfg_free(&cfg)
        if e == HB_OK { reload()
        } else { errAlert(L("导入失败")) }
        _ = n
    }
    func importJSON(path: String? = nil) {
        var p = path
        if p == nil {
            let op = NSOpenPanel(); guard op.runModal() == .OK, let u = op.url
            else { return }
            p = u.path
        }
        guard let p, let data = try? String(contentsOfFile: p),
              let vv = Vault.shared.vault else { return }
        var r = hb_import_result()
        let e = data.withCString { hb_hosts_import_json(vv, $0, &r) }
        hb_import_result_free(&r)
        if e == HB_OK { reload() } else { errAlert(L("导入失败")) }
    }
    func exportJSON(path: String? = nil) {
        var p = path
        if p == nil {
            let sp = NSSavePanel(); sp.nameFieldStringValue = "hosts.json"
            guard sp.runModal() == .OK, let u = sp.url else { return }
            p = u.path
        }
        guard let p, let vv = Vault.shared.vault else { return }
        var out: UnsafeMutablePointer<CChar>?
        guard hb_hosts_export_json(vv, &out) == HB_OK, let out else { return }
        defer { free(out) }
        try? String(cString: out).write(toFile: p, atomically: true,
                                        encoding: .utf8)
    }
    private func errAlert(_ t: String) {
        let a = NSAlert(); a.messageText = t
        a.addButton(withTitle: L("确定"))
        if let w = window { a.beginSheetModal(for: w) { _ in } }
    }

    // MARK: table
    func numberOfRows(in t: NSTableView) -> Int { rows.count }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let e = rows[row]
        let id = NSUserInterfaceItemIdentifier(e.kind == 0 ? "grp" : "host")
        var v = t.makeView(withIdentifier: id, owner: nil) as? HostCell
        if v == nil { v = HostCell(group: e.kind == 0); v?.identifier = id }
        if e.kind == 0 {
            v?.showGroup(e.host?.group ?? "", count: 0)
        } else if let h = e.host {
            v?.showHost(h, card: cardMode)
        }
        return v
    }
    func tableView(_ t: NSTableView, heightOfRow row: Int) -> CGFloat {
        rows[row].kind == 0 ? 22 : (cardMode ? 64 : 34)
    }
    func tableView(_ t: NSTableView, writeRowsWith idx: IndexSet,
                   to pboard: NSPasteboard) -> Bool {
        guard let r = idx.first, r < rows.count, rows[r].kind == 1,
              let h = rows[r].host else { return false }
        pboard.declareTypes([.string], owner: nil)
        pboard.setString(h.id, forType: .string)
        return true
    }
    func tableView(_ t: NSTableView, validateDrop info: NSDraggingInfo,
                   proposedRow row: Int,
                   proposedDropOperation op: NSTableView.DropOperation)
        -> NSDragOperation {
        guard row < rows.count, rows[row].kind == 0,
              op == .on else { return [] }
        return .move
    }
    func tableView(_ t: NSTableView, acceptDrop info: NSDraggingInfo,
                   row: Int, dropOperation op: NSTableView.DropOperation)
        -> Bool {
        guard row < rows.count, rows[row].kind == 0,
              let grp = rows[row].host?.group,
              let sid = info.draggingPasteboard.string(forType: .string),
              var h = store.hosts.first(where: { $0.id == sid }) else {
            return false }
        h.group = grp
        _ = store.apply(h); reload()
        return true
    }

    // MARK: 详情抽屉
    private func showDrawer(_ h: HostRec) {
        drawer?.removeFromSuperview()
        let w: CGFloat = 240
        let d = NSView(frame: NSRect(x: bounds.width - w, y: 0,
                                     width: w, height: bounds.height - 40))
        d.autoresizingMask = [.minXMargin, .height]
        d.wantsLayer = true; d.layer?.backgroundColor = Theme.card.color.cgColor
        addSubview(d)
        drawer = d; drawerHost = h
        var y = d.bounds.height - 30
        func row(_ k: String, _ val: String) {
            let l = NSTextField(labelWithString: "\(k): \(val)")
            l.frame = NSRect(x: 12, y: y - 18, width: w - 24, height: 18)
            l.lineBreakMode = .byTruncatingTail
            d.addSubview(l); y -= 22
        }
        let ttl = NSTextField(labelWithString: h.label)
        ttl.font = .boldSystemFont(ofSize: 14)
        ttl.frame = NSRect(x: 12, y: y - 20, width: w - 24, height: 20)
        d.addSubview(ttl); y -= 30
        row(L("地址"), "\(h.user.isEmpty ? "" : h.user + "@")\(h.address):\(h.port)")
        row(L("认证"), h.auth); row(L("分组"), h.group)
        row(L("标签"), h.tags.joined(separator: ", "))
        row(L("跳板"), "\(h.jumps.count)")
        if h.lastConnected > 0 {
            row(L("最近连接"), Date(timeIntervalSince1970:
                TimeInterval(h.lastConnected) / 1000).description)
        }
        let eb = NSButton(title: L("编辑"), target: self,
                          action: #selector(ctxEdit))
        eb.bezelStyle = .rounded
        eb.frame = NSRect(x: 12, y: y - 28, width: 80, height: 24)
        eb.setAccessibilityIdentifier("drawer-edit")
        d.addSubview(eb)
        let cb = NSButton(title: L("连接"), target: self,
                          action: #selector(ctxConnect))
        cb.bezelStyle = .rounded
        cb.frame = NSRect(x: 100, y: y - 28, width: 80, height: 24)
        cb.setAccessibilityIdentifier("drawer-connect")
        d.addSubview(cb)
    }

    // MARK: 新建/编辑表单
    func editHost(_ h: HostRec?) {
        guard let w = window else { return }
        let sheet = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 480, height: 720),
                             styleMask: [.titled], backing: .buffered, defer: false)
        sheet.title = h == nil ? L("新建主机") : L("编辑主机")
        sheet.isReleasedWhenClosed = false
        let form = HostForm(frame: sheet.contentView!.bounds)
        if let h { form.fill(h) }
        form.onSave = { [weak self] rec, pw in
            let o = self?.store.apply(rec)
            if let o, let pw {
                Vault.shared.setSecret(hb_obj_id(o).str, "password", pw)
            }
            self?.reload()
            w.endSheet(sheet)
        }
        form.onCancel = { w.endSheet(sheet) }
        sheet.contentView = form
        w.beginSheet(sheet) { _ in }
    }
}

/// 主机行/卡片 cell（复用）。
final class HostCell: NSView {
    private let isGroup: Bool
    private let title = NSTextField(labelWithString: "")
    private let sub = NSTextField(labelWithString: "")
    private let meta = NSTextField(labelWithString: "")
    private let dot = NSView()
    init(group: Bool) {
        isGroup = group
        super.init(frame: .zero)
        if group {
            title.font = .boldSystemFont(ofSize: 11)
            title.frame = NSRect(x: 10, y: 2, width: 300, height: 18)
            addSubview(title)
        } else {
            dot.wantsLayer = true
            dot.layer?.cornerRadius = 4
            dot.frame = NSRect(x: 12, y: 14, width: 8, height: 8)
            addSubview(dot)
            title.frame = NSRect(x: 30, y: 8, width: 240, height: 20)
            addSubview(title)
            sub.frame = NSRect(x: 280, y: 8, width: 260, height: 18)
            sub.textColor = .secondaryLabelColor
            addSubview(sub)
            meta.frame = NSRect(x: 545, y: 8, width: 200, height: 18)
            meta.textColor = .tertiaryLabelColor
            meta.font = .systemFont(ofSize: 10)
            addSubview(meta)
        }
    }
    required init?(coder: NSCoder) { fatalError() }
    func showGroup(_ name: String, count: Int) {
        title.stringValue = "▸ \(name)"
    }
    func showHost(_ h: HostRec, card: Bool) {
        title.stringValue = (card ? "  " : "") + h.label
        sub.stringValue = "\(h.user.isEmpty ? "" : h.user + "@")\(h.address):\(h.port)"
        var m = h.os.isEmpty ? "" : "[\(h.os)] "
        m += h.tags.joined(separator: " ")
        if !h.group.isEmpty { m += " · \(h.group)" }
        meta.stringValue = m
        dot.layer?.backgroundColor = colorFor(h.color).cgColor
        if card { title.font = .boldSystemFont(ofSize: 14) }
    }
    private func colorFor(_ c: String) -> NSColor {
        switch c {
        case "red": return .systemRed; case "orange": return .systemOrange
        case "amber": return .systemYellow; case "green": return .systemGreen
        case "blue": return .systemBlue; case "pink": return .systemPink
        case "purple": return .systemPurple; default: return .systemGray
        }
    }
}
