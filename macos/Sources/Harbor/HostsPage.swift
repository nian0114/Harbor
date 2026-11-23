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
            h.domain = hb_obj_get_str(o, "domain").str
            h.tunnel = hb_obj_get_str(o, "tunnel").str
            h.via = hb_obj_get_str(o, "via").str
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
        hb_obj_set_str(o, "domain", h.domain)
        hb_obj_set_str(o, "tunnel", h.tunnel)
        hb_obj_set_str(o, "via", h.via)
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

/// OS → 卡片图标/底色（与 Linux os-tile 对应）。
func osTile(_ os: String) -> (icon: String, bg: Theme.RGBA) {
    switch os {
    case "ubuntu": return ("ubuntu", Theme.RGBA(0xe95420))
    case "debian": return ("asterisk", Theme.RGBA(0xa81d33))
    case "centos", "fedora", "rhel": return ("asterisk", Theme.RGBA(0x5b7ecb))
    case "arch": return ("asterisk", Theme.RGBA(0x1793d1))
    case "alpine": return ("asterisk", Theme.RGBA(0x0d597f))
    case "macos", "darwin", "apple": return ("monitor", Theme.RGBA(0x555a63))
    case "windows", "rdp": return ("win", Theme.RGBA(0x2f7cf6))
    case "vnc": return ("monitor", Theme.RGBA(0x2f7cf6))
    case "local": return ("terminal", Theme.RGBA(0x38404e))
    // 未知/未探测 OS → 通用 server 灰砖
    default: return ("server", Theme.RGBA(0x38404e))
    }
}

/// 主机页（对齐 linux hb-hosts-view + prototype）：
/// 顶栏搜索(⌘K)+连接；新建+导入导出；标签 chip 行；主机·N + 排序 + 卡片/列表。
final class HostsPage: NSView, NSTableViewDataSource, NSTableViewDelegate,
                       NSCollectionViewDataSource,
                       NSCollectionViewDelegateFlowLayout {
    let store = HostStore()
    private var filtered: [HostRec] = []
    private var query = ""
    private var tagFilter = ""
    private var cardMode = true
    private var sortKey = "name"       // name|group|last
    private var expandedGroups: Set<String> = []
    private var rows: [(kind: Int, host: HostRec?)] = []  // kind 0=组头 1=host
    private var keyMon: Any?

    var onConnect: ((HostRec) -> Void)?
    var onQuick: ((String) -> Void)?
    var onSftp: ((String) -> Void)?          // M3：host_id → SFTP tab

    private let search = NSSearchField()
    private let connBtn = NSButton()
    private let chipsRow = NSStackView()
    private let countL = NSTextField(labelWithString: "")
    private let table = NSTableView()
    private let listScroll = NSScrollView()
    private var grid: NSCollectionView!
    private let gridScroll = NSScrollView()
    private var drawer: NSView?
    private var drawerHost: HostRec?

    override init(frame: NSRect) {
        super.init(frame: frame)
        build()
        reload()
    }
    required init?(coder: NSCoder) { fatalError() }
    deinit { if let m = keyMon { NSEvent.removeMonitor(m) } }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if keyMon == nil, window != nil {
            keyMon = NSEvent.addLocalMonitorForEvents(
                matching: .keyDown) { [weak self] e in
                if e.modifierFlags.contains(.command),
                   e.charactersIgnoringModifiers == "k",
                   let self, self.window != nil {
                    self.window?.makeFirstResponder(self.search)
                    return nil
                }
                return e
            }
        }
    }

    private func primaryBtn(_ t: String, _ a: Selector,
                            _ aid: String) -> NSButton {
        let b = NSButton(title: t, target: self, action: a)
        b.isBordered = false
        b.wantsLayer = true
        b.layer?.cornerRadius = 6
        b.layer?.hbBg = "blue"
        b.attributedTitle = NSAttributedString(
            string: t, attributes: [.foregroundColor: NSColor.white,
                                    .font: NSFont.systemFont(
                                        ofSize: 12, weight: .medium)])
        b.setAccessibilityIdentifier(aid)
        return b
    }

    private func build() {
        wantsLayer = true
        layer?.hbBg = "page"
        let W = frame.width

        // ── 行1：搜索 + ⌘K + 连接 ──
        let bar1 = NSView(frame: NSRect(x: 0, y: frame.height - 44,
                                        width: W, height: 44))
        bar1.autoresizingMask = [.width, .minYMargin]
        addSubview(bar1)
        search.placeholderString = "Find a host or ssh user@hostname…"
        search.frame = NSRect(x: 12, y: 9, width: W - 160, height: 26)
        search.autoresizingMask = [.width]
        search.target = self; search.action = #selector(searchChanged)
        search.setAccessibilityIdentifier("hosts-search")
        bar1.addSubview(search)
        let kk = NSTextField(labelWithString: "⌘K")
        kk.textColor = .tertiaryLabelColor
        kk.font = .systemFont(ofSize: 11)
        // 贴在搜索框右内缘（搜索 maxX = W-148，留 6pt 边距）
        kk.frame = NSRect(x: W - 184, y: 14, width: 30, height: 16)
        kk.autoresizingMask = [.minXMargin]
        bar1.addSubview(kk)
        let cb = primaryBtn("Connect", #selector(connectSel), "hosts-connect")
        cb.frame = NSRect(x: W - 108, y: 8, width: 96, height: 28)
        cb.autoresizingMask = [.minXMargin]
        bar1.addSubview(cb)

        // ── 行2：新建主机 + ▾导入导出 ··· 排序 + 卡片/列表 ──
        let bar2 = NSView(frame: NSRect(x: 0, y: frame.height - 80,
                                        width: W, height: 36))
        bar2.autoresizingMask = [.width, .minYMargin]
        addSubview(bar2)
        let nb = primaryBtn("New host", #selector(newHost), "hosts-new")
        nb.frame = NSRect(x: 12, y: 6, width: 92, height: 26)
        bar2.addSubview(nb)
        let imp = NSPopUpButton(frame: NSRect(x: 108, y: 6, width: 32, height: 26),
                                pullsDown: true)
        imp.bezelStyle = .rounded
        imp.addItem(withTitle: "▾")
        imp.addItem(withTitle: L("从 ~/.ssh/config 导入…"))
        imp.addItem(withTitle: L("导入 JSON…"))
        imp.addItem(withTitle: L("导出 JSON…"))
        imp.target = self; imp.action = #selector(importExport(_:))
        imp.setAccessibilityIdentifier("hosts-import")
        bar2.addSubview(imp)
        let sort = NSPopUpButton(frame: NSRect(x: W - 176, y: 6,
                                             width: 100, height: 26),
                                 pullsDown: false)
        sort.addItems(withTitles: [L("按名称"), L("按分组"), L("按最近连接")])
        sort.target = self; sort.action = #selector(sortChanged(_:))
        sort.autoresizingMask = [.minXMargin]
        sort.setAccessibilityIdentifier("hosts-sort")
        bar2.addSubview(sort)
        let seg = NSSegmentedControl(
            images: [hbIcon("grid", size: 13, tint: Theme.dyn("text")),
                     hbIcon("list", size: 13, tint: Theme.dyn("text"))],
            trackingMode: .selectOne, target: self,
            action: #selector(toggleMode(_:)))
        seg.selectedSegment = 0
        seg.frame = NSRect(x: W - 68, y: 6, width: 56, height: 26)
        seg.autoresizingMask = [.minXMargin]
        seg.setAccessibilityIdentifier("hosts-mode")
        bar2.addSubview(seg)

        // ── 行3：标签 chips ──
        chipsRow.orientation = .horizontal
        chipsRow.spacing = 6
        chipsRow.edgeInsets = NSEdgeInsets(top: 0, left: 12, bottom: 0, right: 0)
        chipsRow.frame = NSRect(x: 0, y: frame.height - 114,
                                width: W, height: 28)
        chipsRow.autoresizingMask = [.width, .minYMargin]
        addSubview(chipsRow)

        // ── 行4：主机 · N ──
        countL.font = .boldSystemFont(ofSize: 11)
        countL.frame = NSRect(x: 12, y: frame.height - 140,
                              width: 300, height: 20)
        countL.autoresizingMask = [.minYMargin]
        addSubview(countL)

        // ── 内容：卡片网格 + 列表 ──
        let cy = frame.height - 148
        let lay = NSCollectionViewFlowLayout()
        lay.itemSize = NSSize(width: 244, height: 62)
        lay.minimumInteritemSpacing = 10
        lay.minimumLineSpacing = 10
        lay.sectionInset = NSEdgeInsets(top: 4, left: 12,
                                        bottom: 12, right: 12)
        grid = NSCollectionView(frame: NSRect(x: 0, y: 0, width: W, height: cy))
        grid.collectionViewLayout = lay
        grid.isSelectable = true
        grid.backgroundColors = [.clear]
        grid.register(HostCardItem.self,
                      forItemWithIdentifier: .init("card"))
        grid.dataSource = self; grid.delegate = self
        gridScroll.documentView = grid
        gridScroll.hasVerticalScroller = true
        gridScroll.drawsBackground = false
        gridScroll.frame = NSRect(x: 0, y: 0, width: W, height: cy)
        gridScroll.autoresizingMask = [.width, .height]
        addSubview(gridScroll)

        listScroll.documentView = table
        listScroll.hasVerticalScroller = true
        listScroll.frame = NSRect(x: 0, y: 0, width: W, height: cy)
        listScroll.autoresizingMask = [.width, .height]
        addSubview(listScroll)
        for (cid, title) in [("name", L("名称")), ("info", L("协议 · 用户")),
                             ("group", L("分组")), ("tags", L("标签"))] {
            let c = NSTableColumn(identifier: .init(cid))
            c.title = title
            c.width = cid == "name" ? 240 : (cid == "tags" ? 200 : 140)
            table.addTableColumn(c)
        }
        table.dataSource = self; table.delegate = self
        table.rowHeight = 34
        table.doubleAction = #selector(doubleClick)
        table.target = self
        table.registerForDraggedTypes([.string])
        table.menu = ctxMenu()
        table.setAccessibilityIdentifier("hosts-table")
        listScroll.isHidden = true
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
    }
    /// dev hook：选中某主机并滚动到它
    func devSelect(id: String) -> Bool {
        if cardMode {
            guard let i = filtered.firstIndex(where: { $0.id == id })
            else { return false }
            grid.selectionIndexes = [i]
            grid.scrollToItems(at: [IndexPath(item: i, section: 0)],
                               scrollPosition: .centeredVertically)
            showDrawer(filtered[i])
            return true
        }
        guard let ri = rows.firstIndex(where: { $0.host?.id == id })
        else { return false }
        table.selectRowIndexes(IndexSet(integer: ri),
                               byExtendingSelection: false)
        table.scrollRowToVisible(ri)
        if let h = rows[ri].host { showDrawer(h) }
        return true
    }
    /// dev hook：拖到分组（与 acceptDrop 同一路径）
    func devSetGroup(id: String, group: String) -> Bool {
        guard var h = store.hosts.first(where: { $0.id == id })
        else { return false }
        h.group = group
        _ = store.apply(h); reload()
        return true
    }
    /// dev hook：选中并删除（走撤销对话框路径）
    func devDelete(id: String) -> Bool {
        guard devSelect(id: id) else { return false }
        ctxDel()
        return true
    }
    /// dev hook：滚动计时 —— N 步滚动耗时（毫秒）
    func devScrollTest(steps: Int = 40) -> Double {
        let cv: NSView = cardMode ? grid : table
        guard let clip = cv.enclosingScrollView?.contentView else { return -1 }
        let maxY = max(0, cv.bounds.height - clip.bounds.height)
        let t0 = CACurrentMediaTime()
        for i in 0..<steps {
            clip.scroll(to: NSPoint(x: 0,
                y: maxY * CGFloat(i) / CGFloat(steps)))
            cv.layoutSubtreeIfNeeded()
            enclosingScrollView?.displayIfNeeded()
        }
        return (CACurrentMediaTime() - t0) * 1000 / Double(steps)
    }

    private func refilter() {
        var hs = store.hosts.filter {
            $0.match(query) &&
            (tagFilter.isEmpty || $0.tags.contains(tagFilter))
        }
        switch sortKey {
        case "group": hs.sort { ($0.group, $0.label) < ($1.group, $1.label) }
        case "last": hs.sort { $0.lastConnected > $1.lastConnected }
        default: hs.sort {
            $0.label.localizedCompare($1.label) == .orderedAscending }
        }
        filtered = hs
        countL.stringValue = L("主机") + " · \(hs.count)"
        rebuildChips()
        // 列表行（分组头）
        rows = []
        var lastGroup: String?
        for h in hs {
            if !h.group.isEmpty, h.group != lastGroup {
                rows.append((0, h)); lastGroup = h.group
            } else if h.group.isEmpty { lastGroup = nil }
            rows.append((1, h))
        }
        table.reloadData()
        grid.reloadData()
    }

    // MARK: chips
    private func rebuildChips() {
        chipsRow.arrangedSubviews.forEach { $0.removeFromSuperview() }
        var tags: [String] = []
        for h in store.hosts {
            for t in h.tags where !tags.contains(t) { tags.append(t) }
        }
        mkChip(L("全部"), on: tagFilter.isEmpty)
        for t in tags { mkChip(t, on: tagFilter == t) }
    }
    private func mkChip(_ t: String, on: Bool) {
        let b = NSButton(title: t, target: self,
                         action: #selector(chipHit(_:)))
        b.isBordered = false
        b.wantsLayer = true
        b.layer?.cornerRadius = 10
        b.layer?.backgroundColor =
            (on ? Theme.blue.color : Theme.chip.color).cgColor
        b.attributedTitle = NSAttributedString(
            string: t, attributes: [
                .foregroundColor: on ? NSColor.white
                                     : Theme.text.color,
                .font: NSFont.systemFont(ofSize: 11)])
        b.setAccessibilityIdentifier("chip-\(t)")
        b.heightAnchor.constraint(equalToConstant: 20).isActive = true
        chipsRow.addArrangedSubview(b)
    }
    @objc private func chipHit(_ s: NSButton) {
        tagFilter = s.title == L("全部") ? "" : s.title
        refilter()
    }

    // MARK: actions
    @objc private func searchChanged() {
        query = search.stringValue; refilter()
    }
    @objc private func connectSel() {
        // 搜索框形如 user@host[:port] → 快速连接；否则连选中的
        let s = search.stringValue.trimmingCharacters(in: .whitespaces)
        if s.contains("@") { onQuick?(s); return }
        if let h = selHost() { onConnect?(h) }
    }
    @objc private func toggleMode(_ s: NSSegmentedControl) {
        devMode(list: s.selectedSegment == 1)
    }
    func devMode(list: Bool) {
        cardMode = !list
        gridScroll.isHidden = !cardMode
        listScroll.isHidden = cardMode
    }
    @objc private func sortChanged(_ s: NSPopUpButton) {
        sortKey = ["name", "group", "last"][max(0, s.indexOfSelectedItem)]
        refilter()
    }
    func tableViewSelectionDidChange(_ n: Notification) {
        if let h = selHost() { showDrawer(h) }
        else { drawer?.removeFromSuperview(); drawer = nil }
    }
    func collectionView(_ c: NSCollectionView,
                        didSelectItemsAt idx: Set<IndexPath>) {
        guard let i = idx.first, i.item < filtered.count else { return }
        showDrawer(filtered[i.item])
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
        m.addItem(withTitle: "Connect", action: #selector(ctxConnect), keyEquivalent: "")
        m.addItem(withTitle: "SFTP", action: #selector(ctxSftp),
                  keyEquivalent: "")
        m.addItem(withTitle: L("编辑"), action: #selector(ctxEdit), keyEquivalent: "")
        m.addItem(withTitle: L("复制"), action: #selector(ctxDup), keyEquivalent: "")
        m.addItem(.separator())
        m.addItem(withTitle: L("删除"), action: #selector(ctxDel), keyEquivalent: "")
        m.addItem(withTitle: L("详情"), action: #selector(ctxInfo), keyEquivalent: "")
        m.items.forEach { $0.target = self }
        return m
    }
    private func selHost() -> HostRec? {
        if cardMode {
            guard let i = grid.selectionIndexes.first,
                  i < filtered.count else { return nil }
            return filtered[i]
        }
        let r = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow
        guard r >= 0, r < rows.count, rows[r].kind == 1 else { return nil }
        return rows[r].host
    }
    @objc private func ctxConnect() { selHost().map { onConnect?($0) } }
    @objc private func ctxSftp() { selHost().map { onSftp?($0.id) } }
    @objc private func ctxEdit() { selHost().map { editHost($0) } }
    @objc private func ctxInfo() { selHost().map { showDrawer($0) } }
    @objc private func ctxDup() {
        guard var h = selHost() else { return }
        h.id = ""; h.name += L(" 副本")
        _ = store.apply(h)
        reload()
    }
    @objc private func ctxDel() {
        guard let h = selHost() else { return }
        Vault.shared.delete(h.id)
        reload()
        let a = NSAlert()
        a.messageText = String(format: L("已删除 %@"), h.label)
        a.addButton(withTitle: L("撤销"))
        a.addButton(withTitle: L("关闭"))
        DialogRegistry.shared.present(alert: a, kind: "undo-del",
                                      in: window) { [weak self] ok in
            if ok {
                Vault.shared.undelete(h.id); self?.reload()
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
        hb_import_result_free(&r); hb_sshcfg_free(&cfg)
        if e == HB_OK { reload()
        } else { errAlert(L("导入失败")) }
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

    // MARK: table（列表模式）
    func numberOfRows(in t: NSTableView) -> Int { rows.count }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let e = rows[row]
        if e.kind == 0 {
            guard c?.identifier.rawValue == "name" else { return NSView() }
            let v = NSTextField(labelWithString: "▸ \(e.host?.group ?? "")")
            v.font = .boldSystemFont(ofSize: 11)
            return v
        }
        guard let h = e.host, let c else { return nil }
        switch c.identifier.rawValue {
        case "name":
            let v = NSTableCellView()
            let iv = NSImageView(frame: NSRect(x: 4, y: 7,
                                               width: 22, height: 22))
            let t = osTile(h.os)
            iv.wantsLayer = true
            iv.layer?.cornerRadius = 6
            iv.layer?.backgroundColor = t.bg.color.cgColor
            iv.image = hbIcon(t.icon, size: 13, tint: .white)
            v.addSubview(iv)
            let tf = NSTextField(labelWithString: h.label)
            tf.frame = NSRect(x: 32, y: 8, width: 200, height: 18)
            v.addSubview(tf)
            return v
        case "info":
            return NSTextField(
                labelWithString: "\(h.proto), \(h.user.isEmpty ? "—" : h.user)")
        case "group":
            return NSTextField(labelWithString: h.group)
        default:
            return NSTextField(labelWithString: h.tags.joined(separator: ", "))
        }
    }
    func tableView(_ t: NSTableView, heightOfRow row: Int) -> CGFloat {
        rows[row].kind == 0 ? 22 : 34
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
              let sid = info.draggingPasteboard.string(forType: .string)
        else { return false }
        return devSetGroup(id: sid, group: grp)
    }

    // MARK: collection（卡片网格）
    func collectionView(_ c: NSCollectionView,
                        numberOfItemsInSection s: Int) -> Int {
        filtered.count
    }
    func collectionView(_ c: NSCollectionView, itemForRepresentedObjectAt
                        p: IndexPath) -> NSCollectionViewItem {
        let it = c.makeItem(withIdentifier: .init("card"), for: p)
        (it as? HostCardItem)?.host = filtered[p.item]
        return it
    }

    // MARK: 详情抽屉
    private func showDrawer(_ h: HostRec) {
        drawer?.removeFromSuperview()
        let w: CGFloat = 240
        let d = NSView(frame: NSRect(x: bounds.width - w, y: 0,
                                     width: w, height: bounds.height - 148))
        d.autoresizingMask = [.minXMargin, .height]
        d.wantsLayer = true; d.layer?.hbBg = "card"
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
        row(L("系统"), h.os.isEmpty ? "—" : h.os)
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
        let cb = NSButton(title: "Connect", target: self,
                          action: #selector(ctxConnect))
        cb.bezelStyle = .rounded
        cb.frame = NSRect(x: 100, y: y - 28, width: 60, height: 24)
        cb.setAccessibilityIdentifier("drawer-connect")
        d.addSubview(cb)
        let sb = NSButton(title: "SFTP", target: self,
                          action: #selector(ctxSftp))
        sb.bezelStyle = .rounded
        sb.frame = NSRect(x: 166, y: y - 28, width: 60, height: 24)
        sb.setAccessibilityIdentifier("drawer-sftp")
        d.addSubview(sb)
    }

    // MARK: 新建/编辑表单
    func editHost(_ h: HostRec?) {
        guard let w = window else { return }
        let sheet = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 480, height: 640),
                             styleMask: [.titled], backing: .buffered, defer: false)
        sheet.title = h == nil ? "New host" : L("编辑主机")
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

/// 主机卡片（NSCollectionViewItem，复用）。
final class HostCardItem: NSCollectionViewItem {
    var host: HostRec? {
        didSet { (view as? HostCard)?.host = host }
    }
    override func loadView() { view = HostCard() }
    override var isSelected: Bool {
        didSet { (view as? HostCard)?.selected = isSelected }
    }
}

final class HostCard: NSView {
    var selected = false {
        didSet { updateBg() }
    }
    var host: HostRec? {
        didSet { fill() }
    }
    private let tile = NSView()
    private let tileImg = NSImageView()
    private let nameL = NSTextField(labelWithString: "")
    private let subL = NSTextField(labelWithString: "")
    private let tagRow = NSStackView()
    private var hovered = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.borderWidth = 1
        layer?.hbBorder = "line"
        updateBg()
        tile.wantsLayer = true
        tile.layer?.cornerRadius = 8
        tile.frame = NSRect(x: 12, y: 13, width: 36, height: 36)
        tileImg.frame = NSRect(x: 9, y: 9, width: 18, height: 18)
        tile.addSubview(tileImg)
        addSubview(tile)
        nameL.font = .systemFont(ofSize: 13, weight: .medium)
        nameL.frame = NSRect(x: 58, y: 32, width: 174, height: 18)
        nameL.lineBreakMode = .byTruncatingTail
        addSubview(nameL)
        subL.font = .systemFont(ofSize: 11)
        subL.textColor = .secondaryLabelColor
        subL.frame = NSRect(x: 58, y: 15, width: 174, height: 15)
        subL.lineBreakMode = .byTruncatingTail
        addSubview(subL)
        tagRow.orientation = .horizontal; tagRow.spacing = 4
        tagRow.frame = NSRect(x: 58, y: 2, width: 174, height: 14)
        addSubview(tagRow)
        let ta = NSTrackingArea(rect: .zero,
            options: [.mouseEnteredAndExited, .activeAlways,
                      .inVisibleRect],
            owner: self, userInfo: nil)
        addTrackingArea(ta)
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseEntered(with e: NSEvent) { hovered = true; updateBg() }
    override func mouseExited(with e: NSEvent) { hovered = false; updateBg() }
    private func updateBg() {
        layer?.hbBgResolver = { [weak self] in
            (self?.selected == true ? Theme.sel
                : (self?.hovered == true ? Theme.hover : Theme.card))
                .color.cgColor
        }
        layer?.hbBorderResolver = { [weak self] in
            (self?.selected == true ? Theme.blue : Theme.line)
                .color.cgColor
        }
    }
    private func fill() {
        guard let h = host else { return }
        let t = osTile(h.os)
        tile.layer?.backgroundColor = t.bg.color.cgColor
        tileImg.image = hbIcon(t.icon, size: 18, tint: .white)
        nameL.stringValue = h.label
        var sub = "\(h.proto), \(h.user.isEmpty ? "—" : h.user)"
        if !h.group.isEmpty { sub += " · \(h.group)" }
        subL.stringValue = sub
        tagRow.arrangedSubviews.forEach { $0.removeFromSuperview() }
        for tg in h.tags.prefix(3) {
            let c = NSTextField(labelWithString: tg)
            c.font = .systemFont(ofSize: 9)
            c.hbText = "blue"
            c.wantsLayer = true
            c.layer?.cornerRadius = 4
            c.layer?.hbBg = "chip"
            tagRow.addArrangedSubview(c)
        }
    }
}
