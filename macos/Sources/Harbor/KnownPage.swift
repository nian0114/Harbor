import AppKit
import CHarbor
import HarborKit

/// FR-09 Vault「已知主机」页 —— 对齐 linux/src/hb-known-view.c：
/// 导入（默认 ~/.ssh/known_hosts 或选文件）、导出、写回、
/// host/port/keytype/SHA256 指纹/添加时间/删除(确认+撤销)、双击详情。
final class KnownPage: NSView, NSTableViewDataSource, NSTableViewDelegate {
    private let table = NSTableView()
    private var search: NSSearchField!
    private var items: [OpaquePointer] = []     // "known" obj
    private var filtered: [OpaquePointer]?
    private var vaultObs: NSObjectProtocol?
    private let colIDs = ["host", "port", "keytype", "fp", "added", "del"]

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.hbBg = "page"
        let titles = [L("主机"), L("端口"), L("密钥类型"),
                      L("指纹 (SHA256)"), L("添加时间"), ""]
        let widths: [CGFloat] = [210, 50, 100, 230, 100, 50]
        for (i, cid) in colIDs.enumerated() {
            let c = NSTableColumn(identifier: .init(cid))
            c.title = titles[i]
            c.width = widths[i]
            table.addTableColumn(c)
        }
        table.dataSource = self
        table.delegate = self
        table.rowHeight = 28
        table.intercellSpacing = NSSize(width: 0, height: 2)
        table.backgroundColor = .clear
        table.target = self
        table.doubleAction = #selector(rowDetails)
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
        vaultObs = NotificationCenter.default.addObserver(
            forName: .hbVaultChanged, object: nil, queue: .main)
        { [weak self] n in
            guard (n.userInfo?["type"] as? String) == "known"
            else { return }
            self?.reload()
        }
    }

    deinit {
        if let o = vaultObs { NotificationCenter.default.removeObserver(o) }
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
        _ = btn(L("导入 known_hosts"), #selector(importBtn), "known-import")
        _ = btn(L("导出"), #selector(exportBtn), "known-export", w: 60)
        _ = btn(L("写回 ~/.ssh/known_hosts"), #selector(writeBackBtn),
                "known-write")
        search = NSSearchField(frame: NSRect(
            x: bounds.width - 234, y: bounds.height - 39,
            width: 220, height: 26))
        search.placeholderString = L("搜索主机 / 指纹…")
        search.target = self
        search.action = #selector(searchChanged)
        (search.cell as? NSSearchFieldCell)?.sendsSearchStringImmediately = true
        search.autoresizingMask = [.minXMargin, .minYMargin]
        search.setAccessibilityIdentifier("known-search")
        addSubview(search)
    }

    // MARK: 数据
    func reload() {
        for o in items { hb_obj_free(o) }
        items = Vault.shared.list("known")
        applyFilter()
        table.reloadData()
    }
    private func applyFilter() {
        let q = (search?.stringValue ?? "").lowercased()
        filtered = q.isEmpty ? nil : items.filter {
            hb_obj_get_str($0, "host").str.lowercased().contains(q)
                || hb_obj_get_str($0, "fingerprint").str
                    .lowercased().contains(q)
        }
    }
    private func at(_ r: Int) -> OpaquePointer { (filtered ?? items)[r] }

    // MARK: 行
    func numberOfRows(in t: NSTableView) -> Int {
        (filtered ?? items).count
    }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let o = at(row)
        guard let cid = c?.identifier.rawValue else { return nil }
        if cid == "del" {
            let b = NSButton()
            b.isBordered = false
            b.image = hbIcon("trash", size: 13, tint: Theme.dyn("termDim"))
            b.target = self
            b.action = #selector(delRow(_:))
            b.tag = row
            b.setAccessibilityIdentifier("known-del")
            return b
        }
        let l = NSTextField(labelWithString: "")
        l.lineBreakMode = .byTruncatingMiddle
        switch cid {
        case "host":
            let h = hb_obj_get_str(o, "host").str
            l.stringValue = KnownFormat.hostDisp(h)
            l.toolTip = h
        case "port":
            let p = Int(hb_obj_get_int(o, "port", 22))
            l.stringValue = p > 0 ? "\(p)" : "—"
        case "keytype":
            l.stringValue = hb_obj_get_str(o, "keytype").str
            l.font = .monospacedSystemFont(ofSize: 11.5, weight: .regular)
        case "fp":
            let fp = hb_obj_get_str(o, "fingerprint").str
            l.stringValue = fp
            l.toolTip = fp
            l.font = .monospacedSystemFont(ofSize: 11.5, weight: .regular)
        case "added":
            let a = hb_obj_get_int(o, "added", 0)
            l.stringValue = a > 0 ? LogFormat.dateOnly(a) : ""
        default: break
        }
        return l
    }

    // MARK: 动作
    @objc private func searchChanged() { applyFilter(); table.reloadData() }

    @objc private func rowDetails() {
        let r = table.clickedRow
        guard r >= 0, r < numberOfRows(in: table) else { return }
        let o = at(r)
        let host = hb_obj_get_str(o, "host").str
        let port = hb_obj_get_int(o, "port", 22)
        let kt = hb_obj_get_str(o, "keytype").str
        let fp = hb_obj_get_str(o, "fingerprint").str
        let added = hb_obj_get_int(o, "added", 0)
        let line = hb_obj_get_str(o, "line").str
        let a = NSAlert()
        a.messageText = L("Known host 详情")
        var txt = "\(L("主机")): \(host)\n\(L("端口")): \(port)"
            + "\n\(L("密钥类型")): \(kt)\n\(L("指纹 (SHA256)")): \(fp)"
        if added > 0 {
            txt += "\n\(L("添加时间")): \(LogFormat.date(added))"
        }
        if !line.isEmpty && line != host {
            txt += "\n\(L("原始行")): \(line.prefix(200))"
        }
        a.informativeText = txt
        a.addButton(withTitle: L("关闭"))
        DialogRegistry.shared.present(alert: a, kind: "known-detail",
                                      in: window) { _ in }
    }

    @objc private func delRow(_ b: NSButton) {
        let o = at(b.tag)
        let id = String(cString: hb_obj_id(o))
        let host = KnownFormat.hostDisp(hb_obj_get_str(o, "host").str)
        let a = NSAlert()
        a.messageText = String(format: L("删除 %@ 的已知主机条目？"), host)
        a.informativeText = L("下次连接将重新弹出指纹确认（TOFU）。")
        a.addButton(withTitle: L("删除"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: "known-del",
                                      in: window) { [weak self] ok in
            guard ok else { return }
            Vault.shared.delete(id)
            self?.reload()
            let u = NSAlert()
            u.messageText = String(format: L("已删除 %@"), host)
            u.addButton(withTitle: L("撤销"))
            u.addButton(withTitle: L("关闭"))
            DialogRegistry.shared.present(alert: u, kind: "undo-del",
                                          in: self?.window) { uok in
                if uok { Vault.shared.undelete(id); self?.reload() }
            }
        }
    }

    @objc private func importBtn() { importFile(path: nil) }
    /// path != nil → 直接导入（dev 钩子；测试只传生成文件，绝不碰真实 ~/.ssh）。
    func importFile(path: String?) {
        var p = path
        if p == nil {
            let op = NSOpenPanel()
            op.allowsMultipleSelection = false
            op.directoryURL = URL(fileURLWithPath:
                NSHomeDirectory() + "/.ssh")
            guard op.runModal() == .OK, let u = op.url else { return }
            p = u.path
        }
        guard let p, let v = Vault.shared.vault else { return }
        var r = hb_import_result()
        let ok = p.withCString { hb_known_import_file(v, $0, &r) }
        defer { hb_import_result_free(&r) }
        reload()
        let a = NSAlert()
        if ok == HB_OK {
            a.messageText = String(format: L("已导入 %zu 条，跳过 %zu 条"),
                                   r.created, r.skipped)
            if r.nwarn > 0 {
                var ws = ""
                for i in 0..<r.nwarn {
                    ws += (r.warnings?[i].map { String(cString: $0) } ?? "")
                    ws += "\n"
                }
                a.informativeText = ws
            }
        } else {
            a.messageText = L("导入失败（无法读取文件）")
        }
        a.addButton(withTitle: L("关闭"))
        DialogRegistry.shared.present(alert: a, kind: "known-import",
                                      in: window) { _ in }
    }

    @objc private func exportBtn() { exportFile(path: nil) }
    func exportFile(path: String?) {
        guard let v = Vault.shared.vault else { return }
        var out: UnsafeMutablePointer<CChar>?
        guard hb_known_export_text(v, &out) == HB_OK, let out
        else { return }
        let txt = String(cString: out)
        free(out)
        if let path {
            try? txt.write(toFile: path, atomically: true,
                           encoding: .utf8)
            return
        }
        let sp = NSSavePanel()
        sp.nameFieldStringValue = "known_hosts"
        guard let w = window else { return }
        sp.beginSheetModal(for: w) { r in
            guard r == .OK, let url = sp.url else { return }
            try? txt.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    @objc private func writeBackBtn() {
        let dst = NSHomeDirectory() + "/.ssh/known_hosts"
        let a = NSAlert()
        a.messageText = L("写回 known_hosts")
        a.informativeText = String(format:
            L("将把 Vault 中的 %d 条 known hosts 写入 %@（原文件备份为 .harbor-bak）。继续？"),
            items.count, dst)
        a.addButton(withTitle: L("写入"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: "known-write",
                                      in: window) { [weak self] ok in
            guard ok else { return }
            self?.doWriteBack(dst)
        }
    }
    private func doWriteBack(_ dst: String) {
        guard let v = Vault.shared.vault else { return }
        var out: UnsafeMutablePointer<CChar>?
        guard hb_known_export_text(v, &out) == HB_OK, let out
        else { return }
        let txt = String(cString: out)
        free(out)
        let fm = FileManager.default
        // dotfiles 仓库可能是符号链接：解析到真实目标再写（同 Linux）。
        var real = dst
        if let r = try? fm.destinationOfSymbolicLink(atPath: dst),
           !r.isEmpty {
            real = r.hasPrefix("/")
                ? r : (dst as NSString).deletingLastPathComponent + "/" + r
        }
        // 精确行去重追加（同 Linux）：已有行保留原样，只补缺失行。
        let existing = (try? String(contentsOfFile: real,
                                    encoding: .utf8)) ?? ""
        var have = Set(existing.split(separator: "\n")
                           .map { String($0) })
        var add: [String] = []
        for l in txt.split(separator: "\n") where !have.contains(String(l)) {
            have.insert(String(l)); add.append(String(l))
        }
        if fm.fileExists(atPath: real) {
            try? fm.copyItem(atPath: real, toPath: real + ".harbor-bak")
        }
        try? fm.createDirectory(
            atPath: (real as NSString).deletingLastPathComponent,
            withIntermediateDirectories: true)
        var body = existing
        if !body.isEmpty && !body.hasSuffix("\n") { body += "\n" }
        body += add.joined(separator: "\n")
        if !add.isEmpty { body += "\n" }
        try? body.write(toFile: real, atomically: true, encoding: .utf8)
        // 新建文件默认 0600（同 Linux）；已有文件保留原权限。
        if existing.isEmpty {
            try? fm.setAttributes([.posixPermissions: 0o600],
                                  ofItemAtPath: real)
        }
    }

    // MARK: dev
    var devState: String {
        var arr: [[String: Any]] = []
        for o in filtered ?? items {
            arr.append([
                "id": String(cString: hb_obj_id(o)),
                "host": hb_obj_get_str(o, "host").str,
                "port": hb_obj_get_int(o, "port", 22),
                "keytype": hb_obj_get_str(o, "keytype").str,
                "fp": hb_obj_get_str(o, "fingerprint").str])
        }
        let d = try? JSONSerialization.data(withJSONObject: arr)
        return d.flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
    }
}
