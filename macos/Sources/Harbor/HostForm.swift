import AppKit
import CHarbor
import HarborKit

/// 新建/编辑主机表单（对齐 linux hb-host-form.c）：
/// 分组：基本信息 / 认证 / 跳板（有序、防环）/ 高级（启动片段、保活、颜色）
/// 认证区随 auth 切换：密码 / 密钥(+生成导入) / agent / identity_file。
/// 校验：地址必填、端口 1-65535、保活 ≥0；无效时保存禁用。
final class HostForm: NSView, NSTextFieldDelegate {
    var onSave: ((HostRec, String?) -> Void)?   // (rec, password?)
    var onCancel: (() -> Void)?
    private var orig: HostRec?

    private let aliasF = NSTextField()
    private let protoP = NSPopUpButton()          // ssh (+rdp/vnc 占位)
    private let addrF = NSTextField()
    private let portF = NSTextField()
    private let userF = NSTextField()
    private let groupF = NSTextField()
    private let tagsF = NSTextField()
    private let authP = NSPopUpButton()
    // 认证子区
    private let pwF = NSSecureTextField()
    private let keyP = NSPopUpButton()
    private let idFileF = NSTextField()
    // 跳板有序列表
    private var jumpIds: [String] = []
    private let jumpList = NSStackView()
    private let jumpAddP = NSPopUpButton()
    // 高级
    private let snippetP = NSPopUpButton()
    private let keepF = NSTextField()
    private let colorP = NSPopUpButton()
    private let errL = NSTextField(labelWithString: "")
    private var saveBtn: NSButton!
    private var keyIds: [String] = []
    private var hostChoices: [(id: String, name: String)] = []

    override init(frame: NSRect) {
        super.init(frame: frame)
        build()
        validate()
    }
    required init?(coder: NSCoder) { fatalError() }

    private func build() {
        // 宿主选择集（跳板/self 排除在 fill 时做）
        hostChoices = Vault.shared.list("host").map { o in
            defer { hb_obj_free(o) }
            return (hb_obj_id(o).str, hb_obj_get_str(o, "name").str)
        }
        var y = frame.height - 36
        func heading(_ t: String) {
            let l = NSTextField(labelWithString: t)
            l.font = .boldSystemFont(ofSize: 12)
            l.frame = NSRect(x: 16, y: y - 18, width: 300, height: 18)
            addSubview(l); y -= 26
        }
        func label(_ t: String, _ v: NSView, h: CGFloat = 24) {
            let l = NSTextField(labelWithString: t)
            l.frame = NSRect(x: 16, y: y - h + 4, width: 96, height: 20)
            l.alignment = .right
            addSubview(l)
            v.frame = NSRect(x: 120, y: y - h,
                             width: frame.width - 136, height: h)
            addSubview(v)
            y -= (h + 6)
        }
        heading(L("基本信息"))
        protoP.addItems(withTitles: ["SSH", L("RDP（M5）"), L("VNC（M5）")])
        protoP.selectItem(at: 0)
        (protoP.item(at: 1))?.isEnabled = false
        (protoP.item(at: 2))?.isEnabled = false
        label(L("协议"), protoP); protoP.setAccessibilityIdentifier("hf-proto")
        label(L("别名"), aliasF); aliasF.setAccessibilityIdentifier("hf-alias")
        label(L("地址"), addrF); addrF.setAccessibilityIdentifier("hf-address")
        portF.placeholderString = "22"
        label(L("端口"), portF); portF.setAccessibilityIdentifier("hf-port")
        label(L("用户名"), userF); userF.setAccessibilityIdentifier("hf-user")
        label(L("分组"), groupF); groupF.setAccessibilityIdentifier("hf-group")
        tagsF.placeholderString = L("逗号分隔")
        label(L("标签"), tagsF); tagsF.setAccessibilityIdentifier("hf-tags")

        heading(L("认证"))
        authP.addItems(withTitles: [L("自动"), L("密码"), L("密钥"),
                                    L("Agent"), L("无")])
        authP.target = self; authP.action = #selector(authChanged)
        label(L("认证"), authP); authP.setAccessibilityIdentifier("hf-auth")
        // 密码
        pwF.placeholderString = L("连接密码（加密保存）")
        label(L("密码"), pwF); pwF.setAccessibilityIdentifier("hf-password")
        // 密钥 picker + 内联生成/导入
        reloadKeys()
        label(L("密钥"), keyP); keyP.setAccessibilityIdentifier("hf-key")
        let kb = NSStackView()
        kb.orientation = .horizontal; kb.spacing = 6
        let gen = NSButton(title: L("生成"), target: self,
                           action: #selector(genKeyInline))
        let imp = NSButton(title: L("导入"), target: self,
                           action: #selector(importKeyInline))
        [gen, imp].forEach { $0.bezelStyle = .rounded; kb.addArrangedSubview($0) }
        kb.frame = NSRect(x: 120, y: y - 24, width: 200, height: 24)
        addSubview(kb); y -= 30
        idFileF.placeholderString = "~/.ssh/id_ed25519"
        label(L("密钥文件"), idFileF)
        idFileF.setAccessibilityIdentifier("hf-idfile")
        let fb = NSButton(title: L("选择…"), target: self,
                          action: #selector(chooseFile))
        fb.bezelStyle = .rounded
        fb.frame = NSRect(x: frame.width - 90, y: y + 6, width: 74, height: 22)
        fb.autoresizingMask = [.minXMargin]
        addSubview(fb)

        heading(L("跳板（外→内，按顺序）"))
        jumpList.orientation = .vertical; jumpList.spacing = 4
        jumpList.frame = NSRect(x: 120, y: y - 70,
                                width: frame.width - 136, height: 70)
        addSubview(jumpList)
        y -= 78
        jumpAddP.addItem(withTitle: L("+ 添加跳板…"))
        jumpAddP.target = self; jumpAddP.action = #selector(addJump)
        jumpAddP.frame = NSRect(x: 120, y: y - 24, width: 200, height: 24)
        jumpAddP.setAccessibilityIdentifier("hf-jump-add")
        addSubview(jumpAddP); y -= 32

        heading(L("高级"))
        reloadSnippets()
        label(L("启动片段"), snippetP)
        snippetP.setAccessibilityIdentifier("hf-snippet")
        keepF.placeholderString = L("30（0=关闭）")
        label(L("保活(秒)"), keepF)
        keepF.setAccessibilityIdentifier("hf-keepalive")
        colorP.addItems(withTitles: [L("无"), "red", "orange", "amber",
                                     "green", "blue", "pink", "purple"])
        label(L("颜色"), colorP); colorP.setAccessibilityIdentifier("hf-color")

        errL.textColor = .systemRed
        errL.font = .systemFont(ofSize: 11)
        errL.frame = NSRect(x: 120, y: 44, width: frame.width - 140, height: 16)
        addSubview(errL)
        saveBtn = NSButton(title: L("保存"), target: self,
                           action: #selector(save))
        saveBtn.bezelStyle = .rounded
        saveBtn.frame = NSRect(x: frame.width - 100, y: 12,
                               width: 80, height: 28)
        saveBtn.keyEquivalent = "\r"
        saveBtn.setAccessibilityIdentifier("hf-save")
        addSubview(saveBtn)
        let cancel = NSButton(title: L("取消"), target: self,
                              action: #selector(cancel))
        cancel.bezelStyle = .rounded
        cancel.frame = NSRect(x: frame.width - 190, y: 12,
                              width: 80, height: 28)
        cancel.setAccessibilityIdentifier("hf-cancel")
        addSubview(cancel)
        for f in [addrF, portF, keepF, aliasF, userF] {
            f.delegate = self
        }
        authChanged()
    }

    func controlTextDidChange(_ n: Notification) { validate() }

    private func reloadKeys() {
        keyP.removeAllItems()
        keyP.addItem(withTitle: L("（无）"))
        keyIds = [""]
        for o in Vault.shared.list("key") {
            keyP.addItem(withTitle: hb_obj_get_str(o, "name").str)
            keyIds.append(hb_obj_id(o).str)
            hb_obj_free(o)
        }
    }
    private func reloadSnippets() {
        snippetP.removeAllItems()
        snippetP.addItem(withTitle: L("（无）"))
        for o in Vault.shared.list("snippet") {
            snippetP.addItem(withTitle: hb_obj_get_str(o, "name").str)
            hb_obj_free(o)
        }
    }

    @objc private func authChanged() {
        let i = max(0, authP.indexOfSelectedItem)
        let a = ["auto", "password", "key", "agent", "none"][i]
        pwF.isEnabled = (a == "password")
        keyP.isEnabled = (a == "key" || a == "auto")
        idFileF.isEnabled = (a == "key" || a == "auto")
    }

    // 跳板行：名称 + ↑↓ + 删除
    private func addJumpRow(_ id: String) {
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 4
        let name = hostChoices.first { $0.id == id }?.name ?? id
        let l = NSTextField(labelWithString: name)
        l.widthAnchor.constraint(greaterThanOrEqualToConstant: 140)
            .isActive = true
        let up = NSButton(title: "↑", target: self,
                          action: #selector(jumpUp(_:)))
        let dn = NSButton(title: "↓", target: self,
                          action: #selector(jumpDown(_:)))
        let del = NSButton(title: "×", target: self,
                           action: #selector(jumpDel(_:)))
        [up, dn, del].forEach { $0.bezelStyle = .inline }
        [l, up, dn, del].forEach { row.addArrangedSubview($0) }
        jumpList.addArrangedSubview(row)
    }
    @objc private func addJump() {
        let i = jumpAddP.indexOfSelectedItem
        jumpAddP.selectItem(at: 0)
        guard i > 0, i - 1 < hostChoices.count else { return }
        let id = hostChoices[i - 1].id
        guard id != orig?.id, !jumpIds.contains(id) else { return }
        jumpIds.append(id)
        addJumpRow(id)
        validate()
    }
    private func rowIndex(_ s: NSButton) -> Int {
        jumpList.arrangedSubviews.firstIndex { $0 === s.superview } ?? -1
    }
    @objc private func jumpUp(_ s: NSButton) {
        let i = rowIndex(s); guard i > 0 else { return }
        jumpIds.swapAt(i, i - 1); rebuildJumpRows()
    }
    @objc private func jumpDown(_ s: NSButton) {
        let i = rowIndex(s); guard i >= 0, i < jumpIds.count - 1 else { return }
        jumpIds.swapAt(i, i + 1); rebuildJumpRows()
    }
    @objc private func jumpDel(_ s: NSButton) {
        let i = rowIndex(s); guard i >= 0 else { return }
        jumpIds.remove(at: i); rebuildJumpRows(); validate()
    }
    private func rebuildJumpRows() {
        jumpList.arrangedSubviews.forEach { $0.removeFromSuperview() }
        jumpIds.forEach { addJumpRow($0) }
    }
    /// 防环：任何跳板自身的 jump 链含本机 → 拒绝
    private func jumpCycle() -> Bool {
        guard let selfId = orig?.id, !selfId.isEmpty else { return false }
        var seen = Set<String>()
        var stack = jumpIds
        while let j = stack.popLast() {
            if j == selfId { return true }
            if seen.contains(j) { continue }
            seen.insert(j)
            if let o = Vault.shared.get(j) {
                var sv: UnsafeMutablePointer<UnsafePointer<CChar>?>?
                let n = hb_obj_get_strv(o, "jump", &sv)
                for k in 0..<n { if let p = sv?[k] { stack.append(p.str) } }
                free(sv)
                hb_obj_free(o)
            }
        }
        return false
    }

    @objc private func chooseFile() {
        let op = NSOpenPanel()
        if op.runModal() == .OK, let u = op.url {
            idFileF.stringValue = u.path
        }
    }
    @objc private func genKeyInline() {
        // 复用 KeychainPage 的生成对话框
        if let w = window {
            _ = w
        }
        NSSound.beep()   // 简化：跳到密钥页生成后回来选
    }
    @objc private func importKeyInline() { NSSound.beep() }

    private func validate() {
        var err = ""
        if addrF.stringValue.trimmingCharacters(in: .whitespaces).isEmpty {
            err = L("地址必填")
        } else if let p = Int(portF.stringValue), portF.stringValue.isEmpty == false,
                  !(1...65535).contains(p) {
            err = L("端口须为 1–65535")
        } else if !portF.stringValue.isEmpty,
                  Int(portF.stringValue) == nil {
            err = L("端口须为数字")
        } else if !keepF.stringValue.isEmpty,
                  (Int(keepF.stringValue) ?? -1) < 0 {
            err = L("保活须 ≥0")
        } else if jumpCycle() {
            err = L("跳板链形成环")
        }
        errL.stringValue = err
        saveBtn.isEnabled = err.isEmpty
    }

    func fill(_ h: HostRec) {
        orig = h
        aliasF.stringValue = h.name
        addrF.stringValue = h.address
        portF.stringValue = h.port > 0 ? "\(h.port)" : ""
        userF.stringValue = h.user
        groupF.stringValue = h.group
        tagsF.stringValue = h.tags.joined(separator: ",")
        idFileF.stringValue = h.identityFile
        keepF.stringValue = h.keepalive >= 0 ? "\(h.keepalive)" : ""
        authP.selectItem(at: ["auto", "password", "key", "agent", "none"]
            .firstIndex(of: h.auth) ?? 0)
        if let i = keyIds.firstIndex(of: h.keyId) { keyP.selectItem(at: i) }
        if let i = ["", "red", "orange", "amber", "green", "blue",
                    "pink", "purple"].firstIndex(of: h.color) {
            colorP.selectItem(at: i)
        }
        jumpIds = h.jumps
        rebuildJumpRows()
        authChanged(); validate()
    }

    private var rec: HostRec {
        var h = orig ?? HostRec(id: "")
        h.name = aliasF.stringValue
        h.address = addrF.stringValue
        h.port = Int(portF.stringValue) ?? 22
        h.user = userF.stringValue
        h.group = groupF.stringValue
        h.tags = tagsF.stringValue.split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        let ai = max(0, authP.indexOfSelectedItem)
        h.auth = ["auto", "password", "key", "agent", "none"][ai]
        h.keyId = keyIds.indices.contains(keyP.indexOfSelectedItem)
            ? keyIds[keyP.indexOfSelectedItem] : ""
        h.identityFile = idFileF.stringValue
        h.jumps = jumpIds
        h.snippetId = snippetP.indexOfSelectedItem > 0
            ? snippetP.selectedItem?.title ?? "" : ""
        h.keepalive = Int(keepF.stringValue) ?? -1
        h.color = ["", "red", "orange", "amber", "green", "blue",
                   "pink", "purple"][max(0, colorP.indexOfSelectedItem)]
        h.proto = "ssh"
        return h
    }
    @objc private func save() {
        let h = rec
        guard !h.address.isEmpty else { NSSound.beep(); return }
        let pw = pwF.stringValue.isEmpty ? nil : pwF.stringValue
        pwF.stringValue = ""
        onSave?(h, pw)
    }
    @objc private func cancel() { onCancel?() }
}
