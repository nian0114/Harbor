import AppKit
import UniformTypeIdentifiers
import CHarbor
import HarborKit

/// FR-10 Vault「日志」页 —— 对齐 prototype logsView：
/// toolbar（左筛选 field + 右侧导出 CSV），白卡片表格 =
/// Date(可点排序) | User | Host | 协议 | 结果 pill | 时长 |
/// Saved 书签（琥珀）。保留 mac 侧 仅看 Saved/retention/JSON/清空。
final class LogsPage: NSView, NSTableViewDataSource, NSTableViewDelegate {
    private let card = VTableCard()
    private var search: VField!
    private var savedBtn: HBtn!
    private var retPop: NSPopUpButton!
    private var rows: [LogRow] = []
    private var desc = true
    private var onlySaved = false
    private var vaultObs: NSObjectProtocol?
    private let colIDs = ["date", "user", "host", "proto",
                          "result", "dur", "saved"]

    private struct LogRow {
        let id: Int64, ts: Int64
        let user, host, proto, result, reason: String
        let dur: Int64
        let saved: Bool
    }
    /// retention 选项（与 Linux RET_CHOICES 一致）。
    private static let retChoices: [(String, Int)] = [
        (L("30 天"), 30), (L("90 天"), 90), (L("180 天"), 180),
        (L("365 天"), 365), (L("永久"), 0),
    ]

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.hbBg = "content"
        buildBar()
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.wantsLayer = true
        scroll.layer?.hbBg = "content"
        scroll.frame = NSRect(x: 0, y: 0, width: f.width,
                              height: f.height - 56)
        scroll.autoresizingMask = [.width, .height]
        let box = VFlippedStack()
        box.orientation = .vertical
        box.spacing = 14
        box.alignment = .centerX
        box.edgeInsets = NSEdgeInsets(top: 4, left: 18,
                                      bottom: 20, right: 18)
        box.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = box
        box.widthAnchor.constraint(
            equalTo: scroll.contentView.widthAnchor).isActive = true
        addSubview(scroll)
        card.translatesAutoresizingMaskIntoConstraints = false
        box.addArrangedSubview(card)
        card.widthAnchor.constraint(
            equalTo: box.widthAnchor, constant: -36).isActive = true
        let titles = ["Date ↓", L("User"), L("Host"), L("协议"),
                      L("结果"), L("时长"), "Saved"]
        for (i, cid) in colIDs.enumerated() {
            let c = NSTableColumn(identifier: .init(cid))
            c.title = titles[i]
            c.headerCell = TableHeadCell(textCell: titles[i])
            c.resizingMask = []
            if cid == "host" {
                c.minWidth = 140
            } else {
                c.width = [150, 90, 0, 70, 90, 80, 60][i]
                c.minWidth = c.width
                c.maxWidth = c.width
            }
            card.table.addTableColumn(c)
        }
        let hv = LogsHeaderView(frame: NSRect(x: 0, y: 0,
                                              width: 600, height: 37))
        hv.onSort = { [weak self] in self?.sortToggled() }
        hv.setAccessibilityIdentifier("logs-sort")
        card.table.headerView = hv
        card.table.dataSource = self
        card.table.delegate = self
        card.table.setAccessibilityIdentifier("logs-table")
        reload()
        card.sizeColumns = { [weak self] width in
            self?.card.table.tableColumns[2].width = max(140, width - 540)
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        search.frame.size.width = min(320, max(24, savedBtn.frame.minX - 26))
    }

    private func buildBar() {
        search = VField(placeholder: L("按主机 / 用户 / 结果筛选…"),
                        width: 320)
        search.frame.origin = NSPoint(x: 18, y: bounds.height - 45)
        search.autoresizingMask = [.minYMargin]
        search.onChange = { [weak self] in self?.reload() }
        search.entry.setAccessibilityIdentifier("logs-search")
        addSubview(search)
        // 右侧按钮组（从右往左排）
        var rx = bounds.width - 18
        func placeRight(_ v: NSView, w: CGFloat, h: CGFloat = 33) {
            rx -= w
            v.frame = NSRect(x: rx, y: bounds.height - 45 + (33 - h) / 2,
                             width: w, height: h)
            v.autoresizingMask = [.minXMargin, .minYMargin]
            addSubview(v)
            rx -= 8
        }
        let clear = HBtn(L("清空"), style: .ghost)
        clear.onTap = { [weak self] in self?.clearAll() }
        clear.setAccessibilityIdentifier("logs-clear")
        placeRight(clear, w: clear.intrinsicContentSize.width + 28)
        let json = HBtn(L("导出 JSON"), style: .ghost)
        json.onTap = { [weak self] in self?.exportJSON() }
        json.setAccessibilityIdentifier("logs-json")
        placeRight(json, w: json.intrinsicContentSize.width + 28)
        let csv = HBtn(L("导出 CSV"), icon: "bookmark")
        csv.onTap = { [weak self] in self?.exportCSV() }
        csv.setAccessibilityIdentifier("logs-csv")
        placeRight(csv, w: csv.intrinsicContentSize.width + 28)
        retPop = NSPopUpButton(frame: .zero)
        for (t, _) in Self.retChoices { retPop.addItem(withTitle: t) }
        retPop.target = self
        retPop.action = #selector(retChanged(_:))
        retPop.setAccessibilityIdentifier("logs-retention")
        placeRight(retPop, w: 96, h: 26)
        savedBtn = HBtn(L("仅看 Saved"), style: .ghost)
        savedBtn.onTap = { [weak self] in self?.savedToggled() }
        savedBtn.setAccessibilityIdentifier("logs-saved")
        placeRight(savedBtn, w: savedBtn.intrinsicContentSize.width + 28)
        // retention 初值（settings.log_retention_days，默认 90）
        let days = Int(Vault.shared.settingsInt("log_retention_days", 90))
        retPop.selectItem(at: Self.retChoices.firstIndex {
            $0.1 == days } ?? 1)
        vaultObs = NotificationCenter.default.addObserver(
            forName: .hbVaultChanged, object: nil, queue: .main)
        { [weak self] n in
            guard (n.userInfo?["type"] as? String) == "log" else { return }
            self?.reload()
        }
    }

    deinit {
        if let o = vaultObs { NotificationCenter.default.removeObserver(o) }
    }

    // MARK: 数据
    func reload() {
        rows = []
        guard let v = Vault.shared.vault else { return }
        let cb: hb_log_cb = { l, ud in
            guard let l, let ud else { return true }
            let p = Unmanaged<LogsPage>.fromOpaque(ud)
                .takeUnretainedValue()
            p.rows.append(LogRow(
                id: l.pointee.id, ts: l.pointee.ts,
                user: l.pointee.user.map { String(cString: $0) } ?? "",
                host: l.pointee.host.map { String(cString: $0) } ?? "",
                proto: l.pointee.proto.map { String(cString: $0) } ?? "",
                result: l.pointee.result.map { String(cString: $0) } ?? "",
                reason: l.pointee.reason.map { String(cString: $0) } ?? "",
                dur: l.pointee.duration_s, saved: l.pointee.saved))
            return true
        }
        let q = search?.stringValue ?? ""
        let qq: String? = q.isEmpty ? nil : q
        (qq ?? "").withCString { qs in
            _ = hb_vault_log_foreach(v, qq == nil ? nil : qs, desc,
                                     cb, Unmanaged.passUnretained(self)
                                        .toOpaque())
        }
        if onlySaved { rows = rows.filter { $0.saved } }
        card.table.reloadData()
        card.setRows(rows.count, emptyText: L("无匹配记录"))
    }

    // MARK: 行
    func numberOfRows(in t: NSTableView) -> Int { rows.count }
    func tableView(_ t: NSTableView, rowViewForRow row: Int)
        -> NSTableRowView? { VTableRowView() }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let e = rows[row]
        guard let cid = c?.identifier.rawValue else { return nil }
        switch cid {
        case "saved":
            let wrap = NSView()
            let b = VIconBtn(icon: "bookmark", kind: .bm)
            b.bmOn = e.saved
            b.toolTip = e.saved ? L("取消 Saved") : L("标记 Saved")
            b.onTap = { [weak self] in self?.toggleSaved(id: e.id) }
            b.setAccessibilityIdentifier("log-saved")
            b.translatesAutoresizingMaskIntoConstraints = false
            wrap.addSubview(b)
            NSLayoutConstraint.activate([
                b.leadingAnchor.constraint(equalTo: wrap.leadingAnchor),
                b.centerYAnchor.constraint(equalTo: wrap.centerYAnchor),
            ])
            return wrap
        case "result":
            let (txt, cls) = LogFormat.result(e.result, durationS: e.dur)
            let kind: VPill.Kind
            switch cls {
            case "on": kind = .on
            case "err": kind = .err
            case "wait": kind = .wait
            case "off": kind = .off
            default: kind = .run
            }
            let wrap = NSView()
            let pill = VPill(txt, kind: kind, dot: false)
            pill.toolTip = LogFormat.reason(e.reason, result: e.result)
            pill.translatesAutoresizingMaskIntoConstraints = false
            wrap.addSubview(pill)
            NSLayoutConstraint.activate([
                pill.leadingAnchor.constraint(
                    equalTo: wrap.leadingAnchor),
                pill.centerYAnchor.constraint(
                    equalTo: wrap.centerYAnchor),
            ])
            return wrap
        case "date":
            return vLabel(LogFormat.date(e.ts), mono: true)
        case "user":
            return vLabel(e.user)
        case "host":
            return vLabel(e.host, bold: true)
        case "proto":
            return vLabel(e.proto)
        default:
            return vLabel(LogFormat.duration(e.dur))
        }
    }

    // MARK: 动作
    private func savedToggled() {
        onlySaved.toggle()
        savedBtn.style = onlySaved ? .gray : .ghost
        reload()
    }
    private func sortToggled() {
        desc.toggle()
        card.table.tableColumns[0].headerCell.stringValue =
            desc ? "Date ↓" : "Date ↑"
        card.table.headerView?.needsDisplay = true
        reload()
    }
    private func toggleSaved(id: Int64) {
        guard let v = Vault.shared.vault,
              let e = rows.first(where: { $0.id == id }) else { return }
        _ = hb_vault_log_set_saved(v, e.id, !e.saved)
        reload()
    }
    @objc private func retChanged(_ p: NSPopUpButton) {
        let i = p.indexOfSelectedItem
        guard i >= 0, i < Self.retChoices.count else { return }
        let days = Self.retChoices[i].1
        Vault.shared.setSettingsInt("log_retention_days", Int64(days))
        if let v = Vault.shared.vault {
            _ = hb_vault_log_purge(v, Int32(days))
        }
        reload()
    }
    private func clearAll() {
        let a = NSAlert()
        a.messageText = L("清空连接日志")
        a.informativeText = L("确定删除全部连接日志？此操作不可撤销。")
        a.addButton(withTitle: L("清空"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: "logs-clear",
                                      in: window) { [weak self] ok in
            guard ok, let v = Vault.shared.vault else { return }
            _ = hb_vault_log_clear(v)
            self?.reload()
        }
    }
    private func exportCSV() { export(json: false, path: nil) }
    private func exportJSON() { export(json: true, path: nil) }
    func export(json: Bool, path: String?) {
        guard let v = Vault.shared.vault else { return }
        var out: UnsafeMutablePointer<CChar>?
        let ok = json ? hb_vault_log_export_json(v, &out)
                      : hb_vault_log_export_csv(v, &out)
        guard ok == HB_OK, let out else { return }
        let txt = String(cString: out)
        free(out)
        if let path {
            try? txt.write(toFile: path, atomically: true,
                           encoding: .utf8)
            return
        }
        let sp = NSSavePanel()
        sp.nameFieldStringValue = json ? "harbor-logs.json"
                                       : "harbor-logs.csv"
        sp.allowedContentTypes = json ? [.json] : [.commaSeparatedText]
        guard let w = window else { return }
        sp.beginSheetModal(for: w) { r in
            guard r == .OK, let url = sp.url else { return }
            try? txt.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    // MARK: dev
    var devState: String {
        var arr: [[String: Any]] = []
        for e in rows {
            arr.append([
                "id": e.id, "ts": e.ts, "user": e.user, "host": e.host,
                "proto": e.proto, "result": e.result, "reason": e.reason,
                "dur": e.dur, "saved": e.saved])
        }
        let d = try? JSONSerialization.data(withJSONObject: arr)
        return d.flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
    }
}

/// Date 表头可点排序（prototype .th-sort）。
private final class LogsHeaderView: TableHeaderView {
    var onSort: (() -> Void)?
    override func mouseDown(with e: NSEvent) {
        let pt = convert(e.locationInWindow, from: nil)
        if headerRect(ofColumn: 0).contains(pt) {
            onSort?()
        } else {
            super.mouseDown(with: e)
        }
    }
    override func accessibilityPerformPress() -> Bool {
        onSort?()
        return true
    }
}
