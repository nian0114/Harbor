import AppKit
import HarborKit
import CHarbor

/// 凭据页：密钥生成/导入/导出公钥/密码凭据/级联删除（FR-02 无 TouchID/FIDO）。
final class KeychainPage: NSView, NSTableViewDataSource, NSTableViewDelegate {
    private var items: [(id: String, kind: String, name: String,
                         meta: String, fp: String, bits: Int,
                         created: String, usedBy: Int)] = []
    private let table = NSTableView()

    override init(frame: NSRect) {
        super.init(frame: frame)
        let bar = NSView(frame: NSRect(x: 0, y: frame.height - 40,
                                       width: frame.width, height: 40))
        bar.autoresizingMask = [.width, .minYMargin]
        addSubview(bar)
        var x: CGFloat = 8
        func btn(_ t: String, _ a: Selector, _ id: String) {
            let b = NSButton(title: t, target: self, action: a)
            b.bezelStyle = .rounded
            b.frame = NSRect(x: x, y: 8, width: 96, height: 24)
            b.setAccessibilityIdentifier(id)
            bar.addSubview(b); x += 104
        }
        btn(L("+ 生成密钥"), #selector(genKey), "kc-gen")
        btn(L("导入密钥"), #selector(importKeyBtn), "kc-import")
        btn(L("+ 密码凭据"), #selector(newCred), "kc-cred")
        btn(L("导出公钥"), #selector(exportPub), "kc-export")
        btn(L("删除"), #selector(del), "kc-delete")
        let scroll = NSScrollView(frame: NSRect(
            x: 0, y: 0, width: frame.width, height: frame.height - 40))
        scroll.autoresizingMask = [.width, .height]
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        addSubview(scroll)
        for (i, t) in [L("名称"), L("类型"), L("位数"),
                       L("指纹/描述"), L("创建"), L("使用者")].enumerated() {
            let c = NSTableColumn(identifier: .init("c\(i)"))
            c.title = t; c.width = [170, 90, 60, 260, 90, 80][i]
            table.addTableColumn(c)
        }
        table.headerView = NSTableHeaderView()
        table.dataSource = self; table.delegate = self
        table.setAccessibilityIdentifier("kc-table")
        reload()
    }
    required init?(coder: NSCoder) { fatalError() }
    func devSelect(id: String) -> Bool {
        guard let i = items.firstIndex(where: { $0.id == id })
        else { return false }
        table.selectRowIndexes(IndexSet(integer: i),
                               byExtendingSelection: false)
        return true
    }

    func reload() {
        items = []
        // 统计每个 key 被多少 host 引用
        var ref: [String: Int] = [:]
        for h in Vault.shared.list("host") {
            let kid = hb_obj_get_str(h, "key_id").str
            if !kid.isEmpty { ref[kid, default: 0] += 1 }
            hb_obj_free(h)
        }
        for o in Vault.shared.list("key") {
            let id = hb_obj_id(o).str
            items.append((id, "key",
                          hb_obj_get_str(o, "name").str,
                          hb_obj_get_str(o, "keytype").str,
                          hb_obj_get_str(o, "fingerprint").str,
                          Int(hb_obj_get_int(o, "bits", 0)),
                          hb_obj_get_str(o, "created").str,
                          ref[id] ?? 0))
            hb_obj_free(o)
        }
        for o in Vault.shared.list("cred") {
            items.append((hb_obj_id(o).str, "cred",
                          hb_obj_get_str(o, "name").str, L("密码"), "",
                          0, hb_obj_get_str(o, "created").str, 0))
            hb_obj_free(o)
        }
        table.reloadData()
    }

    // MARK: 生成
    @objc private func genKey() {
        let a = NSAlert()
        a.messageText = L("生成密钥")
        a.addButton(withTitle: L("生成")); a.addButton(withTitle: L("取消"))
        let name = NSTextField(frame: NSRect(x: 0, y: 76, width: 240, height: 22))
        name.placeholderString = L("名称")
        name.setAccessibilityIdentifier("kgen-name")
        let type = NSPopUpButton(frame: NSRect(x: 0, y: 46, width: 240, height: 24))
        type.addItems(withTitles: ["Ed25519", "ECDSA", "RSA 3072", "RSA 4096"])
        type.setAccessibilityIdentifier("kgen-type")
        let pp = NSSecureTextField(frame: NSRect(x: 0, y: 20, width: 240, height: 22))
        pp.placeholderString = L("口令（可空）")
        pp.setAccessibilityIdentifier("kgen-pass")
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 240, height: 104))
        v.addSubview(name); v.addSubview(type); v.addSubview(pp)
        a.accessoryView = v
        DialogRegistry.shared.present(alert: a, kind: "kgen",
                                      in: window) { [weak self] ok in
            guard ok, let self else { return }
            let t: hb_key_type
            var bits = 0
            switch type.indexOfSelectedItem {
            case 1: t = HB_KEY_ECDSA_P256
            case 2: t = HB_KEY_RSA; bits = 3072
            case 3: t = HB_KEY_RSA; bits = 4096
            default: t = HB_KEY_ED25519
            }
            self.generate(type: t, bits: bits,
                          name: name.stringValue, pass: pp.stringValue)
            pp.stringValue = ""
        }
    }
    func generate(type t: hb_key_type, bits: Int,
                  name: String, pass: String) {
        _ = KeyOps.generate(type: t, bits: bits, name: name, pass: pass)
        reload()
    }

    // MARK: 导入
    @objc private func importKeyBtn() { importKey(path: nil) }
    func importKey(path: String?) {
        var p = path
        if p == nil {
            let op = NSOpenPanel()
            guard op.runModal() == .OK, let u = op.url else { return }
            p = u.path
        }
        guard let p else { return }
        askPassphraseOnce { [weak self] pass in
            self?.doImport(path: p, pass: pass)
        }
    }
    private func askPassphraseOnce(_ cb: @escaping (String) -> Void) {
        let a = NSAlert()
        a.messageText = L("私钥口令（无口令留空）")
        a.addButton(withTitle: L("导入")); a.addButton(withTitle: L("取消"))
        let pp = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 22))
        pp.setAccessibilityIdentifier("kimp-pass")
        a.accessoryView = pp
        DialogRegistry.shared.present(alert: a, kind: "kimport",
                                      in: window) { ok in
            cb(ok ? pp.stringValue : "")
        }
    }
    func doImport(path: String, pass: String) {
        _ = KeyOps.doImport(path: path, pass: pass, window: window)
        reload()
    }

    // MARK: 导出公钥
    @objc private func exportPub() {
        let r = table.selectedRow
        guard r >= 0, r < items.count, items[r].kind == "key",
              let o = Vault.shared.get(items[r].id) else { return }
        let it = items[r]
        defer { hb_obj_free(o) }
        let pub = hb_obj_get_str(o, "public").str
        guard !pub.isEmpty else { NSSound.beep(); return }
        let a = NSAlert()
        a.messageText = L("导出公钥")
        a.addButton(withTitle: L("复制到剪贴板"))
        a.addButton(withTitle: L("保存到文件…"))
        a.addButton(withTitle: L("取消"))
        let r2 = a.runModal()
        if r2 == .alertFirstButtonReturn {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(pub, forType: .string)
        } else if r2 == .alertSecondButtonReturn {
            let sp = NSSavePanel()
            sp.nameFieldStringValue = it.name + ".pub"
            if sp.runModal() == .OK, let u = sp.url {
                try? pub.write(to: u, atomically: true,
                               encoding: .utf8)
            }
        }
    }

    // MARK: 密码凭据
    @objc private func newCred() {
        let a = NSAlert()
        a.messageText = L("新建密码凭据")
        a.addButton(withTitle: L("保存")); a.addButton(withTitle: L("取消"))
        let name = NSTextField(frame: NSRect(x: 0, y: 30, width: 240, height: 22))
        name.placeholderString = L("名称")
        name.setAccessibilityIdentifier("cred-name")
        let pw = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 22))
        pw.placeholderString = L("密码")
        pw.setAccessibilityIdentifier("cred-pass")
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 240, height: 56))
        v.addSubview(name); v.addSubview(pw)
        a.accessoryView = v
        DialogRegistry.shared.present(alert: a, kind: "cred",
                                      in: window) { [weak self] ok in
            guard ok, let self, !pw.stringValue.isEmpty else { return }
            let o = hb_obj_new("cred")!
            let cid = hb_obj_id(o).str
            hb_obj_set_str(o, "name", name.stringValue)
            hb_obj_set_int(o, "created",
                Int64(Date().timeIntervalSince1970))
            Vault.shared.put(o)
            Vault.shared.setSecret(cid, "password", pw.stringValue)
            pw.stringValue = ""
            self.reload()
        }
    }

    // MARK: 删除（级联）
    @objc private func del() {
        let r = table.selectedRow
        guard r >= 0, r < items.count else { return }
        let it = items[r]
        // 引用计数：多少 host.key_id == it.id
        let refs = it.kind == "key" ? Vault.shared.list("host").filter {
            let k = hb_obj_get_str($0, "key_id").str == it.id
            hb_obj_free($0); return k
        }.count : 0
        let a = NSAlert()
        a.messageText = refs > 0
            ? String(format: L("删除 %@？%d 台主机引用它，认证将回退为“未设置”。"),
                     it.name, refs)
            : String(format: L("删除 %@？"), it.name)
        a.addButton(withTitle: L("删除")); a.addButton(withTitle: L("取消"))
        a.alertStyle = .warning
        DialogRegistry.shared.present(alert: a, kind: "kdel",
                                      in: window) { [weak self] ok in
            guard ok, let self, let vv = Vault.shared.vault else { return }
            if it.kind == "key" {
                _ = hb_vault_delete_key_cascade(vv, it.id, nil, nil)
            } else {
                _ = hb_vault_delete(vv, it.id)
            }
            self.reload()
        }
    }

    func numberOfRows(in t: NSTableView) -> Int { items.count }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let it = items[row]
        let col = c?.identifier.rawValue ?? ""
        let txt: String
        switch col {
        case "c0": txt = it.name
        case "c1": txt = it.meta
        case "c2": txt = it.bits > 0 ? "\(it.bits)" : "—"
        case "c4":
            let ts = TimeInterval(it.created) ?? 0
            txt = ts > 0 ? DateFormatter.localizedString(
                from: Date(timeIntervalSince1970: ts),
                dateStyle: .short, timeStyle: .none) : "—"
        case "c5": txt = it.usedBy > 0
            ? String(format: L("%d 台主机"), it.usedBy) : "—"
        default:
            txt = it.fp.isEmpty
                ? (it.kind == "cred" ? "••••••" : "") : it.fp
        }
        let v = NSTextField(labelWithString: txt)
        v.lineBreakMode = .byTruncatingMiddle
        return v
    }
}

/// 密钥生成/导入（KeychainPage 与 HostForm 内联复用）。返回新 key id。
enum KeyOps {
    @discardableResult
    static func generate(type t: hb_key_type, bits: Int,
                         name: String, pass: String) -> String? {
        var k = hb_key_info()
        let e = name.withCString { nm in
            hb_key_generate(t, Int32(bits),
                            name.isEmpty ? nil : nm, &k)
        }
        guard e == HB_OK else { NSSound.beep(); return nil }
        defer { hb_key_info_clear(&k) }
        let o = hb_obj_new("key")!
        let kid = hb_obj_id(o).str
        hb_obj_set_str(o, "name", name.isEmpty ? k.comment.str : name)
        hb_obj_set_str(o, "keytype", k.kind.str)
        hb_obj_set_str(o, "kind", k.kind.str)
        hb_obj_set_int(o, "bits", Int64(k.bits))
        hb_obj_set_str(o, "fingerprint", k.fingerprint)
        hb_obj_set_str(o, "public", k.public_line)
        hb_obj_set_str(o, "created",
            "\(Int64(Date().timeIntervalSince1970))")
        Vault.shared.put(o)
        if let p = k.private_text {
            Vault.shared.setSecret(kid, "private", p.str)
        }
        if !pass.isEmpty {
            Vault.shared.setSecret(kid, "passphrase", pass)
        }
        return kid
    }
    @discardableResult
    static func doImport(path: String, pass: String,
                         window: NSWindow?) -> String? {
        var k = hb_key_info()
        let e = pass.withCString { pp in
            hb_key_inspect_file(path, pass.isEmpty ? nil : pp, &k) }
        guard e == HB_OK else {
            let a = NSAlert()
            a.messageText = e.rawValue == HB_ERR_PASSPHRASE.rawValue
                ? L("口令错误或需要口令") : L("无法识别密钥文件")
            a.addButton(withTitle: L("确定"))
            if let w = window { a.beginSheetModal(for: w) { _ in } }
            return nil
        }
        defer { hb_key_info_clear(&k) }
        let o = hb_obj_new("key")!
        let kid = hb_obj_id(o).str
        let base = URL(fileURLWithPath: path)
            .deletingPathExtension().lastPathComponent
        hb_obj_set_str(o, "name",
                       k.comment.str.isEmpty ? base : k.comment.str)
        hb_obj_set_str(o, "keytype", k.kind.str)
        hb_obj_set_str(o, "kind", k.kind.str)
        hb_obj_set_int(o, "bits", Int64(k.bits))
        hb_obj_set_str(o, "fingerprint", k.fingerprint)
        hb_obj_set_str(o, "public", k.public_line)
        hb_obj_set_bool(o, "encrypted", k.encrypted)
        Vault.shared.put(o)
        if let p = k.private_text {
            Vault.shared.setSecret(kid, "private", p.str)
        }
        // 导入口令不落库：首次使用时询问，由“记住”保存（与 Linux 一致）
        return kid
    }
}
