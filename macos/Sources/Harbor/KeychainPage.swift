import AppKit
import HarborKit
import CHarbor

/// 凭据页 —— 对齐 prototype keychainView：工具栏（New key 分裂键 +
/// Certificate/Touch ID/FIDO2 ghost + 搜索），白卡片表格 = 名称 |
/// 类型 | 指纹 | 生物识别 pill | 被引用 | 行内 复制公钥/编辑/删除；
/// 表下 hintbox（FR-11）。密钥生成/导入/密码凭据/级联删除（FR-02）。
final class KeychainPage: NSView, NSTableViewDataSource, NSTableViewDelegate {
    private var items: [(id: String, kind: String, name: String,
                         meta: String, fp: String, bits: Int,
                         created: String, usedBy: Int, bio: Bool)] = []
    private var shown: [(id: String, kind: String, name: String,
                         meta: String, fp: String, bits: Int,
                         created: String, usedBy: Int, bio: Bool)] = []
    private let card = VTableCard()
    private var search: VField!
    private var searchMinimumX: CGFloat = 18

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.hbBg = "content"
        buildBar()
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
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
        let hint = vHintBox(L("私钥经 Secure Enclave / Android Keystore 包装加密存储，同步走端到端加密（FR-11），服务端零明文。"))
        hint.translatesAutoresizingMaskIntoConstraints = false
        box.addArrangedSubview(hint)
        hint.widthAnchor.constraint(
            equalTo: box.widthAnchor, constant: -36).isActive = true
        for (i, t) in [L("名称"), L("类型"), L("指纹"),
                       L("生物识别"), L("被引用"), ("")].enumerated() {
            let c = NSTableColumn(identifier: .init("c\(i)"))
            c.title = t
            c.headerCell = TableHeadCell(textCell: t)
            c.resizingMask = []
            if i == 0 || i == 2 {
                c.minWidth = 160
            } else {
                c.width = [0, 110, 0, 110, 90, 96][i]
                c.minWidth = c.width
                c.maxWidth = c.width
            }
            card.table.addTableColumn(c)
        }
        card.table.dataSource = self
        card.table.delegate = self
        card.table.setAccessibilityIdentifier("kc-table")
        reload()
        card.sizeColumns = { [weak self] width in
            guard let cols = self?.card.table.tableColumns else { return }
            let flex = max(320, width - 406)
            cols[0].width = max(160, floor(flex * 0.42))
            cols[2].width = flex - cols[0].width
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        let width = min(220, max(24, bounds.width - searchMinimumX - 18))
        search.frame = NSRect(x: bounds.width - width - 18,
                              y: bounds.height - 45, width: width, height: 34)
    }

    private func buildBar() {
        let split = VSplit("New key", icon: "plus")
        split.entries = [
            VMenuEntry(L("生成新密钥")) { [weak self] in
                self?.genKey()
            },
            VMenuEntry(L("导入私钥…")) { [weak self] in
                self?.importKeyBtn()
            },
            VMenuEntry(L("导出全部公钥")) { [weak self] in
                self?.exportAllPub()
            },
            VMenuEntry(L("密码凭据")) { [weak self] in
                self?.newCred()
            },
        ]
        split.size()
        split.frame.origin = NSPoint(x: 18, y: bounds.height - 45)
        split.autoresizingMask = [.minYMargin]
        split.main.setAccessibilityIdentifier("kc-gen")
        split.dd.setAccessibilityIdentifier("kc-menu")
        addSubview(split)
        var x = 18 + split.frame.width + 8
        for (t, ic, msg) in [
            (L("Certificate"), "cert", L("证书导入（P1）")),
            (L("Touch ID"), "fingerprint",
             L("Touch ID / 生物识别解锁已在 macOS 生效")),
            (L("FIDO2"), "fido", L("FIDO2 安全密钥（P1，libfido2）")),
        ] {
            let b = HBtn(t, style: .ghost, icon: ic)
            b.sizeToFit()
            b.frame = NSRect(x: x, y: bounds.height - 45,
                             width: b.intrinsicContentSize.width + 28,
                             height: 33)
            b.autoresizingMask = [.minYMargin]
            b.onTap = { [weak self] in
                self?.showToast(SftpToast(text: msg))
            }
            addSubview(b)
            x += b.frame.width + 8
        }
        searchMinimumX = x
        search = VField(placeholder: L("搜索密钥…"), width: 220)
        search.frame.origin = NSPoint(x: bounds.width - 238,
                                      y: bounds.height - 45)
        search.autoresizingMask = [.minXMargin, .minYMargin]
        search.onChange = { [weak self] in self?.applyFilter() }
        search.entry.setAccessibilityIdentifier("kc-search")
        addSubview(search)
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

    func devSelect(id: String) -> Bool {
        guard let i = shown.firstIndex(where: { $0.id == id })
        else { return false }
        card.table.selectRowIndexes(IndexSet(integer: i),
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
                          ref[id] ?? 0,
                          hb_obj_get_bool(o, "confirm", false)))
            hb_obj_free(o)
        }
        for o in Vault.shared.list("cred") {
            items.append((hb_obj_id(o).str, "cred",
                          hb_obj_get_str(o, "name").str, L("密码"), "",
                          0, hb_obj_get_str(o, "created").str, 0,
                          false))
            hb_obj_free(o)
        }
        applyFilter()
    }
    private func applyFilter() {
        let q = search?.stringValue.trimmingCharacters(
            in: .whitespaces).lowercased() ?? ""
        shown = q.isEmpty ? items : items.filter {
            "\($0.name) \($0.meta) \($0.fp)".lowercased().contains(q)
        }
        card.table.reloadData()
        card.setRows(shown.count, emptyText: L("无匹配密钥"))
    }

    // MARK: 生成
    @objc private func genKey() {
        let m = HModal(title: "New key", icon: "key")
        func box(_ v: NSView) -> NSView {
            v.translatesAutoresizingMaskIntoConstraints = false
            let b = NSView()
            b.wantsLayer = true; b.layer?.cornerRadius = 8; b.layer?.borderWidth = 1
            b.layer?.hbBg = "tableHead"; b.layer?.hbBorder = "line"
            b.addSubview(v)
            NSLayoutConstraint.activate([
                b.heightAnchor.constraint(equalToConstant: 36),
                v.leadingAnchor.constraint(equalTo: b.leadingAnchor, constant: 10),
                v.trailingAnchor.constraint(equalTo: b.trailingAnchor, constant: -10),
                v.centerYAnchor.constraint(equalTo: b.centerYAnchor),
            ])
            return b
        }
        func lbl(_ s: String) -> NSTextField {
            let l = NSTextField(labelWithString: s)
            l.font = .systemFont(ofSize: 12, weight: .semibold); l.hbText = "text2"
            return l
        }
        m.addBody(lbl(L("名称")))
        let name = NSTextField()
        name.isBordered = false; name.drawsBackground = false; name.focusRingType = .none
        name.font = .systemFont(ofSize: 13); name.hbText = "text"
        name.placeholderString = "id_ed25519_new"
        name.setAccessibilityIdentifier("kgen-name")
        m.addBody(box(name))

        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 12; row.distribution = .fillEqually
        let c1 = VFlippedStack(); c1.orientation = .vertical; c1.alignment = .leading; c1.spacing = 5
        let c2 = VFlippedStack(); c2.orientation = .vertical; c2.alignment = .leading; c2.spacing = 5
        let type = NSPopUpButton()
        type.isBordered = false; type.font = .systemFont(ofSize: 13)
        type.addItems(withTitles: ["Ed25519", "ECDSA", "RSA 3072", "RSA 4096"])
        type.setAccessibilityIdentifier("kgen-type")
        let tb = box(type)
        c1.addArrangedSubview(lbl(L("算法"))); c1.addArrangedSubview(tb)
        tb.widthAnchor.constraint(equalTo: c1.widthAnchor).isActive = true

        let pp = NSSecureTextField()
        pp.isBordered = false; pp.drawsBackground = false; pp.focusRingType = .none
        pp.font = .systemFont(ofSize: 13); pp.hbText = "text"
        pp.placeholderString = L("可选")
        pp.setAccessibilityIdentifier("kgen-pass")
        let pb = box(pp)
        c2.addArrangedSubview(lbl("Passphrase")); c2.addArrangedSubview(pb)
        pb.widthAnchor.constraint(equalTo: c2.widthAnchor).isActive = true
        row.addArrangedSubview(c1); row.addArrangedSubview(c2)
        m.addBody(row)

        m.addFootBtn(L("取消"), style: .gray, accept: false)
        m.addFootBtn(L("生成"), style: .primary, accept: true)
        m.present(kind: "kgen", in: window) { [weak self] ok in
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

    // MARK: 编辑（名称/确认签名）
    private func editKey(id: String) {
        guard let it = items.first(where: { $0.id == id }),
              it.kind == "key",
              let o = Vault.shared.get(id) else { return }
        let a = NSAlert()
        a.messageText = L("编辑密钥")
        a.addButton(withTitle: L("保存")); a.addButton(withTitle: L("取消"))
        let name = NSTextField(frame: NSRect(x: 0, y: 76, width: 240, height: 22))
        name.stringValue = hb_obj_get_str(o, "name").str
        name.setAccessibilityIdentifier("kedit-name")
        // FR-02：签名需 Touch ID —— 数据模型上是 key.confirm（Linux 语义
        // "使用前确认"）。有生物识别 → 用 LAContext 评估；没有（VM）→
        // 选项禁用并说明原因，已置位的密钥仍按确认框执行。
        let bio = Biometric.canEvaluate()
        let conf = NSButton(checkboxWithTitle:
            bio ? L("签名需 Touch ID") : L("每次用此密钥签名前先确认"),
            target: nil, action: nil)
        conf.state = hb_obj_get_bool(o, "confirm", false) ? .on : .off
        conf.isEnabled = bio
        conf.frame = NSRect(x: 0, y: 44, width: 260, height: 22)
        conf.setAccessibilityIdentifier("kedit-confirm")
        let note = NSTextField(wrappingLabelWithString:
            bio ? "" : L("此设备没有可用的 Touch ID，已开启的密钥仍可要求确认。"))
        note.font = .systemFont(ofSize: 11)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 0, y: 0, width: 260, height: 40)
        let vv = NSView(frame: NSRect(x: 0, y: 0, width: 260, height: 104))
        vv.addSubview(name); vv.addSubview(conf); vv.addSubview(note)
        a.accessoryView = vv
        DialogRegistry.shared.present(alert: a, kind: "kedit",
                                      in: window) { [weak self] ok in
            guard ok else { hb_obj_free(o); return }
            hb_obj_set_str(o, "name", name.stringValue)
            if bio {
                hb_obj_set_bool(o, "confirm", conf.state == .on)
            }
            Vault.shared.put(o)
            hb_obj_free(o)
            self?.reload()
        }
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

    // MARK: 公钥
    private func copyPub(id: String) {
        guard let o = Vault.shared.get(id) else { return }
        defer { hb_obj_free(o) }
        let pub = hb_obj_get_str(o, "public").str
        guard !pub.isEmpty else { NSSound.beep(); return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(pub, forType: .string)
        showToast(SftpToast(text: L("公钥已复制")))
    }
    private func exportAllPub() {
        var lines: [String] = []
        for it in items where it.kind == "key" {
            guard let o = Vault.shared.get(it.id) else { continue }
            let pub = hb_obj_get_str(o, "public").str
            hb_obj_free(o)
            if !pub.isEmpty { lines.append("# \(it.name)\n\(pub)") }
        }
        guard !lines.isEmpty else { NSSound.beep(); return }
        let sp = NSSavePanel()
        sp.nameFieldStringValue = "harbor_pubkeys.txt"
        sp.begin { [weak self] resp in
            guard resp == .OK, let u = sp.url else { return }
            try? lines.joined(separator: "\n").write(
                to: u, atomically: true, encoding: .utf8)
            self?.showToast(SftpToast(
                text: L("公钥已导出（私钥永不导出）")))
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
    private func del(id: String) {
        guard let it = items.first(where: { $0.id == id }) else { return }
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

    func numberOfRows(in t: NSTableView) -> Int { shown.count }
    func tableView(_ t: NSTableView, rowViewForRow row: Int)
        -> NSTableRowView? { VTableRowView() }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        let it = shown[row]
        let col = c?.identifier.rawValue ?? ""
        switch col {
        case "c0":
            let s = NSStackView()
            s.orientation = .horizontal
            s.spacing = 7
            let iv = NSImageView()
            iv.image = hbIcon("key", size: 13, tint: Theme.dyn("text2"))
            iv.translatesAutoresizingMaskIntoConstraints = false
            iv.widthAnchor.constraint(equalToConstant: 13).isActive = true
            iv.heightAnchor.constraint(equalToConstant: 13).isActive = true
            s.addArrangedSubview(iv)
            s.addArrangedSubview(vLabel(it.name, bold: true))
            return s
        case "c1":
            return vLabel(it.bits > 0
                ? "\(it.meta) \(it.bits)" : it.meta)
        case "c2":
            return vLabel(it.fp.isEmpty
                ? (it.kind == "cred" ? "••••••" : "—") : it.fp,
                mono: true, dim: true)
        case "c3":
            let wrap = NSView()
            let p = it.bio ? VPill(L("Touch ID"), kind: .on)
                : VPill(L("关闭"), kind: .off, dot: false)
            p.translatesAutoresizingMaskIntoConstraints = false
            wrap.addSubview(p)
            NSLayoutConstraint.activate([
                p.leadingAnchor.constraint(
                    equalTo: wrap.leadingAnchor),
                p.centerYAnchor.constraint(
                    equalTo: wrap.centerYAnchor),
            ])
            return wrap
        case "c4":
            return vLabel(it.usedBy > 0
                ? String(format: L("%d 台主机"), it.usedBy) : L("0 台主机"))
        default:
            let s = NSStackView()
            s.orientation = .horizontal
            s.spacing = 4
            if it.kind == "key" {
                let cb = VIconBtn(icon: "link")
                cb.toolTip = L("复制公钥")
                cb.onTap = { [weak self] in self?.copyPub(id: it.id) }
                cb.setAccessibilityIdentifier("kc-copy-\(it.id)")
                s.addArrangedSubview(cb)
                let eb = VIconBtn(icon: "wand")
                eb.toolTip = L("编辑")
                eb.onTap = { [weak self] in self?.editKey(id: it.id) }
                eb.setAccessibilityIdentifier("kc-edit-\(it.id)")
                s.addArrangedSubview(eb)
            }
            let db = VIconBtn(icon: "trash", kind: .del)
            db.toolTip = L("删除密钥")
            db.onTap = { [weak self] in self?.del(id: it.id) }
            db.setAccessibilityIdentifier("kc-del-\(it.id)")
            s.addArrangedSubview(db)
            return s
        }
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
