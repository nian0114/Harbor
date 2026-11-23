import AppKit
import UniformTypeIdentifiers
import CHarbor
import HarborKit

/// FR-10 Vault「日志」页 —— 对齐 linux/src/hb-logs-view.c：
/// 过滤（user/host/proto/result 子串）、仅看 Saved、日期排序、
/// retention（log_retention_days 默认 90，0=永久）、CSV/JSON 导出、
/// 清空；行 = Date/User/Host/Proto/Result pill/Reason/Duration/Saved。
final class LogsPage: NSView, NSTableViewDataSource, NSTableViewDelegate {
    private let table = NSTableView()
    private var search: NSSearchField!
    private var savedBtn: NSButton!
    private var sortBtn: NSButton!
    private var retPop: NSPopUpButton!
    private var rows: [LogRow] = []
    private var desc = true
    private var onlySaved = false
    private var vaultObs: NSObjectProtocol?
    private let colIDs = ["date", "user", "host", "proto",
                          "result", "reason", "dur", "saved"]

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
        layer?.hbBg = "page"
        let titles = [L("Date"), L("User"), L("Host"), L("协议"),
                      L("结果"), L("原因"), L("时长"), "Saved"]
        let widths: [CGFloat] = [130, 90, 150, 56, 70, 130, 70, 56]
        for (i, cid) in colIDs.enumerated() {
            let c = NSTableColumn(identifier: .init(cid))
            c.title = titles[i]
            c.width = widths[i]
            table.addTableColumn(c)
        }
        table.dataSource = self
        table.delegate = self
        table.rowHeight = 26
        table.intercellSpacing = NSSize(width: 0, height: 2)
        table.backgroundColor = .clear
        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.frame = NSRect(x: 0, y: 0, width: f.width,
                              height: f.height - 46)
        scroll.autoresizingMask = [.width, .height]
        addSubview(scroll)
        buildBar()
        reload()
    }
    required init?(coder: NSCoder) { fatalError() }

    private func buildBar() {
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
        savedBtn = btn(L("仅看 Saved"), #selector(savedToggled(_:)),
                       "logs-saved", w: 92)
        savedBtn.setButtonType(.pushOnPushOff)
        sortBtn = btn(L("日期 ↓"), #selector(sortToggled), "logs-sort", w: 74)
        retPop = NSPopUpButton(frame: NSRect(
            x: x, y: bounds.height - 38, width: 96, height: 26))
        for (t, _) in Self.retChoices { retPop.addItem(withTitle: t) }
        retPop.target = self
        retPop.action = #selector(retChanged(_:))
        retPop.autoresizingMask = [.minYMargin]
        retPop.setAccessibilityIdentifier("logs-retention")
        addSubview(retPop)
        x += 102
        _ = btn(L("导出 CSV"), #selector(exportCSV), "logs-csv", w: 72)
        _ = btn(L("导出 JSON"), #selector(exportJSON), "logs-json", w: 80)
        _ = btn(L("清空"), #selector(clearAll), "logs-clear", w: 56)
        search = NSSearchField(frame: NSRect(
            x: bounds.width - 234, y: bounds.height - 39,
            width: 220, height: 26))
        search.placeholderString = L("按主机 / 用户 / 结果筛选…")
        search.target = self
        search.action = #selector(searchChanged)
        (search.cell as? NSSearchFieldCell)?.sendsSearchStringImmediately = true
        search.autoresizingMask = [.minXMargin, .minYMargin]
        search.setAccessibilityIdentifier("logs-search")
        addSubview(search)
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
        table.reloadData()
    }

    // MARK: 行
    func numberOfRows(in t: NSTableView) -> Int { rows.count }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let e = rows[row]
        guard let cid = c?.identifier.rawValue else { return nil }
        if cid == "saved" {
            let b = NSButton()
            b.isBordered = false
            b.image = hbIcon("bookmark", size: 13,
                             tint: e.saved ? Theme.blue.color
                                           : Theme.termDim.color)
            b.target = self
            b.action = #selector(toggleSaved(_:))
            b.tag = row
            b.setAccessibilityIdentifier("log-saved")
            return b
        }
        if cid == "result" {
            let (txt, cls) = LogFormat.result(e.result, durationS: e.dur)
            // linux pill：居中、圆角、着色文字 + 淡底
            let holder = NSView()
            let pill = NSTextField(labelWithString: " " + txt + " ")
            pill.font = .systemFont(ofSize: 10.5, weight: .medium)
            pill.alignment = .center
            pill.wantsLayer = true
            pill.layer?.cornerRadius = 8
            let (bg, fg) = pillColors(cls)
            pill.layer?.backgroundColor = bg.cgColor
            pill.textColor = fg
            pill.translatesAutoresizingMaskIntoConstraints = false
            holder.addSubview(pill)
            NSLayoutConstraint.activate([
                pill.centerXAnchor.constraint(equalTo: holder.centerXAnchor),
                pill.centerYAnchor.constraint(equalTo: holder.centerYAnchor),
                pill.heightAnchor.constraint(equalToConstant: 17),
            ])
            return holder
        }
        let l = NSTextField(labelWithString: "")
        l.lineBreakMode = .byTruncatingTail
        switch cid {
        case "date":
            l.stringValue = LogFormat.date(e.ts)
            l.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        case "user":   l.stringValue = e.user
        case "host":
            l.stringValue = e.host
            l.font = .systemFont(ofSize: 12, weight: .medium)
        case "proto":  l.stringValue = e.proto
        case "reason":
            l.stringValue = LogFormat.reason(e.reason, result: e.result)
        case "dur":    l.stringValue = LogFormat.duration(e.dur)
        default: break
        }
        return l
    }
    private func pillColors(_ cls: String) -> (NSColor, NSColor) {
        switch cls {
        case "on":   return (Theme.greenPillBg.color, Theme.greenPillFg.color)
        case "err":  return (Theme.pillErrBg.color, Theme.pillErrFg.color)
        case "wait": return (Theme.pillWaitBg.color, Theme.pillWaitFg.color)
        case "off":  return (Theme.chip.color, .secondaryLabelColor)
        default:     return (Theme.pillRunBg.color, Theme.pillRunFg.color)
        }
    }

    // MARK: 动作
    @objc private func searchChanged() { reload() }
    @objc private func savedToggled(_ b: NSButton) {
        onlySaved = b.state == .on
        reload()
    }
    @objc private func sortToggled() {
        desc.toggle()
        sortBtn.title = desc ? L("日期 ↓") : L("日期 ↑")
        reload()
    }
    @objc private func toggleSaved(_ b: NSButton) {
        guard let v = Vault.shared.vault else { return }
        let e = rows[b.tag]
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
    @objc private func clearAll() {
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
    @objc private func exportCSV() { export(json: false, path: nil) }
    @objc private func exportJSON() { export(json: true, path: nil) }
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
