import AppKit
import CHarbor
import HarborKit
import ObjectiveC

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

/// OS → 卡片图标/底色（prototype os-ico 五类）。
func osTile(_ os: String) -> (icon: String, bg: Theme.RGBA) {
    switch os {
    case "ubuntu": return ("ubuntu", Theme.RGBA(0xe95420))
    case "local": return ("terminal", Theme.RGBA(0x3b4559))
    case "vnc": return ("monitor", Theme.RGBA(0x3f8cff))
    case "windows", "rdp": return ("win", Theme.RGBA(0x0f6cbd))
    default: return ("asterisk", Theme.amber)
    }
}

// MARK: - 通用小件（prototype .btn / .tag）

/// prototype .btn：gray/primary/ghost/danger + sm，hover 换底。
final class HBtn: NSButton {
    enum Style { case gray, primary, ghost, danger }
    var style: Style = .gray { didSet { restyle() } }
    var sm = false { didSet { restyle() } }
    var icon: String? { didSet { restyle() } }
    var titleText = "" { didSet { restyle() } }
    private var hovered = false
    override var isEnabled: Bool {
        didSet { alphaValue = isEnabled ? 1 : 0.45 }
    }

    convenience init(_ title: String, style: Style = .gray,
                     sm: Bool = false, icon: String? = nil) {
        self.init(frame: .zero)
        self.style = style; self.sm = sm
        self.icon = icon; titleText = title
        bezelStyle = .shadowlessSquare
        isBordered = false
        wantsLayer = true
        layer?.cornerRadius = 8
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        restyle()
    }
    override func mouseEntered(with e: NSEvent) { hovered = true; restyle() }
    override func mouseExited(with e: NSEvent) { hovered = false; restyle() }
    private func restyle() {
        let fg: NSColor
        switch style {
        case .primary: fg = .white
        case .danger: fg = Theme.dyn("red")
        default: fg = Theme.dyn("text")
        }
        layer?.hbBgResolver = { [weak self] in
            guard let self else { return nil }
            switch self.style {
            case .primary:
                return (self.hovered ? Theme.primaryHover : Theme.blue)
                    .color.cgColor
            case .danger: return Theme.dangerBg.color.cgColor
            case .ghost:
                return (self.hovered ? Theme.btn : Theme.RGBA(0, 0))
                    .color.cgColor
            case .gray:
                return (self.hovered ? Theme.btnHover : Theme.btn)
                    .color.cgColor
            }
        }
        let font = NSFont.systemFont(ofSize: sm ? 12 : 13, weight: .medium)
        let s = NSMutableAttributedString()
        if let icon {
            let att = NSTextAttachment()
            att.image = hbIcon(icon, size: sm ? 12 : 13, tint: fg)
            let m = NSMutableAttributedString(attachment: att)
            m.addAttribute(.baselineOffset, value: -2,
                           range: NSRange(location: 0, length: m.length))
            s.append(m)
            s.append(NSAttributedString(string: " "))
        }
        s.append(NSAttributedString(string: titleText))
        s.addAttributes([.font: font, .foregroundColor: fg],
                        range: NSRange(location: 0, length: s.length))
        attributedTitle = s
    }
}

/// prototype .tag：bluePill / greenPill 小标签。
func hbTagLabel(_ t: String, green: Bool = false) -> NSTextField {
    let l = NSTextField(labelWithString: t)
    l.font = .systemFont(ofSize: 10.5, weight: .semibold)
    l.hbText = green ? "greenPillFg" : "bluePillFg"
    l.alignment = .center
    l.wantsLayer = true
    l.layer?.cornerRadius = 6
    l.layer?.hbBg = green ? "greenPillBg" : "bluePillBg"
    l.frame.size = NSSize(width: l.intrinsicContentSize.width + 14,
                          height: 18)
    return l
}

private var kHbTapKey: UInt8 = 0
extension NSButton {
    /// 闭包点击（表格行内按钮用）。
    var onTap: (() -> Void)? {
        get { objc_getAssociatedObject(self, &kHbTapKey) as? () -> Void }
        set {
            objc_setAssociatedObject(self, &kHbTapKey, newValue,
                                     .OBJC_ASSOCIATION_RETAIN)
            if newValue != nil {
                target = self; action = #selector(hbTapFire)
            }
        }
    }
    @objc fileprivate func hbTapFire() { onTap?() }
}

/// prototype .sortsel：白底描边下拉。
final class SortSel: NSView {
    var items: [String] = [] { didSet { refresh() } }
    var selected = 0 { didSet { refresh() } }
    var onPick: ((Int) -> Void)?
    private let label = NSTextField(labelWithString: "")
    private var hovered = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.borderWidth = 1
        layer?.hbBorder = "line"
        layer?.hbBgResolver = { [weak self] in
            (self?.hovered == true ? Theme.subtle : Theme.card)
                .color.cgColor
        }
        label.font = .systemFont(ofSize: 12.5)
        label.hbText = "text2"
        label.frame = NSRect(x: 10, y: 7, width: frame.width - 32,
                             height: 16)
        label.autoresizingMask = [.width]
        label.lineBreakMode = .byTruncatingTail
        addSubview(label)
        let cv = NSImageView(frame: NSRect(x: frame.width - 22, y: 9,
                                           width: 12, height: 12))
        cv.autoresizingMask = [.minXMargin]
        cv.image = hbIcon("chevron-d", size: 12, tint: Theme.dyn("text3"))
        addSubview(cv)
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
    }
    required init?(coder: NSCoder) { fatalError() }
    private func refresh() {
        label.stringValue = items.indices.contains(selected)
            ? items[selected] : ""
    }
    override func mouseEntered(with e: NSEvent) { hovered = true }
    override func mouseExited(with e: NSEvent) { hovered = false }
    override func mouseDown(with e: NSEvent) {
        let m = NSMenu()
        for (i, t) in items.enumerated() {
            let it = m.addItem(withTitle: t, action: #selector(pick(_:)),
                               keyEquivalent: "")
            it.target = self; it.tag = i
            it.state = i == selected ? .on : .off
        }
        m.popUp(positioning: nil, at: NSPoint(x: 0, y: 0), in: self)
    }
    @objc private func pick(_ s: NSMenuItem) {
        selected = s.tag; onPick?(s.tag)
    }
}

/// prototype .table th：tableHead 底 + 12 semibold text2。
final class TableHeadCell: NSTableHeaderCell {
    override func draw(withFrame cellFrame: NSRect, in controlView: NSView) {
        Theme.dyn("tableHead").setFill()
        cellFrame.fill()
        Theme.dyn("line").setFill()
        NSRect(x: cellFrame.minX, y: cellFrame.maxY - 1,
               width: cellFrame.width, height: 1).fill()
        let s = NSAttributedString(string: stringValue, attributes: [
            .font: NSFont.systemFont(ofSize: 12, weight: .semibold),
            .foregroundColor: Theme.dyn("text2"),
        ])
        let sz = s.size()
        s.draw(at: NSPoint(x: cellFrame.minX + 14,
                           y: cellFrame.midY - sz.height / 2))
    }
}

class TableHeaderView: NSTableHeaderView {
    override func draw(_ dirtyRect: NSRect) {
        Theme.dyn("tableHead").setFill()
        bounds.fill()
        guard let tv = tableView else { return }
        for (i, c) in tv.tableColumns.enumerated() {
            c.headerCell.draw(withFrame: headerRect(ofColumn: i), in: self)
        }
    }
}

/// prototype .rowlink：hover #f6f9fc，行底细线，无系统选中高亮。
final class HostRowView: NSTableRowView {
    private var hovered = false { didSet { needsDisplay = true } }
    override init(frame: NSRect) {
        super.init(frame: frame)
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseEntered(with e: NSEvent) { hovered = true }
    override func mouseExited(with e: NSEvent) { hovered = false }
    override func drawBackground(in dirtyRect: NSRect) {
        if hovered {
            Theme.dyn("rowHover").setFill()
            dirtyRect.fill()
        }
    }
    override func drawSeparator(in dirtyRect: NSRect) {
        Theme.dyn("rowLine").setFill()
        NSRect(x: 0, y: bounds.maxY - 1, width: bounds.width,
               height: 1).fill()
    }
    override func drawSelection(in dirtyRect: NSRect) {}
}

// MARK: - 主机页

/// 主机页（对齐 prototype hostsView）：
/// quickbar(搜索+⌘K+Connect)；toolbar(New host▾/Serial/排序/视图)；
/// 分组 chips；Hosts·N + 卡片网格/表格；400px 详情抽屉。
final class HostsPage: NSView, NSTableViewDataSource, NSTableViewDelegate,
                       NSCollectionViewDataSource,
                       NSCollectionViewDelegateFlowLayout,
                       NSTextFieldDelegate {
    let store = HostStore()
    private var filtered: [HostRec] = []
    private var query = ""
    private var chipFilter = ""          // ""=全部 "__tun__"=SSH 隧道 其余=分组
    private var chipIds: [String] = []
    private var cardMode = true
    private var sortKey = "name"         // name|last|proto

    var onConnect: ((HostRec) -> Void)?
    var onQuick: ((String) -> Void)?
    var onSftp: ((String) -> Void)?

    private let quickBar = NSView()
    private let search = NSTextField()
    private let chipsRow = NSStackView()
    private let countL = NSTextField(labelWithString: "")
    private let table = NSTableView()
    private let listScroll = NSScrollView()
    private var grid: NSCollectionView!
    private let gridScroll = NSScrollView()
    private var sortSel: SortSel!
    private var modeBtns: [NSButton] = []
    private var emptyView: NSView!
    private var scrim: NSView?
    private var drawer: HostDrawer?
    private var drawerHost: HostRec?

    override init(frame: NSRect) {
        super.init(frame: frame)
        build()
        reload()
    }
    required init?(coder: NSCoder) { fatalError() }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        sizeGrid()
        sizeColumns()
    }

    private func build() {
        wantsLayer = true
        layer?.hbBg = "content"
        let W = frame.width, H = frame.height

        // ── quickbar：搜索 + ⌘K + Connect ──
        quickBar.frame = NSRect(x: 18, y: H - 54, width: W - 36, height: 40)
        quickBar.autoresizingMask = [.width, .minYMargin]
        quickBar.wantsLayer = true
        quickBar.layer?.cornerRadius = 10
        quickBar.layer?.borderWidth = 1
        quickBar.layer?.hbBorder = "line"
        quickBar.layer?.hbBg = "subtle"
        addSubview(quickBar)
        let qw = quickBar.frame.width
        let si = NSImageView(frame: NSRect(x: 14, y: 12, width: 16,
                                           height: 16))
        si.image = hbIcon("search", size: 16, tint: Theme.dyn("text3"))
        quickBar.addSubview(si)
        search.frame = NSRect(x: 38, y: 10, width: qw - 38 - 142, height: 20)
        search.autoresizingMask = [.width]
        search.isBordered = false
        search.drawsBackground = false
        search.focusRingType = .none
        search.font = .systemFont(ofSize: 13.5)
        search.placeholderString = "Find a host or ssh user@hostname…"
        search.delegate = self
        search.target = self; search.action = #selector(connectSel)
        search.setAccessibilityIdentifier("hosts-search")
        quickBar.addSubview(search)
        let kk = NSButton(title: "⌘K", target: self,
                          action: #selector(openPalette))
        kk.isBordered = false
        kk.wantsLayer = true
        kk.layer?.cornerRadius = 5
        kk.layer?.borderWidth = 1
        kk.layer?.hbBorder = "line"
        kk.layer?.hbBg = "card"
        kk.attributedTitle = NSAttributedString(
            string: "⌘K", attributes: [
                .font: NSFont.systemFont(ofSize: 11),
                .foregroundColor: Theme.dyn("text3")])
        kk.frame = NSRect(x: qw - 134, y: 9, width: 36, height: 22)
        kk.autoresizingMask = [.minXMargin]
        quickBar.addSubview(kk)
        let cb = HBtn("Connect", sm: true)
        cb.target = self; cb.action = #selector(connectSel)
        cb.frame = NSRect(x: qw - 90, y: 6, width: 82, height: 28)
        cb.autoresizingMask = [.minXMargin]
        cb.setAccessibilityIdentifier("hosts-connect")
        quickBar.addSubview(cb)

        // ── toolbar：New host▾ · Serial ··· 排序 + 视图 ──
        let bar2 = NSView(frame: NSRect(x: 18, y: H - 98,
                                        width: W - 36, height: 32))
        bar2.autoresizingMask = [.width, .minYMargin]
        addSubview(bar2)
        let tw = bar2.frame.width
        let nb = HBtn("New host", style: .gray, icon: "plus")
        nb.target = self; nb.action = #selector(newHost)
        nb.frame = NSRect(x: 0, y: 1, width: 110, height: 30)
        nb.layer?.maskedCorners = [.layerMinXMinYCorner, .layerMinXMaxYCorner]
        nb.setAccessibilityIdentifier("hosts-new")
        bar2.addSubview(nb)
        let imp = HBtn("▾", style: .gray)
        imp.target = self; imp.action = #selector(importMenu(_:))
        imp.frame = NSRect(x: 110, y: 1, width: 30, height: 30)
        imp.layer?.maskedCorners = [.layerMaxXMinYCorner, .layerMaxXMaxYCorner]
        imp.setAccessibilityIdentifier("hosts-import")
        bar2.addSubview(imp)
        let divider = NSView(frame: NSRect(x: 110, y: 1, width: 1,
                                           height: 30))
        divider.wantsLayer = true
        divider.layer?.backgroundColor =
            NSColor.black.withAlphaComponent(0.08).cgColor
        bar2.addSubview(divider)
        let serial = HBtn("Serial", style: .ghost, icon: "plug")
        serial.target = self; serial.action = #selector(serialInfo)
        serial.frame = NSRect(x: 148, y: 1, width: 88, height: 30)
        bar2.addSubview(serial)
        sortSel = SortSel(frame: NSRect(x: tw - 184, y: 1,
                                        width: 112, height: 30))
        sortSel.autoresizingMask = [.minXMargin]
        sortSel.items = [L("按名称"), L("按最近连接"), L("按协议")]
        sortSel.onPick = { [weak self] i in
            self?.sortKey = ["name", "last", "proto"][i]
            self?.refilter()
        }
        sortSel.setAccessibilityIdentifier("hosts-sort")
        bar2.addSubview(sortSel)
        for i in 0..<2 {
            let b = NSButton(title: "", target: self,
                             action: #selector(modeHit(_:)))
            b.tag = i
            b.isBordered = false
            b.wantsLayer = true
            b.layer?.cornerRadius = 7
            b.frame = NSRect(x: tw - 66 + CGFloat(i) * 34, y: 1,
                             width: 32, height: 30)
            b.autoresizingMask = [.minXMargin]
            b.setAccessibilityIdentifier(i == 0 ? "hosts-mode"
                                                : "hosts-mode-list")
            bar2.addSubview(b)
            modeBtns.append(b)
        }
        refreshModeBtns()

        // ── 分组 chips ──
        chipsRow.orientation = .horizontal
        chipsRow.spacing = 7
        chipsRow.frame = NSRect(x: 18, y: H - 132, width: W - 36, height: 26)
        chipsRow.autoresizingMask = [.width, .minYMargin]
        addSubview(chipsRow)

        // ── Hosts · N ──
        countL.font = .boldSystemFont(ofSize: 13)
        countL.frame = NSRect(x: 20, y: H - 160, width: 300, height: 18)
        countL.autoresizingMask = [.minYMargin]
        addSubview(countL)

        // ── 卡片网格 ──
        let cy = H - 168
        let lay = NSCollectionViewFlowLayout()
        lay.minimumInteritemSpacing = 12
        lay.minimumLineSpacing = 12
        lay.sectionInset = NSEdgeInsets(top: 4, left: 18,
                                        bottom: 20, right: 18)
        grid = NSCollectionView(frame: NSRect(x: 0, y: 0, width: W,
                                              height: cy))
        grid.collectionViewLayout = lay
        grid.isSelectable = true
        grid.backgroundColors = [.clear]
        grid.register(HostCardItem.self,
                      forItemWithIdentifier: .init("card"))
        grid.dataSource = self; grid.delegate = self
        let dbl = NSClickGestureRecognizer(target: self,
                                           action: #selector(gridDbl(_:)))
        dbl.numberOfClicksRequired = 2
        grid.addGestureRecognizer(dbl)
        gridScroll.documentView = grid
        gridScroll.hasVerticalScroller = true
        gridScroll.drawsBackground = false
        gridScroll.frame = NSRect(x: 0, y: 0, width: W, height: cy)
        gridScroll.autoresizingMask = [.width, .height]
        addSubview(gridScroll)
        sizeGrid()

        // ── 表格 ──
        listScroll.frame = NSRect(x: 18, y: 8, width: W - 36,
                                  height: H - 184)
        listScroll.autoresizingMask = [.width, .height]
        listScroll.hasVerticalScroller = true
        listScroll.drawsBackground = false
        listScroll.wantsLayer = true
        listScroll.layer?.cornerRadius = 10
        listScroll.layer?.masksToBounds = true
        listScroll.layer?.hbBg = "card"
        addSubview(listScroll)
        for (cid, title) in [("name", L("名称")), ("user", L("用户")),
                             ("port", L("端口")), ("proto", L("协议")),
                             ("group", L("分组")), ("go", "")] {
            let c = NSTableColumn(identifier: .init(cid))
            c.title = title
            c.headerCell = TableHeadCell(textCell: title)
            c.resizingMask = []
            table.addTableColumn(c)
        }
        table.frame = listScroll.bounds
        table.autoresizingMask = [.width, .height]
        table.dataSource = self; table.delegate = self
        table.rowHeight = 40
        table.intercellSpacing = .zero
        table.headerView = TableHeaderView(frame: NSRect(x: 0, y: 0,
                                                         width: W - 36,
                                                         height: 34))
        table.backgroundColor = .clear
        table.selectionHighlightStyle = .none
        table.gridStyleMask = []
        table.style = .plain
        table.doubleAction = #selector(doubleClick)
        table.target = self
        table.menu = ctxMenu()
        table.setAccessibilityIdentifier("hosts-table")
        listScroll.documentView = table
        listScroll.isHidden = true
        sizeColumns()

        buildEmpty()
    }

    private func buildEmpty() {
        let e = NSView(frame: NSRect(x: 0, y: 0, width: frame.width,
                                     height: frame.height - 168))
        e.autoresizingMask = [.width, .height]
        let cx = e.frame.width / 2
        let cy = e.frame.height / 2
        let ico = NSView(frame: NSRect(x: cx - 29, y: cy + 34,
                                       width: 58, height: 58))
        ico.autoresizingMask = [.minXMargin, .maxXMargin,
                                .minYMargin, .maxYMargin]
        ico.wantsLayer = true
        ico.layer?.cornerRadius = 14
        ico.layer?.hbBg = "chip"
        let iv = NSImageView(frame: NSRect(x: 17, y: 17, width: 24,
                                           height: 24))
        iv.image = hbIcon("search", size: 24, tint: Theme.dyn("text2"))
        ico.addSubview(iv)
        e.addSubview(ico)
        let t = NSTextField(labelWithString: L("没有匹配的主机"))
        t.font = .boldSystemFont(ofSize: 15)
        t.sizeToFit()
        t.frame.origin = NSPoint(x: cx - t.frame.width / 2, y: cy + 4)
        t.autoresizingMask = [.minXMargin, .maxXMargin,
                              .minYMargin, .maxYMargin]
        e.addSubview(t)
        let s = NSTextField(labelWithString:
            L("换个关键词或分组试试，也可以直接新建一台。"))
        s.font = .systemFont(ofSize: 12.5)
        s.hbText = "text3"
        s.sizeToFit()
        s.frame.origin = NSPoint(x: cx - s.frame.width / 2, y: cy - 22)
        s.autoresizingMask = [.minXMargin, .maxXMargin,
                              .minYMargin, .maxYMargin]
        e.addSubview(s)
        let b1 = HBtn(L("清除筛选"), sm: true)
        b1.target = self; b1.action = #selector(clearFilters)
        b1.frame = NSRect(x: cx - 92, y: cy - 66, width: 88, height: 28)
        b1.autoresizingMask = [.minXMargin, .maxXMargin,
                               .minYMargin, .maxYMargin]
        e.addSubview(b1)
        let b2 = HBtn("New host", style: .primary, sm: true)
        b2.target = self; b2.action = #selector(newHost)
        b2.frame = NSRect(x: cx + 4, y: cy - 66, width: 88, height: 28)
        b2.autoresizingMask = [.minXMargin, .maxXMargin,
                               .minYMargin, .maxYMargin]
        e.addSubview(b2)
        addSubview(e)
        emptyView = e
        e.isHidden = true
    }

    private func sizeGrid() {
        guard let lay = grid?.collectionViewLayout
            as? NSCollectionViewFlowLayout else { return }
        // 预留竖向滚动条 15px：按 clip 实测宽会在滚动条显隐间来回改列数
        let usable = frame.width - 36 - 15
        let cols = max(1, Int((usable + 12) / 312))
        // -1：整除时 flow layout 的浮点余量会把 N 列挤成 N-1 列
        let w = (usable - CGFloat(cols - 1) * 12) / CGFloat(cols) - 1
        let size = NSSize(width: w, height: 64)
        if lay.itemSize != size { lay.itemSize = size }
    }
    private func sizeColumns() {
        let tw = frame.width - 36 - 15
        let name = max(180, tw * 0.34)
        let mid = max(70, (tw - name - 86) / 4)
        let widths: [CGFloat] = [name, mid, mid, mid, mid, 86]
        for (i, c) in table.tableColumns.enumerated()
            where i < widths.count {
            c.width = widths[i]
        }
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
        guard let ri = filtered.firstIndex(where: { $0.id == id })
        else { return false }
        table.selectRowIndexes(IndexSet(integer: ri),
                               byExtendingSelection: false)
        table.scrollRowToVisible(ri)
        showDrawer(filtered[ri])
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
        var hs = store.hosts.filter { h in
            guard h.match(query) else { return false }
            if chipFilter.isEmpty { return true }
            if chipFilter == "__tun__" {
                return !h.via.isEmpty || h.tunnel == "ssh"
            }
            return h.group == chipFilter
        }
        switch sortKey {
        case "last": hs.sort { $0.lastConnected > $1.lastConnected }
        case "proto": hs.sort {
            ($0.proto, $0.label) < ($1.proto, $1.label) }
        default: hs.sort {
            $0.label.localizedCompare($1.label) == .orderedAscending }
        }
        filtered = hs
        countL.stringValue = "Hosts · \(hs.count)"
        rebuildChips()
        table.reloadData()
        grid.reloadData()
        let empty = hs.isEmpty
        emptyView.isHidden = !empty
        gridScroll.isHidden = !cardMode || empty
        listScroll.isHidden = cardMode || empty
    }

    // MARK: chips（全部 + 分组 + SSH 隧道）
    private func rebuildChips() {
        chipsRow.arrangedSubviews.forEach { $0.removeFromSuperview() }
        var groups: [String] = []
        for h in store.hosts where !h.group.isEmpty
            && !groups.contains(h.group) { groups.append(h.group) }
        chipIds = [""] + groups + ["__tun__"]
        let titles = [L("全部")] + groups + [L("SSH 隧道")]
        for (i, t) in titles.enumerated() {
            mkChip(t, idx: i, on: chipFilter == chipIds[i])
        }
    }
    private func mkChip(_ t: String, idx: Int, on: Bool) {
        let b = NSButton(title: t, target: self,
                         action: #selector(chipHit(_:)))
        b.tag = idx
        b.isBordered = false
        b.wantsLayer = true
        b.layer?.cornerRadius = 13
        b.layer?.borderWidth = 1
        b.layer?.backgroundColor =
            (on ? Theme.blue.color : Theme.card.color).cgColor
        b.layer?.borderColor =
            (on ? Theme.blue.color : Theme.line.color).cgColor
        let font = NSFont.systemFont(ofSize: 12, weight: .semibold)
        b.attributedTitle = NSAttributedString(
            string: t, attributes: [
                .foregroundColor: on ? NSColor.white : Theme.dyn("text2"),
                .font: font])
        b.setAccessibilityIdentifier("chip-\(t)")
        b.translatesAutoresizingMaskIntoConstraints = false
        let w = (t as NSString).size(withAttributes: [.font: font]).width
        b.widthAnchor.constraint(equalToConstant: w + 24).isActive = true
        b.heightAnchor.constraint(equalToConstant: 26).isActive = true
        chipsRow.addArrangedSubview(b)
    }
    @objc private func chipHit(_ s: NSButton) {
        guard chipIds.indices.contains(s.tag) else { return }
        chipFilter = chipIds[s.tag]
        refilter()
    }
    @objc private func clearFilters() {
        query = ""; search.stringValue = ""
        chipFilter = ""
        refilter()
    }

    // MARK: actions
    func controlTextDidChange(_ n: Notification) {
        query = search.stringValue; refilter()
    }
    func controlTextDidBeginEditing(_ n: Notification) {
        quickBar.layer?.borderColor = Theme.blue.color.cgColor
        quickBar.layer?.shadowColor = Theme.blue.color.cgColor
        quickBar.layer?.shadowOpacity = 0.15
        quickBar.layer?.shadowRadius = 3
        quickBar.layer?.shadowOffset = .zero
    }
    func controlTextDidEndEditing(_ n: Notification) {
        quickBar.layer?.borderColor = Theme.line.color.cgColor
        quickBar.layer?.shadowOpacity = 0
    }
    @objc private func openPalette() {
        (window as? MainWindow)?.openPalette()
    }
    @objc private func connectSel() {
        // 搜索框形如 user@host[:port] → 快速连接；否则连选中的
        let s = search.stringValue.trimmingCharacters(in: .whitespaces)
        if s.contains("@") { onQuick?(s); return }
        if let h = selHost() { onConnect?(h) }
    }
    @objc private func serialInfo() {
        NotifyCenter.shared.post(kind: "info",
                                 title: L("串口终端为 P2 功能（FR-14）"),
                                 body: "", tabIndex: nil)
    }
    @objc private func modeHit(_ s: NSButton) { devMode(list: s.tag == 1) }
    func devMode(list: Bool) {
        cardMode = !list
        refreshModeBtns()
        gridScroll.isHidden = !cardMode || filtered.isEmpty
        listScroll.isHidden = cardMode || filtered.isEmpty
        if !list { sizeGrid() } else { sizeColumns() }
    }
    private func refreshModeBtns() {
        for (i, b) in modeBtns.enumerated() {
            let on = (i == 0) == cardMode
            b.layer?.backgroundColor =
                (on ? Theme.btn : Theme.RGBA(0, 0)).color.cgColor
            b.image = hbIcon(i == 0 ? "grid" : "list", size: 14,
                             tint: on ? Theme.dyn("blue")
                                      : Theme.dyn("text2"))
        }
    }
    func tableViewSelectionDidChange(_ n: Notification) {
        let r = table.selectedRow
        if r >= 0, r < filtered.count { showDrawer(filtered[r]) }
    }
    func collectionView(_ c: NSCollectionView,
                        didSelectItemsAt idx: Set<IndexPath>) {
        guard let i = idx.first, i.item < filtered.count else { return }
        showDrawer(filtered[i.item])
    }
    @objc private func doubleClick() {
        let r = table.clickedRow
        guard r >= 0, r < filtered.count else { return }
        onConnect?(filtered[r])
    }
    @objc private func gridDbl(_ g: NSClickGestureRecognizer) {
        let p = g.location(in: grid)
        guard let ip = grid.indexPathForItem(at: p),
              ip.item < filtered.count else { return }
        onConnect?(filtered[ip.item])
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
        if let h = drawerHost { return h }
        if cardMode {
            guard let i = grid.selectionIndexes.first,
                  i < filtered.count else { return nil }
            return filtered[i]
        }
        let r = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow
        guard r >= 0, r < filtered.count else { return nil }
        return filtered[r]
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
        closeDrawer()
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
    @objc private func importMenu(_ s: NSButton) {
        let m = NSMenu()
        m.addItem(withTitle: L("从 ~/.ssh/config 导入…"),
                  action: #selector(importSSHMenu), keyEquivalent: "")
        m.addItem(withTitle: L("导入 JSON…"),
                  action: #selector(importJSONMenu), keyEquivalent: "")
        m.addItem(withTitle: L("导出 JSON…"),
                  action: #selector(exportJSONMenu), keyEquivalent: "")
        m.items.forEach { $0.target = self }
        m.popUp(positioning: nil,
                at: NSPoint(x: 0, y: s.bounds.height + 4), in: s)
    }
    @objc private func importSSHMenu() { importSSHConfig() }
    @objc private func importJSONMenu() { importJSON() }
    @objc private func exportJSONMenu() { exportJSON() }
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
    func numberOfRows(in t: NSTableView) -> Int { filtered.count }
    func tableView(_ t: NSTableView, rowViewForRow row: Int)
        -> NSTableRowView? { HostRowView() }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        guard row < filtered.count, let c else { return nil }
        let h = filtered[row]
        let cw = c.width
        func plain(_ s: String, mono: Bool = false,
                   dim: Bool = false) -> NSTextField {
            let l = NSTextField(labelWithString: s)
            l.font = mono ? .monospacedSystemFont(ofSize: 12.5,
                                                  weight: .regular)
                          : .systemFont(ofSize: 13)
            if dim { l.hbText = "text3" }
            l.frame = NSRect(x: 14, y: 11, width: cw - 28, height: 18)
            l.lineBreakMode = .byTruncatingTail
            return l
        }
        switch c.identifier.rawValue {
        case "name":
            let v = NSView(frame: NSRect(x: 0, y: 0, width: cw,
                                         height: 40))
            let tile = NSView(frame: NSRect(x: 14, y: 9, width: 22,
                                            height: 22))
            tile.wantsLayer = true
            tile.layer?.cornerRadius = 6
            let ot = osTile(h.os)
            tile.layer?.backgroundColor = ot.bg.color.cgColor
            let iv = NSImageView(frame: NSRect(x: 5, y: 5, width: 12,
                                               height: 12))
            iv.image = hbIcon(ot.icon, size: 12, tint: .white)
            tile.addSubview(iv)
            v.addSubview(tile)
            let tf = NSTextField(labelWithString: h.label)
            tf.font = .systemFont(ofSize: 13, weight: .semibold)
            tf.frame = NSRect(x: 44, y: 11, width: cw - 58, height: 18)
            tf.lineBreakMode = .byTruncatingTail
            v.addSubview(tf)
            return v
        case "user":
            return plain(h.user.isEmpty ? "—" : h.user,
                         dim: h.user.isEmpty)
        case "port":
            return plain(h.port > 0 ? "\(h.port)" : "—", mono: true)
        case "proto":
            return plain(h.proto.uppercased())
        case "group":
            if h.group.isEmpty { return plain("—", dim: true) }
            let tag = hbTagLabel(h.group)
            tag.frame.origin = NSPoint(x: 14, y: 11)
            let wrap = NSView(frame: NSRect(x: 0, y: 0, width: cw,
                                            height: 40))
            wrap.addSubview(tag)
            return wrap
        default:
            let wrap = NSView(frame: NSRect(x: 0, y: 0, width: cw,
                                            height: 40))
            let b = HBtn(L("连接"), sm: true)
            b.frame = NSRect(x: 7, y: 7, width: 64, height: 26)
            b.onTap = { [weak self] in self?.onConnect?(h) }
            wrap.addSubview(b)
            return wrap
        }
    }

    // MARK: collection（卡片网格）
    func collectionView(_ c: NSCollectionView,
                        numberOfItemsInSection s: Int) -> Int {
        filtered.count
    }
    func collectionView(_ c: NSCollectionView, itemForRepresentedObjectAt
                        p: IndexPath) -> NSCollectionViewItem {
        let it = c.makeItem(withIdentifier: .init("card"), for: p)
        if let ci = it as? HostCardItem {
            ci.host = filtered[p.item]
            ci.onGo = { [weak self] h in self?.onConnect?(h) }
        }
        return it
    }

    // MARK: 详情抽屉（400px + scrim，右滑入；覆盖整窗，对齐 prototype）
    private func showDrawer(_ h: HostRec) {
        closeDrawer()
        guard let cv = window?.contentView else { return }
        drawerHost = h
        let sc = NSView(frame: cv.bounds)
        sc.autoresizingMask = [.width, .height]
        sc.wantsLayer = true
        sc.layer?.backgroundColor =
            Theme.RGBA(0x0f1626, 0.32).color.cgColor
        let tap = NSClickGestureRecognizer(target: self,
                                           action: #selector(closeDrawer))
        sc.addGestureRecognizer(tap)
        cv.addSubview(sc)
        scrim = sc
        let d = HostDrawer(host: h, store: store,
                           frame: NSRect(x: cv.bounds.width, y: 0,
                                         width: 400, height: cv.bounds.height))
        d.autoresizingMask = [.minXMargin, .height]
        d.onClose = { [weak self] in self?.closeDrawer() }
        d.onConnect = { [weak self] hh in
            self?.closeDrawer(); self?.onConnect?(hh)
        }
        d.onEdit = { [weak self] hh in self?.editHost(hh) }
        d.onDup = { [weak self] hh in
            guard let self else { return }
            var c = hh; c.id = ""; c.name += L(" 副本")
            _ = self.store.apply(c)
            self.closeDrawer(); self.reload()
        }
        d.onSftp = { [weak self] hh in self?.onSftp?(hh.id) }
        d.onDel = { [weak self] hh in
            guard let self else { return }
            self.drawerHost = hh
            self.ctxDel()
        }
        cv.addSubview(d)
        drawer = d
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.22
            ctx.timingFunction = CAMediaTimingFunction(name: .easeOut)
            d.animator().setFrameOrigin(
                NSPoint(x: cv.bounds.width - 400, y: 0))
        }
    }
    @objc func closeDrawer() {
        scrim?.removeFromSuperview(); scrim = nil
        drawer?.removeFromSuperview(); drawer = nil
        drawerHost = nil
    }
    override func removeFromSuperview() {
        closeDrawer()
        super.removeFromSuperview()
    }

    // MARK: 新建/编辑表单
    func editHost(_ h: HostRec?) {
        guard let w = window else { return }
        let sheet = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 640),
                             styleMask: [.borderless], backing: .buffered, defer: false)
        sheet.isOpaque = false
        sheet.backgroundColor = .clear
        sheet.hasShadow = true
        sheet.isReleasedWhenClosed = false
        let form = HostForm(frame: NSRect(x: 0, y: 0, width: 520, height: 640),
                            title: h == nil ? "New host" : L("编辑主机"))
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

// MARK: - 主机卡片

/// 命中穿透的装饰视图：点击落到父卡片上。
final class PassView: NSView {
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

/// 主机卡片（NSCollectionViewItem，复用）。
final class HostCardItem: NSCollectionViewItem {
    var host: HostRec? {
        didSet { (view as? HostCard)?.host = host }
    }
    var onGo: ((HostRec) -> Void)? {
        didSet { (view as? HostCard)?.onGo = onGo }
    }
    override func loadView() { view = HostCard() }
    override var isSelected: Bool {
        didSet { (view as? HostCard)?.selected = isSelected }
    }
}

/// prototype .host-card：38px os-ico + 名称(内联 tag) + 副行 + card-go。
final class HostCard: NSView {
    var selected = false { didSet { updateStyle() } }
    var host: HostRec? { didSet { fill() } }
    var onGo: ((HostRec) -> Void)?
    private let tile = PassView()
    private let tileImg = NSImageView()
    private let nameL = NSTextField(labelWithString: "")
    private let subL = NSTextField(labelWithString: "")
    private var tags: [NSTextField] = []
    private let go = PassView()
    private let goImg = NSImageView()
    private var hovered = false
    private var goHover = false

    override var isFlipped: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.borderWidth = 1
        layer?.hbBg = "card"
        layer?.shadowColor = Theme.RGBA(0x19233c).color.cgColor
        updateStyle()
        tile.wantsLayer = true
        tile.layer?.cornerRadius = 10
        tile.frame = NSRect(x: 15, y: 13, width: 38, height: 38)
        tileImg.frame = NSRect(x: 9, y: 9, width: 20, height: 20)
        tile.addSubview(tileImg)
        addSubview(tile)
        nameL.font = .systemFont(ofSize: 13.5, weight: .semibold)
        nameL.lineBreakMode = .byTruncatingTail
        addSubview(nameL)
        subL.font = .systemFont(ofSize: 12)
        subL.hbText = "text3"
        subL.lineBreakMode = .byTruncatingTail
        addSubview(subL)
        go.wantsLayer = true
        go.layer?.cornerRadius = 8
        goImg.frame = NSRect(x: 8.5, y: 8.5, width: 13, height: 13)
        go.addSubview(goImg)
        addSubview(go)
        updateGo()
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited, .mouseMoved,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        layoutCard()
    }
    required init?(coder: NSCoder) { fatalError() }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        layoutCard()
    }
    private func layoutCard() {
        let w = bounds.width
        go.frame = NSRect(x: w - 45, y: 17, width: 30, height: 30)
        let textW = max(40, w - 65 - 55)
        nameL.frame = NSRect(x: 65, y: 14, width: textW, height: 18)
        subL.frame = NSRect(x: 65, y: 34, width: textW, height: 16)
        layoutTags()
    }
    private func layoutTags() {
        let nameW = (nameL.stringValue as NSString).size(
            withAttributes: [.font: nameL.font!]).width
        var x = 65 + min(nameW, nameL.frame.width) + 7
        for t in tags {
            t.isHidden = x + t.frame.width > go.frame.minX - 8
            t.frame.origin = NSPoint(x: x, y: 14)
            x += t.frame.width + 7
        }
    }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; updateStyle()
        mouseMoved(with: e)
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; updateStyle()
        if goHover { goHover = false; updateGo() }
    }
    override func mouseMoved(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        let g = go.frame.contains(p)
        if g != goHover { goHover = g; updateGo() }
    }
    override func mouseDown(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        if go.frame.contains(p), let h = host {
            onGo?(h); return
        }
        super.mouseDown(with: e)
    }
    private func updateStyle() {
        let hot = hovered || selected
        layer?.hbBorderResolver = { [weak self] in
            (self?.hovered == true || self?.selected == true
                ? Theme.cardHover : Theme.RGBA(0, 0)).color.cgColor
        }
        layer?.shadowOpacity = hot ? 0.09 : 0.05
        layer?.shadowRadius = hot ? 14 : 2
        layer?.shadowOffset = CGSize(width: 0, height: hot ? 4 : 1)
    }
    private func updateGo() {
        go.layer?.backgroundColor =
            (goHover ? Theme.blue : Theme.chip).color.cgColor
        goImg.image = hbIcon("plug", size: 13,
                             tint: goHover ? .white
                                           : Theme.dyn("text3"))
    }
    private func fill() {
        guard let h = host else { return }
        let t = osTile(h.os)
        tile.layer?.backgroundColor = t.bg.color.cgColor
        tileImg.image = hbIcon(t.icon, size: 20, tint: .white)
        nameL.stringValue = h.label
        var sub = "\(h.proto), \(h.user.isEmpty ? "—" : h.user)"
        if !h.group.isEmpty { sub += " · \(h.group)" }
        subL.stringValue = sub
        tags.forEach { $0.removeFromSuperview() }
        tags = []
        if !h.via.isEmpty || h.tunnel == "ssh" {
            let tg = hbTagLabel(L("SSH 隧道"), green: true)
            addSubview(tg); tags.append(tg)
        }
        if h.proto == "local" {
            let tg = hbTagLabel(L("本地"))
            addSubview(tg); tags.append(tg)
        }
        layoutCard()
    }
}

// MARK: - 详情抽屉

/// prototype .modal.drawer：400px 全高，头部 os-ico+标题+×，
/// dr-row 键值行，底部 连接/编辑/复制/SFTP/删除。
final class HostDrawer: NSView {
    var onClose: (() -> Void)?
    var onConnect: ((HostRec) -> Void)?
    var onEdit: ((HostRec) -> Void)?
    var onDup: ((HostRec) -> Void)?
    var onSftp: ((HostRec) -> Void)?
    var onDel: ((HostRec) -> Void)?
    private let host: HostRec

    override var isFlipped: Bool { true }

    init(host: HostRec, store: HostStore, frame: NSRect) {
        self.host = host
        super.init(frame: frame)
        wantsLayer = true
        layer?.hbBg = "card"
        layer?.cornerRadius = 14
        layer?.maskedCorners = [.layerMinXMinYCorner, .layerMinXMaxYCorner]
        layer?.shadowColor = NSColor.black.cgColor
        layer?.shadowOpacity = 0.18
        layer?.shadowRadius = 24
        layer?.shadowOffset = CGSize(width: -6, height: 0)
        let W = frame.width, H = frame.height

        // head
        let tile = NSView(frame: NSRect(x: 18, y: 16, width: 32, height: 32))
        tile.wantsLayer = true
        tile.layer?.cornerRadius = 9
        let ot = osTile(host.os)
        tile.layer?.backgroundColor = ot.bg.color.cgColor
        let iv = NSImageView(frame: NSRect(x: 8, y: 8, width: 16, height: 16))
        iv.image = hbIcon(ot.icon, size: 16, tint: .white)
        tile.addSubview(iv)
        addSubview(tile)
        let ttl = NSTextField(labelWithString: host.label)
        ttl.font = .boldSystemFont(ofSize: 15)
        ttl.frame = NSRect(x: 60, y: 22, width: W - 110, height: 20)
        ttl.lineBreakMode = .byTruncatingTail
        addSubview(ttl)
        let x = NSButton(title: "", target: self, action: #selector(hitClose))
        x.isBordered = false
        x.image = hbIcon("x", size: 13, tint: Theme.dyn("text2"))
        x.frame = NSRect(x: W - 42, y: 19, width: 26, height: 26)
        x.autoresizingMask = [.minXMargin]
        addSubview(x)
        sep(62)

        // tags
        var tx: CGFloat = 18
        func addTag(_ t: String, green: Bool = false) {
            let tg = hbTagLabel(t, green: green)
            tg.frame.origin = NSPoint(x: tx, y: 76)
            addSubview(tg)
            tx += tg.frame.width + 6
        }
        if !host.via.isEmpty || host.tunnel == "ssh" {
            addTag(L("SSH 隧道"), green: true)
        }
        if !host.group.isEmpty { addTag(host.group) }
        if host.proto == "local" { addTag(L("本地")) }
        addTag(L("E2EE 已同步"))

        // rows
        var y: CGFloat = 108
        func row(_ k: String, _ v: String) {
            let kl = NSTextField(labelWithString: k)
            kl.font = .systemFont(ofSize: 12.5)
            kl.hbText = "text3"
            kl.frame = NSRect(x: 18, y: y + 9, width: 104, height: 16)
            addSubview(kl)
            let vl = NSTextField(labelWithString: v)
            vl.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
            vl.alignment = .right
            vl.lineBreakMode = .byTruncatingMiddle
            vl.frame = NSRect(x: 130, y: y + 9, width: W - 148, height: 16)
            vl.autoresizingMask = [.width]
            addSubview(vl)
            let s = NSView(frame: NSRect(x: 18, y: y + 33,
                                         width: W - 36, height: 1))
            s.autoresizingMask = [.width]
            s.wantsLayer = true
            s.layer?.hbBg = "hover"
            addSubview(s)
            y += 34
        }
        row(L("协议"), host.proto.uppercased())
        row(L("地址"), host.address.isEmpty ? host.label : host.address)
        row(L("端口"), host.port > 0 ? "\(host.port)" : "—")
        row(L("用户名"), host.user.isEmpty ? "—" : host.user)
        row(L("认证"), host.auth)
        row(L("分组"), host.group.isEmpty ? L("未分组") : host.group)
        var jump = L("直连")
        if !host.via.isEmpty,
           let via = store.hosts.first(where: { $0.id == host.via }) {
            jump = "\(via.user.isEmpty ? "" : via.user + "@")\(via.label)"
        } else if !host.jumps.isEmpty {
            jump = String(format: L("%ld 个跳板"), host.jumps.count)
        }
        row(L("跳板 / 隧道"), jump)
        if host.lastConnected > 0 {
            let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH:mm"
            row(L("最近连接"), f.string(from: Date(
                timeIntervalSince1970:
                    TimeInterval(host.lastConnected) / 1000)))
        } else {
            row(L("最近连接"), L("尚未连接"))
        }

        // hintbox（ssh）
        if host.proto == "ssh" || host.proto.isEmpty {
            let hb = NSView(frame: NSRect(x: 18, y: y + 10,
                                          width: W - 36, height: 52))
            hb.autoresizingMask = [.width]
            hb.wantsLayer = true
            hb.layer?.cornerRadius = 9
            hb.layer?.borderWidth = 1
            hb.layer?.hbBg = "hintBlueBg"
            hb.layer?.hbBorder = "hintBlueLine"
            let hl = NSTextField(wrappingLabelWithString:
                L("凭据变更 5s 内同步到其它设备；主机指纹详见 Known Hosts。"))
            hl.font = .systemFont(ofSize: 12)
            hl.hbText = "hintBlueFg"
            hl.frame = NSRect(x: 13, y: 8, width: W - 62, height: 36)
            hl.autoresizingMask = [.width]
            hb.addSubview(hl)
            addSubview(hb)
        }

        // foot
        let fs = NSView(frame: NSRect(x: 0, y: H - 61, width: W, height: 1))
        fs.autoresizingMask = [.width, .minYMargin]
        fs.wantsLayer = true
        fs.layer?.hbBg = "line"
        addSubview(fs)
        func foot(_ b: NSButton, _ x: CGFloat, _ w: CGFloat,
                  _ aid: String, _ act: Selector) {
            b.target = self; b.action = act
            b.frame = NSRect(x: x, y: H - 47, width: w, height: 32)
            b.autoresizingMask = [.minYMargin]
            b.setAccessibilityIdentifier(aid)
            addSubview(b)
        }
        foot(HBtn(L("连接"), style: .primary, icon: "plug"),
             18, 88, "drawer-connect", #selector(hitConnect))
        foot(HBtn(L("编辑")), 114, 64, "drawer-edit", #selector(hitEdit))
        foot(HBtn(L("复制")), 186, 64, "drawer-dup", #selector(hitDup))
        foot(HBtn("SFTP"), 258, 64, "drawer-sftp", #selector(hitSftp))
        let del = HBtn("", style: .danger, icon: "trash")
        foot(del, W - 54, 36, "drawer-del", #selector(hitDel))
        del.autoresizingMask = [.minXMargin, .minYMargin]
    }
    required init?(coder: NSCoder) { fatalError() }
    private func sep(_ y: CGFloat) {
        let s = NSView(frame: NSRect(x: 0, y: y,
                                     width: bounds.width, height: 1))
        s.autoresizingMask = [.width]
        s.wantsLayer = true
        s.layer?.hbBg = "line"
        addSubview(s)
    }
    @objc private func hitClose() { onClose?() }
    @objc private func hitConnect() { onConnect?(host) }
    @objc private func hitEdit() { onEdit?(host) }
    @objc private func hitDup() { onDup?(host) }
    @objc private func hitSftp() { onSftp?(host) }
    @objc private func hitDel() { onDel?(host) }
}