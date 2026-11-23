import AppKit
import CHarbor
import HarborKit

/// 新建/编辑主机表单（对齐 linux hb-host-form.c）：
/// 分节滚动 + 固定底部按钮条；字段级错误紧随其字段；
/// 跳板有序列表（防环），空时有紧凑占位；密钥内联生成/导入。
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
    private let pwF = NSSecureTextField()
    private let keyP = NSPopUpButton()
    private let idFileF = NSTextField()
    private var jumpIds: [String] = []
    private let jumpList = NSStackView()
    private let jumpAddP = NSPopUpButton()
    private let snippetP = NSPopUpButton()
    private let keepF = NSTextField()
    private let colorP = NSPopUpButton()
    private var saveBtn: NSButton!
    // 桌面（rdp/vnc）字段：桌面认证 + SSH 隧道（linux hb-host-form.c）
    private let dpwF = NSSecureTextField()
    private let dpwClearB = NSButton(title: "", target: nil, action: nil)
    private var dpwCleared = false
    private let domF = NSTextField()
    private let tunnelSw = NSSwitch()
    private let viaP = NSPopUpButton()
    private var viaIds: [String?] = []
    private let fwdBtn = NSButton(title: "", target: nil, action: nil)
    private let tunnelHint = NSTextField(labelWithString: "")
    private var keyIds: [String] = []
    private var hostChoices: [(id: String, name: String)] = []
    /// 字段 → 错误标签
    private var errFor: [String: NSTextField] = [:]
    /// 当前展示的错误集合；无错误的字段错误行整体折叠不占位
    private var errShown: Set<String> = []
    private let content = FlippedView()

    override init(frame: NSRect) {
        super.init(frame: frame)
        build()
        validate()
    }
    required init?(coder: NSCoder) { fatalError() }

    /// 内容布局游标
    private var y: CGFloat = 0
    private let pad: CGFloat = 16
    private var contentW: CGFloat { content.bounds.width }

    private func heading(_ t: String) {
        let l = NSTextField(labelWithString: t)
        l.font = .boldSystemFont(ofSize: 12)
        l.textColor = .secondaryLabelColor
        l.frame = NSRect(x: pad, y: y, width: 300, height: 18)
        content.addSubview(l); y += 26
    }
    /// 字段行：label + control，可选字段级错误标签紧随其下
    @discardableResult
    private func field(_ t: String, _ v: NSView, h: CGFloat = 24,
                       errKey: String? = nil) -> NSView {
        let l = NSTextField(labelWithString: t)
        l.frame = NSRect(x: pad, y: y + 2, width: 92, height: 20)
        l.alignment = .right
        content.addSubview(l)
        v.frame = NSRect(x: pad + 100, y: y,
                         width: contentW - pad * 2 - 100, height: h)
        content.addSubview(v)
        y += h + 2
        if let k = errKey {
            let e = NSTextField(labelWithString: "")
            e.textColor = .systemRed
            e.font = .systemFont(ofSize: 10)
            let show = errShown.contains(k)
            e.isHidden = !show
            e.frame = NSRect(x: pad + 100, y: y,
                             width: contentW - pad * 2 - 100, height: 14)
            content.addSubview(e)
            errFor[k] = e
            if show { y += 14 }
        }
        y += 6
        return v
    }

    private func build() {
        hostChoices = Vault.shared.list("host").map { o in
            defer { hb_obj_free(o) }
            return (hb_obj_id(o).str, hb_obj_get_str(o, "name").str)
        }
        // 滚动区 + 固定底部
        let scroll = NSScrollView(frame: NSRect(
            x: 0, y: 48, width: frame.width, height: frame.height - 48))
        scroll.autoresizingMask = [.width, .height]
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        addSubview(scroll)
        let foot = NSView(frame: NSRect(x: 0, y: 0,
                                        width: frame.width, height: 48))
        foot.autoresizingMask = [.width]
        foot.wantsLayer = true
        foot.layer?.hbBg = "card"
        let sep = NSView(frame: NSRect(x: 0, y: 47,
                                       width: frame.width, height: 1))
        sep.wantsLayer = true
        sep.layer?.hbBg = "line"
        foot.addSubview(sep)
        addSubview(foot)
        saveBtn = NSButton(title: L("保存"), target: self,
                           action: #selector(save))
        saveBtn.bezelStyle = .rounded
        saveBtn.frame = NSRect(x: frame.width - 100, y: 10,
                               width: 80, height: 28)
        saveBtn.keyEquivalent = "\r"
        saveBtn.setAccessibilityIdentifier("hf-save")
        foot.addSubview(saveBtn)
        let cancel = NSButton(title: L("取消"), target: self,
                              action: #selector(cancel))
        cancel.bezelStyle = .rounded
        cancel.frame = NSRect(x: frame.width - 190, y: 10,
                              width: 80, height: 28)
        cancel.setAccessibilityIdentifier("hf-cancel")
        foot.addSubview(cancel)

        content.frame = NSRect(x: 0, y: 0,
                               width: frame.width, height: 10)
        scroll.documentView = content
        y = 14
        rebuild()
    }

    private var isDesktop: Bool {
        ["rdp", "vnc"].contains(selProto)
    }
    private var selProto: String {
        ["ssh", "rdp", "vnc"][max(0, protoP.indexOfSelectedItem)]
    }

    /// 布局所有字段（内容宽度固定为表单宽）。
    private func rebuild() {
        content.subviews.forEach { $0.removeFromSuperview() }
        errFor.removeAll()
        y = 14
        heading(L("基本信息"))
        let prevSel = protoP.indexOfSelectedItem
        protoP.removeAllItems()
        protoP.addItems(withTitles: ["SSH", "RDP", "VNC"])
        protoP.selectItem(at: max(0, prevSel))
        protoP.target = self; protoP.action = #selector(protoChanged)
        field(L("协议"), protoP); protoP.setAccessibilityIdentifier("hf-proto")
        field(L("别名"), aliasF); aliasF.setAccessibilityIdentifier("hf-alias")
        field(L("地址"), addrF, errKey: "addr")
        addrF.setAccessibilityIdentifier("hf-address")
        portF.placeholderString = selProto == "rdp" ? "3389"
            : selProto == "vnc" ? "5901" : "22"
        field(L("端口"), portF, errKey: "port")
        portF.setAccessibilityIdentifier("hf-port")
        field(L("用户名"), userF); userF.setAccessibilityIdentifier("hf-user")
        field(L("分组"), groupF); groupF.setAccessibilityIdentifier("hf-group")
        tagsF.placeholderString = L("逗号分隔")
        field(L("标签"), tagsF); tagsF.setAccessibilityIdentifier("hf-tags")

        if isDesktop {
            buildDesktopSections()
        } else {
            buildSshSections()
        }
        // 颜色 / 标签已在基本信息内？保持原顺序：高级段收尾
        if !isDesktop {
            buildAdvanced()
        } else {
            buildAdvancedDesktop()
        }
        content.frame = NSRect(x: 0, y: 0,
                               width: content.bounds.width, height: y + 8)
        rebuildJumpRows()
        for f in [addrF, portF, keepF, aliasF, userF] where f.delegate == nil {
            f.delegate = self
        }
        authChanged()
    }

    /// ssh 专属：认证 + 跳板（linux sec_auth / sec_jump）。
    private func buildSshSections() {
        heading(L("认证"))
        authP.removeAllItems()
        authP.addItems(withTitles: [L("自动"), L("密码"), L("密钥"),
                                    L("Agent"), L("无")])
        authP.target = self; authP.action = #selector(authChanged)
        field(L("认证"), authP); authP.setAccessibilityIdentifier("hf-auth")
        pwF.placeholderString = L("连接密码（加密保存）")
        field(L("密码"), pwF); pwF.setAccessibilityIdentifier("hf-password")
        reloadKeys()
        field(L("密钥"), keyP); keyP.setAccessibilityIdentifier("hf-key")
        let kb = NSStackView()
        kb.orientation = .horizontal; kb.spacing = 6
        let gen = NSButton(title: L("生成"), target: self,
                           action: #selector(genKeyInline))
        gen.setAccessibilityIdentifier("hf-keygen")
        let imp = NSButton(title: L("导入"), target: self,
                           action: #selector(importKeyInline))
        imp.setAccessibilityIdentifier("hf-keyimport")
        [gen, imp].forEach { $0.bezelStyle = .rounded;
                             kb.addArrangedSubview($0) }
        kb.frame = NSRect(x: pad + 100, y: y,
                          width: contentW - pad * 2 - 100, height: 24)
        content.addSubview(kb); y += 30
        idFileF.placeholderString = "~/.ssh/id_ed25519"
        field(L("密钥文件"), idFileF)
        idFileF.setAccessibilityIdentifier("hf-idfile")
        let fb = NSButton(title: L("选择…"), target: self,
                          action: #selector(chooseFile))
        fb.bezelStyle = .rounded
        fb.frame = NSRect(x: contentW - pad - 74, y: y - 30,
                          width: 74, height: 22)
        content.addSubview(fb)

        heading(L("跳板（外→内，按顺序）"))
        let jh = max(CGFloat(jumpIds.count), 1) * 26 + 4
        jumpList.orientation = .vertical; jumpList.spacing = 4
        jumpList.frame = NSRect(x: pad + 100, y: y,
                                width: contentW - pad * 2 - 100, height: jh)
        content.addSubview(jumpList)
        y += jh + 6
        jumpAddP.removeAllItems()
        jumpAddP.addItem(withTitle: L("+ 添加跳板…"))
        for hc in hostChoices where hc.id != orig?.id {
            jumpAddP.addItem(withTitle: hc.name.isEmpty ? hc.id : hc.name)
        }
        jumpAddP.target = self; jumpAddP.action = #selector(addJump)
        jumpAddP.frame = NSRect(x: pad + 100, y: y, width: 200, height: 24)
        jumpAddP.setAccessibilityIdentifier("hf-jump-add")
        content.addSubview(jumpAddP); y += 30
        let je = NSTextField(labelWithString: "")
        je.textColor = .systemRed; je.font = .systemFont(ofSize: 10)
        let jshow = errShown.contains("jump")
        je.isHidden = !jshow
        je.frame = NSRect(x: pad + 100, y: y,
                          width: contentW - pad * 2 - 100, height: 14)
        content.addSubview(je); errFor["jump"] = je
        y += jshow ? 18 : 4

    }
    private func buildAdvanced() {
        heading(L("高级"))
        reloadSnippets()
        field(L("启动片段"), snippetP)
        snippetP.setAccessibilityIdentifier("hf-snippet")
        keepF.placeholderString = L("30（0=关闭）")
        field(L("保活(秒)"), keepF, errKey: "keep")
        keepF.setAccessibilityIdentifier("hf-keepalive")
        buildColor()
    }
    /// 桌面主机的高级段：颜色（Linux 桌面主机也显示颜色/分组/OS 段，
    /// 此处颜色行与 ssh 共用）。
    private func buildAdvancedDesktop() {
        heading(L("高级"))
        buildColor()
    }
    private func buildColor() {
        colorP.removeAllItems()
        colorP.addItems(withTitles: [L("无"), "red", "orange", "amber",
                                     "green", "blue", "pink", "purple"])
        field(L("颜色"), colorP); colorP.setAccessibilityIdentifier("hf-color")
    }

    /// rdp/vnc 专属：桌面认证 + SSH 隧道（linux sec_dauth / sec_tunnel）。
    private func buildDesktopSections() {
        heading(L("桌面认证"))
        // 用户名复用「基本信息」user 字段；密码可存 Vault secret
        dpwF.placeholderString = L("密码（可空，连接时询问）")
        let pr = NSStackView()
        pr.orientation = .horizontal; pr.spacing = 6
        dpwF.setAccessibilityIdentifier("hf-dpassword")
        pr.addArrangedSubview(dpwF)
        dpwClearB.title = L("清除已保存密码")
        dpwClearB.bezelStyle = .inline
        dpwClearB.target = self; dpwClearB.action = #selector(dpwClear)
        dpwClearB.setAccessibilityIdentifier("hf-dpw-clear")
        dpwClearB.isHidden = !hadDsecret || dpwCleared
        pr.addArrangedSubview(dpwClearB)
        field("", pr)
        if selProto == "rdp" {
            domF.placeholderString = L("留空 = 本机域")
            field(L("域（RDP）"), domF)
            domF.setAccessibilityIdentifier("hf-domain")
        }
        heading(L("隧道"))
        tunnelSw.target = self; tunnelSw.action = #selector(tunnelChanged)
        field(L("通过 SSH 隧道"), tunnelSw)
        tunnelSw.setAccessibilityIdentifier("hf-tunnel")
        reloadVia()
        field(L("经由 SSH 主机"), viaP)
        viaP.setAccessibilityIdentifier("hf-via")
        viaP.target = self; viaP.action = #selector(tunnelChanged)
        fwdBtn.title = L("生成配套转发规则")
        fwdBtn.bezelStyle = .rounded
        fwdBtn.target = self; fwdBtn.action = #selector(genFwd)
        fwdBtn.toolTip = L("创建本机端口 → 此主机端口的 Local 转发，经所选 SSH 主机")
        fwdBtn.setAccessibilityIdentifier("hf-genfwd")
        let fb = NSStackView()
        fb.orientation = .vertical; fb.spacing = 4
        fb.alignment = .left
        fb.addArrangedSubview(fwdBtn)
        tunnelHint.textColor = .systemGreen
        tunnelHint.font = .systemFont(ofSize: 10)
        tunnelHint.setAccessibilityIdentifier("hf-genfwd-hint")
        fb.addArrangedSubview(tunnelHint)
        field("", fb)
        fwdBtnRefresh()
    }

    private var hadDsecret = false
    @objc private func dpwClear() {
        dpwCleared = true
        dpwClearB.isHidden = true
        dpwF.stringValue = ""
        dpwF.toolTip = L("保存后将清除已保存密码")
    }
    @objc private func protoChanged() {
        /* 端口仍停在某协议的默认值 → 跟随新协议（留空即默认） */
        let p = Int(portF.stringValue)
        if p == nil || [22, 3389, 5901].contains(p ?? 0) {
            portF.stringValue = ""
        }
        rebuild(); validate()
    }
    @objc private func tunnelChanged() { fwdBtnRefresh() }
    private func fwdBtnRefresh() {
        viaP.isEnabled = tunnelSw.state == .on
        fwdBtn.isEnabled = tunnelSw.state == .on
            && viaP.indexOfSelectedItem > 0
    }
    private func reloadVia() {
        let sel = viaP.indexOfSelectedItem > 0
            ? viaIds[safe: viaP.indexOfSelectedItem] ?? nil : nil
        viaP.removeAllItems()
        viaP.addItem(withTitle: L("（无）"))
        viaIds = [nil]
        for hc in hostChoices where hc.id != orig?.id {
            viaP.addItem(withTitle: hc.name.isEmpty ? hc.id : hc.name)
            viaIds.append(hc.id)
        }
        if let sel, let i = viaIds.firstIndex(where: { $0 == sel }) {
            viaP.selectItem(at: i)
        }
    }
    /// 「生成配套转发规则」：10000+dest_port 起向上找空闲端口建 local 规则。
    @objc private func genFwd() {
        let vi = viaP.indexOfSelectedItem
        guard vi > 0, vi < viaIds.count, let vid = viaIds[vi]
        else { tunnelHint.stringValue = L("请先选择经由 SSH 主机"); return }
        let dport = Int(portF.stringValue)
            ?? (selProto == "rdp" ? 3389 : 5901)
        var p = dport + 10000
        if p > 65535 { p = 10000 }
        let fwds = Vault.shared.list("forward")
        defer { fwds.forEach { hb_obj_free($0) } }
        var bport = 0
        while p <= 65535 {
            let clash = fwds.contains { o in
                hb_obj_get_str(o, "kind").str != "remote"
                    && Int(hb_obj_get_int(o, "bind_port", 0)) == p
            }
            if !clash && hb_pf_probe_bind("127.0.0.1", Int32(p), nil)
                    == HB_OK { bport = p; break }
            p += 1
        }
        guard bport > 0 else {
            tunnelHint.stringValue = L("找不到空闲监听端口"); return
        }
        guard let o = Vault.shared.newObj("forward") else { return }
        hb_obj_set_str(o, "kind", "local")
        hb_obj_set_str(o, "bind_addr", "127.0.0.1")
        hb_obj_set_int(o, "bind_port", Int64(bport))
        hb_obj_set_str(o, "dest_addr", "127.0.0.1")
        hb_obj_set_int(o, "dest_port", Int64(dport))
        hb_obj_set_str(o, "via", vid)
        hb_obj_set_bool(o, "autostart", false)
        hb_obj_set_bool(o, "enabled", false)
        hb_obj_set_int(o, "order", Int64(Date().timeIntervalSince1970))
        Vault.shared.put(o); hb_obj_free(o)
        var vname = vid
        if let vh = Vault.shared.get(vid) {
            let nm = hb_obj_get_str(vh, "name").str
            if !nm.isEmpty { vname = nm }
            hb_obj_free(vh)
        }
        tunnelHint.stringValue = String(format:
            L("已创建转发规则 127.0.0.1:%d → 127.0.0.1:%d（经 %@）"),
            bport, dport, vname)
    }
    override func resize(withOldSuperviewSize s: NSSize) {
        super.resize(withOldSuperviewSize: s)
        if content.superview != nil { rebuild() }
    }

    func controlTextDidChange(_ n: Notification) { validate() }

    private func reloadKeys() {
        let sel = keyP.indexOfSelectedItem >= 0
            ? keyIds[safe: keyP.indexOfSelectedItem] ?? "" : ""
        keyP.removeAllItems()
        keyP.addItem(withTitle: L("（无）"))
        keyIds = [""]
        for o in Vault.shared.list("key") {
            keyP.addItem(withTitle: hb_obj_get_str(o, "name").str)
            keyIds.append(hb_obj_id(o).str)
            hb_obj_free(o)
        }
        if let i = keyIds.firstIndex(of: sel) { keyP.selectItem(at: i) }
    }
    private func reloadSnippets() {
        let sel = snippetP.selectedItem?.title
        snippetP.removeAllItems()
        snippetP.addItem(withTitle: L("（无）"))
        for o in Vault.shared.list("snippet") {
            snippetP.addItem(withTitle: hb_obj_get_str(o, "name").str)
            hb_obj_free(o)
        }
        if let sel { snippetP.selectItem(withTitle: sel) }
    }

    @objc private func authChanged() {
        let i = max(0, authP.indexOfSelectedItem)
        let a = ["auto", "password", "key", "agent", "none"][i]
        pwF.isEnabled = (a == "password")
        keyP.isEnabled = (a == "key" || a == "auto")
        idFileF.isEnabled = (a == "key" || a == "auto")
    }

    // MARK: 跳板
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
    private func emptyJumpRow() {
        let l = NSTextField(labelWithString: L("无跳板 — 从下方添加"))
        l.textColor = .tertiaryLabelColor
        l.font = .systemFont(ofSize: 11)
        jumpList.addArrangedSubview(l)
    }
    @objc private func addJump() {
        let i = jumpAddP.indexOfSelectedItem
        jumpAddP.selectItem(at: 0)
        guard i > 0 else { return }
        let avail = hostChoices.filter { $0.id != orig?.id }
        guard i - 1 < avail.count else { return }
        let id = avail[i - 1].id
        guard !jumpIds.contains(id) else { return }
        jumpIds.append(id)
        rebuild()
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
        jumpIds.remove(at: i); rebuild(); validate()
    }
    private func rebuildJumpRows() {
        jumpList.arrangedSubviews.forEach { $0.removeFromSuperview() }
        if jumpIds.isEmpty { emptyJumpRow() }
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
    // 内联生成：与密钥页同对话框，生成后入库并选中
    @objc private func genKeyInline() {
        let a = NSAlert()
        a.messageText = L("生成密钥")
        a.addButton(withTitle: L("生成")); a.addButton(withTitle: L("取消"))
        let name = NSTextField(frame: NSRect(x: 0, y: 76,
                                             width: 240, height: 22))
        name.placeholderString = L("名称")
        name.setAccessibilityIdentifier("kgen-name")
        let type = NSPopUpButton(frame: NSRect(x: 0, y: 46,
                                               width: 240, height: 24))
        type.addItems(withTitles: ["Ed25519", "ECDSA", "RSA 3072", "RSA 4096"])
        type.setAccessibilityIdentifier("kgen-type")
        let pp = NSSecureTextField(frame: NSRect(x: 0, y: 20,
                                                 width: 240, height: 22))
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
            let kid = KeyOps.generate(type: t, bits: bits,
                                      name: name.stringValue,
                                      pass: pp.stringValue)
            pp.stringValue = ""
            self.reloadKeys()
            if let kid, let i = self.keyIds.firstIndex(of: kid) {
                self.keyP.selectItem(at: i)
            }
        }
    }
    // 内联导入：选文件 → 口令 → 入库并选中
    @objc private func importKeyInline() {
        let op = NSOpenPanel()
        guard op.runModal() == .OK, let u = op.url else { return }
        let a = NSAlert()
        a.messageText = L("私钥口令（无口令留空）")
        a.addButton(withTitle: L("导入")); a.addButton(withTitle: L("取消"))
        let pp = NSSecureTextField(frame: NSRect(x: 0, y: 0,
                                                 width: 240, height: 22))
        pp.setAccessibilityIdentifier("kimp-pass")
        a.accessoryView = pp
        DialogRegistry.shared.present(alert: a, kind: "kimport",
                                      in: window) { [weak self] ok in
            guard ok, let self else { return }
            let kid = KeyOps.doImport(path: u.path, pass: pp.stringValue,
                                      window: self.window)
            self.reloadKeys()
            if let kid, let i = self.keyIds.firstIndex(of: kid) {
                self.keyP.selectItem(at: i)
            }
        }
    }

    private func validate() {
        var errs: [String: String] = [:]
        if addrF.stringValue.trimmingCharacters(in: .whitespaces).isEmpty {
            errs["addr"] = L("地址必填")
        }
        if !portF.stringValue.isEmpty {
            if Int(portF.stringValue) == nil {
                errs["port"] = L("端口须为数字")
            } else if !(1...65535).contains(Int(portF.stringValue)!) {
                errs["port"] = L("端口须为 1–65535")
            }
        }
        if !keepF.stringValue.isEmpty,
           (Int(keepF.stringValue) ?? -1) < 0 {
            errs["keep"] = L("保活须 ≥0")
        }
        if jumpCycle() { errs["jump"] = L("跳板链形成环") }
        for (k, e) in errFor { e.stringValue = errs[k] ?? "" }
        saveBtn.isEnabled = errs.isEmpty
        // 错误集合变化 → rebuild 折叠/展开对应错误行（不改控件值）；
        // rebuild 会移除再挂载控件，先记下正在编辑的字段再恢复焦点
        if Set(errs.keys) != errShown {
            errShown = Set(errs.keys)
            let editing: NSTextField? = [addrF, portF, keepF, aliasF, userF,
                                         groupF, tagsF, idFileF]
                .first { window?.firstResponder === $0.currentEditor() }
            rebuild()
            for (k, e) in errFor { e.stringValue = errs[k] ?? "" }
            if let f = editing {
                window?.makeFirstResponder(f)
                f.currentEditor()?.moveToEndOfLine(nil)
            }
        }
    }

    func fill(_ h: HostRec) {
        orig = h
        rebuild()
        protoP.selectItem(at: ["ssh", "rdp", "vnc"]
            .firstIndex(of: h.proto) ?? 0)
        rebuild()               // 按协议铺开对应分区
        hadDsecret = Vault.shared.secret(h.id, "password") != nil
        dpwClearB.isHidden = !hadDsecret || dpwCleared
        domF.stringValue = h.domain
        tunnelSw.state = h.tunnel == "ssh" ? .on : .off
        if let i = viaIds.firstIndex(where: { $0 == h.via }) {
            viaP.selectItem(at: i)
        }
        fwdBtnRefresh()
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
        reloadKeys()
        if let i = keyIds.firstIndex(of: h.keyId) { keyP.selectItem(at: i) }
        if let i = snippetP.itemTitles.firstIndex(of: h.snippetId) {
            snippetP.selectItem(at: i)
        }
        if let i = ["", "red", "orange", "amber", "green", "blue",
                    "pink", "purple"].firstIndex(of: h.color) {
            colorP.selectItem(at: i)
        }
        jumpIds = h.jumps
        rebuild()
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
        h.proto = selProto
        let dport = Int(portF.stringValue)
        h.port = dport ?? (selProto == "rdp" ? 3389
                           : selProto == "vnc" ? 5901 : 22)
        if isDesktop {
            h.domain = selProto == "rdp" ? domF.stringValue : ""
            h.tunnel = tunnelSw.state == .on ? "ssh" : ""
            h.via = tunnelSw.state == .on
                ? (viaIds[safe: viaP.indexOfSelectedItem] ?? nil) ?? "" : ""
            /* ssh 专属字段清空 */
            h.auth = "auto"; h.keyId = ""; h.identityFile = ""
            h.jumps = []; h.snippetId = ""; h.keepalive = -1
        } else {
            h.domain = ""; h.tunnel = ""; h.via = ""
        }
        return h
    }
    @objc private func save() {
        let h = rec
        guard !h.address.isEmpty else { NSSound.beep(); return }
        if isDesktop {
            /* 桌面密码：空 = 保留已保存；dpwCleared = 显式清除 */
            let pw = dpwF.stringValue.isEmpty ? nil : dpwF.stringValue
            dpwF.stringValue = ""
            if dpwCleared, !h.id.isEmpty {
                Vault.shared.vault.map {
                    _ = hb_vault_set_secret($0, h.id, "password", nil)
                }
            }
            onSave?(h, pw)
        } else {
            let pw = pwF.stringValue.isEmpty ? nil : pwF.stringValue
            pwF.stringValue = ""
            onSave?(h, pw)
        }
    }
    @objc private func cancel() { onCancel?() }
}

/// 顶部锚定的 documentView（isFlipped：y 自顶向下增长）。
final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}

private extension Array {
    subscript(safe i: Int) -> Element? {
        indices.contains(i) ? self[i] : nil
    }
}
