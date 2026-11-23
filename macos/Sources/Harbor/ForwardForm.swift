import AppKit
import CHarbor
import HarborKit

/// FR-06 转发规则表单 —— 对齐 linux/src/hb-forward-form.c：
/// 类型卡 + 说明、绑定/目标字段（dynamic 隐藏目标）、经由主机下拉、
/// ssh 等效命令预览、随主机连接自动启动、绑定冲突行内红字阻止保存
/// （300ms 防抖 + 保存时复查）。footer：取消 / 保存 / 保存并启动。
final class ForwardForm: NSView, NSTextFieldDelegate {
    /// (start, obj)：保存交出 vault 对象（未 put）；取消 obj=nil。
    var onDone: ((Bool, OpaquePointer?) -> Void)?
    private var obj: OpaquePointer?          // 编辑对象（借用，不 free）
    private var kind: FwdKit.Kind = .local
    private var conflict = false
    private var probeTimer: Timer?
    private var viaIds: [String] = []

    private let seg = NSSegmentedControl()
    private let explain = NSTextField(wrappingLabelWithString: "")
    private let bindE = NSTextField()
    private let portE = NSTextField()
    private let destE = NSTextField()
    private let dportE = NSTextField()
    private var destRow: NSStackView!
    private let viaDD = NSPopUpButton()
    private let hintL = NSTextField(wrappingLabelWithString: "")
    private let cmdL = NSTextField(wrappingLabelWithString: "")
    private let autoSw = NSSwitch()
    private let errL = NSTextField(wrappingLabelWithString: "")
    private var saveBtn: NSButton!
    private var saveStartBtn: NSButton!

    override init(frame f: NSRect) {
        super.init(frame: f)
        let pad: CGFloat = 18
        let col = NSStackView()
        col.orientation = .vertical
        col.alignment = .leading
        col.spacing = 10
        col.translatesAutoresizingMaskIntoConstraints = false
        col.edgeInsets = NSEdgeInsets(top: 16, left: pad,
                                    bottom: 14, right: pad)
        addSubview(col)
        col.widthAnchor.constraint(
            equalTo: widthAnchor, constant: -2 * pad).isActive = true
        col.leadingAnchor.constraint(equalTo: leadingAnchor,
                                     constant: pad).isActive = true
        col.topAnchor.constraint(equalTo: topAnchor,
                                 constant: 16).isActive = true

        // 类型
        seg.segmentCount = 3
        seg.segmentStyle = .rounded
        seg.setLabel(FwdKit.Kind.local.label, forSegment: 0)
        seg.setLabel(FwdKit.Kind.remote.label, forSegment: 1)
        seg.setLabel(FwdKit.Kind.dynamic.label, forSegment: 2)
        seg.target = self
        seg.action = #selector(onKind)
        seg.selectedSegment = 0
        seg.setAccessibilityIdentifier("fwdf-kind")
        col.addArrangedSubview(seg)
        explain.font = .systemFont(ofSize: 11.5)
        explain.textColor = .secondaryLabelColor
        col.addArrangedSubview(explain)

        func field(_ t: NSTextField, _ ph: String, _ w: CGFloat)
            -> NSTextField {
            t.placeholderString = ph
            t.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
            t.widthAnchor.constraint(equalToConstant: w).isActive = true
            t.delegate = self
            return t
        }
        func label(_ s: String) -> NSTextField {
            let l = NSTextField(labelWithString: s)
            l.font = .systemFont(ofSize: 12)
            return l
        }
        // 绑定行
        let bindRow = NSStackView()
        bindRow.orientation = .horizontal
        bindRow.spacing = 8
        bindRow.addArrangedSubview(label(L("绑定")))
        bindRow.addArrangedSubview(field(bindE, "127.0.0.1", 150))
        bindRow.addArrangedSubview(label(":"))
        bindRow.addArrangedSubview(field(portE, "13306", 70))
        col.addArrangedSubview(bindRow)
        portE.stringValue = "13306"
        // 目标行
        destRow = NSStackView()
        destRow.orientation = .horizontal
        destRow.spacing = 8
        destRow.addArrangedSubview(label(L("目标")))
        destRow.addArrangedSubview(field(destE, "db.internal", 150))
        destRow.addArrangedSubview(label(":"))
        destRow.addArrangedSubview(field(dportE, "3306", 70))
        col.addArrangedSubview(destRow)
        dportE.stringValue = "3306"
        // 经由主机
        let viaRow = NSStackView()
        viaRow.orientation = .horizontal
        viaRow.spacing = 8
        viaRow.addArrangedSubview(label(L("经由 SSH 主机")))
        viaDD.setAccessibilityIdentifier("fwdf-via")
        viaRow.addArrangedSubview(viaDD)
        col.addArrangedSubview(viaRow)
        reloadHosts()
        viaDD.target = self
        viaDD.action = #selector(onVia)
        // 提示（黄）/ 冲突错误（红）
        hintL.font = .systemFont(ofSize: 11)
        hintL.hbText = "amber"
        col.addArrangedSubview(hintL)
        errL.font = .systemFont(ofSize: 11)
        errL.hbText = "pillErrFg"
        errL.setAccessibilityIdentifier("fwdf-err")
        col.addArrangedSubview(errL)
        // 命令预览
        let cmdBox = NSStackView()
        cmdBox.orientation = .vertical
        cmdBox.alignment = .leading
        cmdBox.spacing = 4
        let cmdT = label(L("等效命令"))
        cmdT.textColor = .secondaryLabelColor
        cmdBox.addArrangedSubview(cmdT)
        cmdL.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        cmdL.textColor = .secondaryLabelColor
        cmdL.isSelectable = true
        cmdL.setAccessibilityIdentifier("fwdf-cmd")
        cmdBox.addArrangedSubview(cmdL)
        col.addArrangedSubview(cmdBox)
        // autostart
        let aRow = NSStackView()
        aRow.orientation = .horizontal
        aRow.spacing = 8
        aRow.addArrangedSubview(autoSw)
        let al = label(L("随主机连接自动启动"))
        aRow.addArrangedSubview(al)
        let ah = label(L("该 SSH 主机连上时自动拉起此规则"))
        ah.font = .systemFont(ofSize: 10.5)
        ah.textColor = .secondaryLabelColor
        aRow.addArrangedSubview(ah)
        col.addArrangedSubview(aRow)
        // footer
        let foot = NSStackView()
        foot.orientation = .horizontal
        foot.spacing = 10
        foot.distribution = .gravityAreas
        let spacer = NSView()
        foot.addArrangedSubview(spacer)
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let cancel = NSButton(title: L("取消"), target: self,
                              action: #selector(onCancel))
        cancel.bezelStyle = .rounded
        saveBtn = NSButton(title: L("保存"), target: self,
                           action: #selector(onSave))
        saveBtn.bezelStyle = .rounded
        saveStartBtn = NSButton(title: L("保存并启动"), target: self,
                                action: #selector(onSaveStart))
        saveStartBtn.bezelStyle = .rounded
        saveStartBtn.keyEquivalent = "\r"
        saveStartBtn.setAccessibilityIdentifier("fwdf-savestart")
        saveBtn.setAccessibilityIdentifier("fwdf-save")
        foot.addArrangedSubview(cancel)
        foot.addArrangedSubview(saveBtn)
        foot.addArrangedSubview(saveStartBtn)
        col.addArrangedSubview(foot)
        foot.widthAnchor.constraint(equalTo: col.widthAnchor)
            .isActive = true
        onKind()
    }
    required init?(coder: NSCoder) { fatalError() }
    deinit { probeTimer?.invalidate() }

    // ---------- hosts ----------
    private func reloadHosts() {
        viaDD.removeAllItems()
        viaIds = []
        viaDD.addItem(withTitle: L("选择主机…"))
        viaIds.append("")
        for h in Vault.shared.list("host") {
            let nm = hb_obj_get_str(h, "name").str
            let ad = hb_obj_get_str(h, "address").str
            viaDD.addItem(withTitle: nm.isEmpty ? ad : nm)
            viaIds.append(hb_obj_id(h).str)
            hb_obj_free(h)
        }
    }

    // ---------- 编辑填充 ----------
    func fill(_ o: OpaquePointer) {
        obj = o
        let ks = hb_obj_get_str(o, "kind").str
        kind = FwdKit.Kind(rawValue: ks) ?? .local
        seg.selectedSegment = kind == .local ? 0 : kind == .remote ? 1 : 2
        bindE.stringValue = hb_obj_get_str(o, "bind_addr").str
        let bp = hb_obj_get_int(o, "bind_port", 0)
        portE.stringValue = bp > 0 ? "\(bp)" : ""
        destE.stringValue = hb_obj_get_str(o, "dest_addr").str
        let dp = hb_obj_get_int(o, "dest_port", 0)
        dportE.stringValue = dp > 0 ? "\(dp)" : ""
        let vid = hb_obj_get_str(o, "via").str
        if let i = viaIds.firstIndex(of: vid) {
            viaDD.selectItem(at: i)
        }
        autoSw.state = hb_obj_get_bool(o, "autostart", false) ? .on : .off
        onKind()
    }

    // ---------- 取值 ----------
    private func bindAddr() -> String {
        let s = bindE.stringValue.trimmingCharacters(in: .whitespaces)
        return s.isEmpty ? "127.0.0.1" : s
    }
    private func bindPort() -> Int {
        Int(portE.stringValue.trimmingCharacters(in: .whitespaces)) ?? 0
    }
    private func destAddr() -> String {
        destE.stringValue.trimmingCharacters(in: .whitespaces)
    }
    private func destPort() -> Int {
        Int(dportE.stringValue.trimmingCharacters(in: .whitespaces)) ?? 0
    }
    private func viaId() -> String? {
        let i = viaDD.indexOfSelectedItem
        guard i >= 0 && i < viaIds.count, !viaIds[i].isEmpty
        else { return nil }
        return viaIds[i]
    }

    // ---------- 刷新 ----------
    @objc private func onKind() {
        kind = seg.selectedSegment == 0 ? .local :
               seg.selectedSegment == 1 ? .remote : .dynamic
        explain.stringValue = kind.explain
        destRow.isHidden = kind == .dynamic
        scheduleProbe()
        refreshCmd()
        refreshHints()
    }
    @objc private func onVia() { refreshCmd() }
    func controlTextDidChange(_ n: Notification) {
        scheduleProbe()
        refreshCmd()
        refreshHints()
    }
    private func refreshHints() {
        hintL.stringValue = FwdKit.hint(kind: kind,
            bindAddr: bindAddr(), bindPort: bindPort()) ?? ""
    }
    private func refreshCmd() {
        var user: String?, addr: String?, port = 22, jumps: String?
        if let vid = viaId(),
           let h = Vault.shared.get(vid), !hb_obj_tombstone(h) {
            user = hb_obj_get_str(h, "user").str
            if user?.isEmpty == true { user = nil }
            addr = hb_obj_get_str(h, "address").str
            if addr?.isEmpty == true {
                addr = hb_obj_get_str(h, "name").str
            }
            if addr?.isEmpty == true { addr = nil }
            port = Int(hb_obj_get_int(h, "port", 22))
            var names: [String] = []
            var jids: UnsafeMutablePointer<UnsafePointer<CChar>?>?
            let nj = hb_obj_get_strv(h, "jump", &jids)
            for i in 0..<nj {
                if let j = jids?[i],
                   let jh = Vault.shared.get(String(cString: j)),
                   !hb_obj_tombstone(jh) {
                    var nm = hb_obj_get_str(jh, "name").str
                    if nm.isEmpty {
                        nm = hb_obj_get_str(jh, "address").str
                    }
                    names.append(nm.isEmpty ? String(cString: j) : nm)
                    hb_obj_free(jh)
                } else if let j = jids?[i] {
                    names.append(String(cString: j))
                }
            }
            if let jids { free(jids) }
            jumps = names.isEmpty ? nil : names.joined(separator: ",")
            hb_obj_free(h)
        }
        cmdL.stringValue = FwdKit.cmdPreview(kind: kind,
            bindAddr: bindAddr(), bindPort: bindPort(),
            destAddr: destAddr(), destPort: destPort(),
            user: user, hostAddr: addr, hostPort: port, jumps: jumps)
    }

    // ---------- 冲突预检（linux do_probe + rule_conflict）----------
    private func setPortErr(_ msg: String?) {
        errL.stringValue = msg ?? ""
        conflict = msg != nil
        saveBtn.isEnabled = !conflict
        saveStartBtn.isEnabled = !conflict
    }
    private func scheduleProbe() {
        probeTimer?.invalidate()
        probeTimer = Timer.scheduledTimer(withTimeInterval: 0.3,
            repeats: false) { [weak self] _ in self?.doProbe() }
    }
    private func portHeldBySelf() -> Bool {
        guard let obj else { return false }
        let st = ForwardMgr.shared.status(hb_obj_id(obj).str)
        guard st.state == .listening || st.state == .connecting ||
              st.state == .reconnecting else { return false }
        let sa = hb_obj_get_str(obj, "bind_addr").str
        return (sa.isEmpty ? "127.0.0.1" : sa) == bindAddr() &&
               Int(hb_obj_get_int(obj, "bind_port", 0)) == bindPort()
    }
    private func doProbe() {
        let addr = bindAddr()
        let port = bindPort()
        if !FwdKit.addrOk(addr) {
            setPortErr(L("绑定地址必须是数字 IP（IPv4/IPv6）或 localhost"))
            return
        }
        if port < 1 || port > 65535 {
            setPortErr(L("端口必须在 1–65535"))
            return
        }
        if kind == .remote || portHeldBySelf() {
            setPortErr(nil)
            return
        }
        /* 其它已存 local/dynamic 规则同端口（通配地址视为重叠） */
        let selfId = obj.map { hb_obj_id($0).str }
        for o in Vault.shared.list("forward") {
            defer { hb_obj_free(o) }
            if hb_obj_id(o).str == selfId { continue }
            if hb_obj_get_str(o, "kind").str == "remote" { continue }
            if Int(hb_obj_get_int(o, "bind_port", 0)) != port { continue }
            let oa = hb_obj_get_str(o, "bind_addr").str
            let oaN = oa.isEmpty ? "127.0.0.1" : oa
            if FwdKit.isWildcard(addr) || FwdKit.isWildcard(oaN) ||
               oaN == addr {
                setPortErr(L("端口 %d 已被另一条转发规则占用（%@:%d）")
                    .fmt(port, oaN,
                         Int(hb_obj_get_int(o, "bind_port", 0))))
                return
            }
        }
        var err: UnsafeMutablePointer<CChar>?
        if hb_pf_probe_bind(addr, Int32(port), &err) != HB_OK {
            setPortErr(err.map { String(cString: $0) } ?? L("端口不可用"))
            if let err { free(err) }
            return
        }
        if let err { free(err) }
        setPortErr(nil)
    }

    // ---------- 保存 ----------
    private func doSave(_ start: Bool) {
        probeTimer?.invalidate(); probeTimer = nil
        doProbe()                       /* 保存时复查（linux 同） */
        if conflict { return }          /* 行内红字已给出 */
        if let msg = FwdKit.validate(kind: kind, bindAddr: bindAddr(),
            bindPort: bindPort(), destAddr: destAddr(),
            destPort: destPort(), viaSet: viaId() != nil) {
            errL.stringValue = msg      /* 显示但不冻结保存键 */
            return
        }
        guard let o = obj ?? Vault.shared.newObj("forward")
        else { return }
        if obj == nil {
            hb_obj_set_int(o, "order",
                           Int64(Date().timeIntervalSince1970))
            hb_obj_set_bool(o, "enabled", false)
        }
        hb_obj_set_str(o, "kind", kind.rawValue)
        hb_obj_set_str(o, "bind_addr", bindAddr())
        hb_obj_set_int(o, "bind_port", Int64(bindPort()))
        if kind == .dynamic {
            hb_obj_set_str(o, "dest_addr",
                           nil as UnsafePointer<CChar>?)
            hb_obj_set_int(o, "dest_port", 0)
        } else {
            hb_obj_set_str(o, "dest_addr", destAddr())
            hb_obj_set_int(o, "dest_port", Int64(destPort()))
        }
        if let vid = viaId() {
            hb_obj_set_str(o, "via", vid)
        } else {
            hb_obj_set_str(o, "via", nil as UnsafePointer<CChar>?)
        }
        hb_obj_set_bool(o, "autostart", autoSw.state == .on)
        onDone?(start, o)
    }
    @objc private func onSave() { doSave(false) }
    @objc private func onSaveStart() { doSave(true) }
    @objc private func onCancel() { onDone?(false, nil) }
}
