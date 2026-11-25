import AppKit
import CHarbor
import HarborKit

/// 新建/编辑主机表单（对齐 prototype .modal + PRD FR-01/FR-07 全部字段）：
/// 520px 圆角白底弹窗，顶部标题 + × 关闭；标签在上方、两栏 field-row；
/// 底部 E2EE hintbox + 取消 / 保存按钮条。
final class HostForm: NSView, NSTextFieldDelegate {
    var onSave: ((HostRec, String?) -> Void)?   // (rec, password?)
    var onCancel: (() -> Void)?
    private var orig: HostRec?
    private let modalTitle: String

    private let aliasF = NSTextField()
    private let protoP = NSPopUpButton()          // ssh / rdp / vnc
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
    private var saveBtn: HBtn!
    private var titleL: NSTextField!
    // 桌面（rdp/vnc）字段：桌面认证 + SSH 隧道
    private let dpwF = NSSecureTextField()
    private let dpwClearB = HBtn(L("清除已保存密码"), style: .gray, sm: true)
    private var dpwCleared = false
    private let domF = NSTextField()
    private let tunnelSw = NSSwitch()
    private let viaP = NSPopUpButton()
    private var viaIds: [String?] = []
    private let fwdBtn = HBtn(L("生成配套转发规则"), style: .gray, sm: true)
    private let tunnelHint = NSTextField(labelWithString: "")
    private var keyIds: [String] = []
    private var hostChoices: [(id: String, name: String)] = []
    /// 字段 → 错误标签
    private var errFor: [String: NSTextField] = [:]
    /// 当前展示的错误集合；无错误的字段错误行整体折叠不占位
    private var errShown: Set<String> = []
    private let content = FlippedView()

    init(frame: NSRect, title: String = "New host") {
        self.modalTitle = title
        super.init(frame: frame)
        build()
        validate()
    }
    required init?(coder: NSCoder) { fatalError() }

    override func cancelOperation(_ sender: Any?) { cancel() }

    /// 内容布局游标
    private var y: CGFloat = 0
    private let pad: CGFloat = 22
    private var contentW: CGFloat { content.bounds.width }

    // MARK: - 原型 .field / .field-row / .hintbox 构建器

    /// 原型 .field-box：圆角 8、tableHead 底、line 描边、聚焦蓝框。
    private func styleInput(_ tf: NSTextField, placeholder: String = "",
                            h: CGFloat = 36) -> NSView {
        let box = ModalFieldBox(frame: NSRect(x: 0, y: 0, width: 100, height: h))
        tf.isBordered = false
        tf.drawsBackground = false
        tf.focusRingType = .none
        tf.font = .systemFont(ofSize: 13)
        tf.hbText = "text"
        tf.placeholderString = placeholder
        tf.lineBreakMode = .byTruncatingTail
        tf.frame = NSRect(x: 11, y: (h - 18) / 2, width: 78, height: 18)
        tf.autoresizingMask = [.width]
        box.addSubview(tf)
        return box
    }

    private func styleSelect(_ pop: NSPopUpButton, h: CGFloat = 36) -> NSView {
        let box = ModalFieldBox(frame: NSRect(x: 0, y: 0, width: 100, height: h))
        pop.isBordered = false
        pop.font = .systemFont(ofSize: 13)
        pop.frame = NSRect(x: 6, y: (h - 24) / 2, width: 88, height: 24)
        pop.autoresizingMask = [.width]
        box.addSubview(pop)
        return box
    }

    private func makeLabel(_ t: String) -> NSTextField {
        let l = NSTextField(labelWithString: t)
        l.font = .systemFont(ofSize: 12, weight: .semibold)
        l.hbText = "text2"
        return l
    }

    private func heading(_ t: String) {
        let sep = NSView(frame: NSRect(x: pad, y: y + 2,
                                       width: contentW - pad * 2, height: 1))
        sep.wantsLayer = true
        sep.layer?.hbBg = "rowLine"
        content.addSubview(sep)
        let l = NSTextField(labelWithString: t)
        l.font = .systemFont(ofSize: 11.5, weight: .bold)
        l.hbText = "text3"
        l.frame = NSRect(x: pad, y: y + 10, width: 300, height: 16)
        content.addSubview(l)
        y += 32
    }

    /// 单列字段（标签在上方，可选错误行紧随其下）。
    private func field(_ t: String, _ v: NSView, h: CGFloat = 36,
                       errKey: String? = nil) {
        let w = contentW - pad * 2
        if !t.isEmpty {
            let l = makeLabel(t)
            l.frame = NSRect(x: pad, y: y, width: w, height: 16)
            content.addSubview(l)
            y += 21
        }
        v.frame = NSRect(x: pad, y: y, width: w, height: h)
        content.addSubview(v)
        y += h
        if let k = errKey {
            let e = NSTextField(labelWithString: "")
            e.textColor = Theme.red.color
            e.font = .systemFont(ofSize: 10.5)
            let show = errShown.contains(k)
            e.isHidden = !show
            e.frame = NSRect(x: pad, y: y + 2, width: w, height: 14)
            content.addSubview(e)
            errFor[k] = e
            if show { y += 16 }
        }
        y += 12
    }

    /// 原型 .field-row：两栏并排，支持自定义比例（如 2:1）。
    private func fieldRow(
        _ left: (label: String, view: NSView, weight: CGFloat, errKey: String?),
        _ right: (label: String, view: NSView, weight: CGFloat, errKey: String?),
        h: CGFloat = 36
    ) {
        let gap: CGFloat = 12
        let totalW = contentW - pad * 2 - gap
        let sum = left.weight + right.weight
        let lw = floor(totalW * left.weight / sum)
        let rw = totalW - lw
        let rx = pad + lw + gap

        let ll = makeLabel(left.label)
        ll.frame = NSRect(x: pad, y: y, width: lw, height: 16)
        content.addSubview(ll)
        let rl = makeLabel(right.label)
        rl.frame = NSRect(x: rx, y: y, width: rw, height: 16)
        content.addSubview(rl)
        y += 21

        left.view.frame = NSRect(x: pad, y: y, width: lw, height: h)
        content.addSubview(left.view)
        right.view.frame = NSRect(x: rx, y: y, width: rw, height: h)
        content.addSubview(right.view)
        y += h

        var hasErr = false
        for (item, x, w) in [(left, pad, lw), (right, rx, rw)] {
            if let k = item.errKey {
                let e = NSTextField(labelWithString: "")
                e.textColor = Theme.red.color
                e.font = .systemFont(ofSize: 10.5)
                let show = errShown.contains(k)
                e.isHidden = !show
                e.frame = NSRect(x: x, y: y + 2, width: w, height: 14)
                content.addSubview(e)
                errFor[k] = e
                if show { hasErr = true }
            }
        }
        if hasErr { y += 16 }
        y += 12
    }

    private func build() {
        wantsLayer = true
        layer?.cornerRadius = 14
        layer?.masksToBounds = true
        layer?.hbBg = "card"

        hostChoices = Vault.shared.list("host").map { o in
            defer { hb_obj_free(o) }
            return (hb_obj_id(o).str, hb_obj_get_str(o, "name").str)
        }

        // ── .modal-head：标题 + × 关闭 ──
        let head = NSView(frame: NSRect(x: 0, y: frame.height - 52,
                                        width: frame.width, height: 52))
        head.autoresizingMask = [.width, .minYMargin]
        titleL = NSTextField(labelWithString: modalTitle)
        titleL.font = .boldSystemFont(ofSize: 16)
        titleL.hbText = "text"
        titleL.frame = NSRect(x: pad, y: 14, width: frame.width - pad * 2 - 36, height: 22)
        titleL.autoresizingMask = [.width]
        head.addSubview(titleL)
        let closeBtn = NSButton(title: "", target: self, action: #selector(cancel))
        closeBtn.isBordered = false
        closeBtn.image = hbIcon("x", size: 13, tint: Theme.dyn("text3"))
        closeBtn.frame = NSRect(x: frame.width - pad - 28, y: 12, width: 28, height: 28)
        closeBtn.autoresizingMask = [.minXMargin]
        closeBtn.wantsLayer = true
        closeBtn.layer?.cornerRadius = 6
        head.addSubview(closeBtn)
        addSubview(head)

        // ── .modal-foot：右对齐 取消 + 保存 ──
        let foot = NSView(frame: NSRect(x: 0, y: 0,
                                        width: frame.width, height: 60))
        foot.autoresizingMask = [.width]
        foot.wantsLayer = true
        foot.layer?.hbBg = "card"
        let sep = NSView(frame: NSRect(x: 0, y: 59,
                                       width: frame.width, height: 1))
        sep.wantsLayer = true
        sep.layer?.hbBg = "line"
        foot.addSubview(sep)
        addSubview(foot)

        saveBtn = HBtn(L("保存"), style: .primary)
        saveBtn.target = self; saveBtn.action = #selector(save)
        saveBtn.frame = NSRect(x: frame.width - pad - 78, y: 14,
                               width: 78, height: 32)
        saveBtn.autoresizingMask = [.minXMargin]
        saveBtn.keyEquivalent = "\r"
        saveBtn.setAccessibilityIdentifier("hf-save")
        foot.addSubview(saveBtn)

        let cancelBtn = HBtn(L("取消"), style: .gray)
        cancelBtn.target = self; cancelBtn.action = #selector(cancel)
        cancelBtn.frame = NSRect(x: frame.width - pad - 78 - 9 - 78, y: 14,
                                 width: 78, height: 32)
        cancelBtn.autoresizingMask = [.minXMargin]
        cancelBtn.setAccessibilityIdentifier("hf-cancel")
        foot.addSubview(cancelBtn)

        // ── .modal-body：滚动区 ──
        let scroll = NSScrollView(frame: NSRect(
            x: 0, y: 60, width: frame.width, height: frame.height - 112))
        scroll.autoresizingMask = [.width, .height]
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        addSubview(scroll)

        content.frame = NSRect(x: 0, y: 0, width: frame.width, height: 10)
        scroll.documentView = content
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
        y = 6

        // 第 1 行：类型 | 别名
        let prevSel = protoP.indexOfSelectedItem
        protoP.removeAllItems()
        protoP.addItems(withTitles: ["SSH", "RDP", "VNC"])
        protoP.selectItem(at: max(0, prevSel))
        protoP.target = self; protoP.action = #selector(protoChanged)
        protoP.setAccessibilityIdentifier("hf-proto")
        aliasF.setAccessibilityIdentifier("hf-alias")
        fieldRow(
            (L("类型"), styleSelect(protoP), 1, nil),
            (L("别名"), styleInput(aliasF, placeholder: "my-server"), 1, nil)
        )

        // 第 2 行：地址 (2) | 端口 (1)
        addrF.setAccessibilityIdentifier("hf-address")
        portF.setAccessibilityIdentifier("hf-port")
        let defPort = selProto == "rdp" ? "3389" : selProto == "vnc" ? "5901" : "22"
        fieldRow(
            (L("地址"), styleInput(addrF, placeholder: "10.0.0.5"), 2, "addr"),
            (L("端口"), styleInput(portF, placeholder: defPort), 1, "port")
        )

        // 第 3 行：用户名 | 分组
        userF.setAccessibilityIdentifier("hf-user")
        groupF.setAccessibilityIdentifier("hf-group")
        fieldRow(
            (L("用户名"), styleInput(userF, placeholder: "root"), 1, nil),
            (L("分组"), styleInput(groupF, placeholder: L("未分组")), 1, nil)
        )

        if isDesktop {
            buildDesktopSections()
        } else {
            buildSshSections()
        }

        if !isDesktop {
            buildAdvanced()
        } else {
            buildAdvancedDesktop()
        }

        // ── 原型 .hintbox：E2EE 同步提示 ──
        let w = contentW - pad * 2
        let hb = NSView(frame: NSRect(x: pad, y: y + 4, width: w, height: 38))
        hb.wantsLayer = true
        hb.layer?.cornerRadius = 9
        hb.layer?.borderWidth = 1
        hb.layer?.hbBg = "hintBlueBg"
        hb.layer?.hbBorder = "hintBlueLine"
        let hl = NSTextField(labelWithString: L("保存后凭据将经 E2EE 同步到你的所有设备（FR-11）。"))
        hl.font = .systemFont(ofSize: 12)
        hl.hbText = "hintBlueFg"
        hl.frame = NSRect(x: 13, y: 10, width: w - 26, height: 18)
        hb.addSubview(hl)
        content.addSubview(hb)
        y += 54

        content.frame = NSRect(x: 0, y: 0,
                               width: content.bounds.width, height: y + 8)
        rebuildJumpRows()
        for f in [addrF, portF, keepF, aliasF, userF, groupF, tagsF, idFileF]
            where f.delegate == nil {
            f.delegate = self
        }
        authChanged()
    }

    /// ssh 专属：跳板（ProxyJump）+ 认证。
    private func buildSshSections() {
        // 跳板机 (ProxyJump)
        let w = contentW - pad * 2
        let jl = makeLabel(L("跳板机 (ProxyJump)"))
        jl.frame = NSRect(x: pad, y: y, width: w, height: 16)
        content.addSubview(jl); y += 21

        let jh = max(CGFloat(jumpIds.count), 1) * 26 + 4
        jumpList.orientation = .vertical; jumpList.spacing = 4
        jumpList.frame = NSRect(x: pad, y: y, width: w, height: jh)
        content.addSubview(jumpList)
        y += jh + 4

        jumpAddP.removeAllItems()
        jumpAddP.addItem(withTitle: L("+ 添加跳板…"))
        for hc in hostChoices where hc.id != orig?.id {
            jumpAddP.addItem(withTitle: hc.name.isEmpty ? hc.id : hc.name)
        }
        jumpAddP.target = self; jumpAddP.action = #selector(addJump)
        jumpAddP.setAccessibilityIdentifier("hf-jump-add")
        let jumpSel = styleSelect(jumpAddP, h: 34)
        jumpSel.frame = NSRect(x: pad, y: y, width: w, height: 34)
        content.addSubview(jumpSel); y += 36

        let je = NSTextField(labelWithString: "")
        je.textColor = Theme.red.color; je.font = .systemFont(ofSize: 10.5)
        let jshow = errShown.contains("jump")
        je.isHidden = !jshow
        je.frame = NSRect(x: pad, y: y, width: w, height: 14)
        content.addSubview(je); errFor["jump"] = je
        y += jshow ? 22 : 10

        // 认证
        heading(L("认证"))
        authP.removeAllItems()
        authP.addItems(withTitles: [L("自动"), L("密码"), L("密钥"),
                                    L("Agent"), L("无")])
        authP.target = self; authP.action = #selector(authChanged)
        authP.setAccessibilityIdentifier("hf-auth")
        pwF.setAccessibilityIdentifier("hf-password")
        fieldRow(
            (L("认证方式"), styleSelect(authP), 1, nil),
            (L("密码"), styleInput(pwF, placeholder: L("连接密码（加密保存）")), 1, nil)
        )

        reloadKeys()
        keyP.setAccessibilityIdentifier("hf-key")
        let keyLabel = makeLabel(L("密钥"))
        keyLabel.frame = NSRect(x: pad, y: y, width: w, height: 16)
        content.addSubview(keyLabel); y += 21
        let keyBox = styleSelect(keyP)
        keyBox.frame = NSRect(x: pad, y: y, width: w - 136, height: 36)
        content.addSubview(keyBox)
        let gen = HBtn(L("生成"), style: .gray, sm: true)
        gen.target = self; gen.action = #selector(genKeyInline)
        gen.frame = NSRect(x: contentW - pad - 128, y: y + 2, width: 60, height: 32)
        gen.setAccessibilityIdentifier("hf-keygen")
        content.addSubview(gen)
        let imp = HBtn(L("导入"), style: .gray, sm: true)
        imp.target = self; imp.action = #selector(importKeyInline)
        imp.frame = NSRect(x: contentW - pad - 60, y: y + 2, width: 60, height: 32)
        imp.setAccessibilityIdentifier("hf-keyimport")
        content.addSubview(imp)
        y += 48

        idFileF.setAccessibilityIdentifier("hf-idfile")
        let idLabel = makeLabel(L("密钥文件"))
        idLabel.frame = NSRect(x: pad, y: y, width: w, height: 16)
        content.addSubview(idLabel); y += 21
        let idBox = styleInput(idFileF, placeholder: "~/.ssh/id_ed25519")
        idBox.frame = NSRect(x: pad, y: y, width: w - 82, height: 36)
        content.addSubview(idBox)
        let fb = HBtn(L("选择…"), style: .gray, sm: true)
        fb.target = self; fb.action = #selector(chooseFile)
        fb.frame = NSRect(x: contentW - pad - 74, y: y + 2, width: 74, height: 32)
        content.addSubview(fb)
        y += 48
    }

    private func buildAdvanced() {
        heading(L("高级"))
        reloadSnippets()
        snippetP.setAccessibilityIdentifier("hf-snippet")
        keepF.setAccessibilityIdentifier("hf-keepalive")
        fieldRow(
            (L("启动片段"), styleSelect(snippetP), 1, nil),
            (L("保活(秒)"), styleInput(keepF, placeholder: L("30（0=关闭）")), 1, "keep")
        )
        buildColorAndTags()
    }

    private func buildAdvancedDesktop() {
        heading(L("高级"))
        buildColorAndTags()
    }

    private func buildColorAndTags() {
        colorP.removeAllItems()
        colorP.addItems(withTitles: [L("无"), "red", "orange", "amber",
                                     "green", "blue", "pink", "purple"])
        colorP.setAccessibilityIdentifier("hf-color")
        tagsF.setAccessibilityIdentifier("hf-tags")
        fieldRow(
            (L("颜色标签"), styleSelect(colorP), 1, nil),
            (L("标签"), styleInput(tagsF, placeholder: L("逗号分隔")), 1, nil)
        )
    }

    /// rdp/vnc 专属：原型 .check-row 隧道开关 + 经由 SSH 主机 + 桌面认证。
    private func buildDesktopSections() {
        let w = contentW - pad * 2
        // 原型 .check-row：通过 SSH 隧道连接（回环穿透）
        let cr = NSView(frame: NSRect(x: pad, y: y, width: w, height: 42))
        let ct = NSTextField(labelWithString: L("通过 SSH 隧道连接（回环穿透）"))
        ct.font = .systemFont(ofSize: 13, weight: .semibold)
        ct.hbText = "text"
        ct.frame = NSRect(x: 0, y: 20, width: w - 54, height: 18)
        cr.addSubview(ct)
        let cs = NSTextField(labelWithString: L("目标只监听 127.0.0.1 时，借下方 SSH 主机自动建立 Local 转发"))
        cs.font = .systemFont(ofSize: 11.5)
        cs.hbText = "text3"
        cs.lineBreakMode = .byTruncatingTail
        cs.frame = NSRect(x: 0, y: 2, width: w - 54, height: 16)
        cr.addSubview(cs)
        tunnelSw.target = self; tunnelSw.action = #selector(tunnelChanged)
        tunnelSw.setAccessibilityIdentifier("hf-tunnel")
        tunnelSw.frame = NSRect(x: w - 42, y: 9, width: 42, height: 24)
        cr.addSubview(tunnelSw)
        content.addSubview(cr)
        y += 52

        reloadVia()
        viaP.setAccessibilityIdentifier("hf-via")
        viaP.target = self; viaP.action = #selector(tunnelChanged)
        let viaLabel = makeLabel(L("经由 SSH 主机"))
        viaLabel.frame = NSRect(x: pad, y: y, width: w, height: 16)
        content.addSubview(viaLabel); y += 21
        let viaBox = styleSelect(viaP)
        viaBox.frame = NSRect(x: pad, y: y, width: w - 134, height: 36)
        content.addSubview(viaBox)
        fwdBtn.target = self; fwdBtn.action = #selector(genFwd)
        fwdBtn.toolTip = L("创建本机端口 → 此主机端口的 Local 转发，经所选 SSH 主机")
        fwdBtn.frame = NSRect(x: contentW - pad - 126, y: y + 2, width: 126, height: 32)
        fwdBtn.setAccessibilityIdentifier("hf-genfwd")
        content.addSubview(fwdBtn)
        y += 40
        tunnelHint.textColor = Theme.green.color
        tunnelHint.font = .systemFont(ofSize: 11)
        tunnelHint.setAccessibilityIdentifier("hf-genfwd-hint")
        tunnelHint.frame = NSRect(x: pad, y: y, width: w, height: 16)
        content.addSubview(tunnelHint)
        y += 22

        heading(L("桌面认证"))
        dpwF.setAccessibilityIdentifier("hf-dpassword")
        dpwClearB.target = self; dpwClearB.action = #selector(dpwClear)
        dpwClearB.setAccessibilityIdentifier("hf-dpw-clear")
        dpwClearB.isHidden = !hadDsecret || dpwCleared
        let pwW = dpwClearB.isHidden ? w : (w - 118)
        let pwLabel = makeLabel(L("密码"))
        pwLabel.frame = NSRect(x: pad, y: y, width: w, height: 16)
        content.addSubview(pwLabel); y += 21
        let dpwBox = styleInput(dpwF, placeholder: L("密码（可空，连接时询问）"))
        dpwBox.frame = NSRect(x: pad, y: y, width: pwW, height: 36)
        content.addSubview(dpwBox)
        if !dpwClearB.isHidden {
            dpwClearB.frame = NSRect(x: contentW - pad - 110, y: y + 2, width: 110, height: 32)
            content.addSubview(dpwClearB)
        }
        y += 48

        if selProto == "rdp" {
            domF.setAccessibilityIdentifier("hf-domain")
            field(L("域（RDP）"), styleInput(domF, placeholder: L("留空 = 本机域")))
        }
        fwdBtnRefresh()
    }

    private var hadDsecret = false
    @objc private func dpwClear() {
        dpwCleared = true
        dpwClearB.isHidden = true
        dpwF.stringValue = ""
        dpwF.toolTip = L("保存后将清除已保存密码")
        rebuild()
    }
    @objc private func protoChanged() {
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
        l.hbText = "text"
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
        l.hbText = "text3"
        l.font = .systemFont(ofSize: 11.5)
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
        rebuild()
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

/// 原型 .field input/select 容器：圆角 8、#fbfcfd 底、1px 描边。
private final class ModalFieldBox: NSView {
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.borderWidth = 1
        layer?.hbBg = "tableHead"
        layer?.hbBorder = "line"
    }
    required init?(coder: NSCoder) { fatalError() }
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
