import AppKit
import CHarbor
import HarborKit

/// M6 账户页（FR-11）——对齐 linux/src/hb-account-view.c。
/// 未登录：登录卡（服务器/邮箱/密码/TOTP/错误行 + 注册/恢复）；
/// 已登录：E2EE 横幅 + 账号卡 + 设备卡 + 同步范围卡 + 冲突卡列表。
final class AccountPage: NSView {
    private let svc = SyncService.shared
    private var obs: NSObjectProtocol?

    // 未登录表单
    private var srvE = NSTextField()
    private var emE = NSTextField()
    private var pwE = NSSecureTextField()
    private var totpE = NSTextField()
    private var totpRow: NSStackView?
    private var errL = NSTextField(wrappingLabelWithString: "")
    private var devices: [SyncKit.DeviceInfo] = []
    private var devicesRequested = false

    static let defaultServer = "http://127.0.0.1:8787"

    override init(frame f: NSRect) {
        super.init(frame: f)
        obs = NotificationCenter.default.addObserver(
            forName: .hbSyncChanged, object: nil, queue: .main) {
            [weak self] _ in self?.rebuild()
        }
        rebuild()
    }
    required init?(coder: NSCoder) { nil }
    deinit { obs.map { NotificationCenter.default.removeObserver($0) } }

    // ---------- 小部件 ----------
    private func label(_ s: String, _ size: CGFloat = 12,
                       bold: Bool = false,
                       dim: Bool = false) -> NSTextField {
        let l = NSTextField(wrappingLabelWithString: s)
        l.font = bold ? .boldSystemFont(ofSize: size)
                      : .systemFont(ofSize: size)
        if dim { l.textColor = .secondaryLabelColor }
        return l
    }
    private func h4(_ s: String) -> NSTextField { label(s, 14, bold: true) }
    private func psub(_ s: String) -> NSTextField {
        label(s, 11.5, dim: true)
    }
    private func hint(_ s: String, color: NSColor) -> NSTextField {
        let l = label(s, 11.5); l.textColor = color; return l
    }
    private func btn(_ t: String, primary: Bool = false,
                     danger: Bool = false,
                     act: Selector) -> NSButton {
        let b = NSButton(title: t, target: self, action: act)
        b.bezelStyle = .rounded
        if primary { b.keyEquivalent = "\r" }
        if danger { b.contentTintColor = .systemRed }
        return b
    }
    private func field(_ t: String, _ e: NSTextField) -> NSStackView {
        let l = label(t, 11.5, dim: true)
        l.widthAnchor.constraint(equalToConstant: 110).isActive = true
        e.translatesAutoresizingMaskIntoConstraints = false
        e.widthAnchor.constraint(greaterThanOrEqualToConstant: 220)
            .isActive = true
        let r = NSStackView(views: [l, e])
        r.orientation = .horizontal; r.spacing = 8
        return r
    }
    private func card() -> NSStackView {
        let v = NSStackView()
        v.orientation = .vertical
        v.alignment = .leading
        v.spacing = 10
        v.edgeInsets = .init(top: 14, left: 16, bottom: 14, right: 16)
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.card.color.cgColor
        v.layer?.cornerRadius = 8
        v.layer?.borderWidth = 1
        v.layer?.borderColor = Theme.line.color.cgColor
        return v
    }

    // ---------- 重建 ----------
    private func rebuild() {
        subviews.forEach { $0.removeFromSuperview() }
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        addSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        let col = NSStackView()
        col.orientation = .vertical
        col.alignment = .centerX   // 子卡显式宽度
        col.spacing = 14
        col.edgeInsets = .init(top: 20, left: 20, bottom: 30, right: 20)
        col.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = col
        col.widthAnchor.constraint(equalTo: scroll.widthAnchor).isActive = true
        if svc.state == .signedOut {
            col.addArrangedSubview(buildSignedOut())
        } else {
            buildSignedIn(col)
        }
        if svc.mergePending && mergeSheet == nil { askMerge() }
    }

    // ---------- 未登录 ----------
    private func buildSignedOut() -> NSView {
        let p = card()
        p.widthAnchor.constraint(equalToConstant: 460).isActive = true
        p.addArrangedSubview(h4(L("登录 Harbor 账号")))
        p.addArrangedSubview(psub(L(
            "登录后从云端拉取密文并在本地解密，恢复全部 Vault 与同步。")))
        srvE.stringValue = svc.server ?? Self.defaultServer
        p.addArrangedSubview(field(L("服务器"), srvE))
        p.addArrangedSubview(field(L("邮箱"), emE))
        p.addArrangedSubview(field(L("主密码"), pwE))
        let tr = field(L("TOTP 验证码"), totpE)
        tr.isHidden = true
        totpRow = tr
        p.addArrangedSubview(tr)
        errL.textColor = .systemRed
        p.addArrangedSubview(errL)
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 9
        row.addArrangedSubview(
            btn(L("登录并同步"), primary: true, act: #selector(doLogin)))
        row.addArrangedSubview(
            btn(L("注册新账号"), act: #selector(openRegister)))
        row.addArrangedSubview(
            btn(L("用恢复码恢复"), act: #selector(openRecover)))
        p.addArrangedSubview(row)
        p.addArrangedSubview(hint(L(
            "零知识架构：密码错误则无法解密 Vault，服务端不能帮你重置。"),
            color: .secondaryLabelColor))
        return p
    }

    private func showErr(_ m: String?) {
        errL.stringValue = m ?? ""
        errL.isHidden = (m ?? "").isEmpty
    }

    @objc private func doLogin() {
        guard let nsrv = SyncKit.checkServer(srvE.stringValue) else {
            showErr(L("服务器地址仅支持 https:// 或回环 http://")); return
        }
        let em = emE.stringValue, pw = pwE.stringValue
        let tp = totpE.stringValue
        if em.isEmpty || pw.isEmpty {
            showErr(L("请填写邮箱与密码")); return
        }
        showErr(nil)
        svc.login(server: nsrv, email: em, password: pw,
                  totp: tp.isEmpty ? nil : tp) { [weak self] r in
            guard let self else { return }
            if r.ok { self.showErr(nil); return }   // merge 由 rebuild 处理
            if r.code == "totp_required" {
                self.totpRow?.isHidden = false
                self.showErr(L("需要两步验证码"))
                self.window?.makeFirstResponder(self.totpE)
                return
            }
            if r.code == "rate_limited", r.retryAfter > 0 {
                self.showErr(SyncKit.retryMsg(r.retryAfter)); return
            }
            self.showErr(r.message ?? L("登录失败"))
        }
        pwE.stringValue = ""; totpE.stringValue = ""
    }

    // ---------- 已登录 ----------
    private func buildSignedIn(_ col: NSStackView) {
        // E2EE 横幅
        let ban = card()
        ban.widthAnchor.constraint(equalToConstant: 560).isActive = true
        let brow = NSStackView()
        brow.orientation = .horizontal; brow.spacing = 12
        let ic = NSImageView(image: hbIcon("key", size: 22,
                                           tint: .systemGreen))
        let vb = NSStackView()
        vb.orientation = .vertical; vb.spacing = 2
        vb.alignment = .leading
        vb.addArrangedSubview(
            label(L("端到端加密已开启"), 13, bold: true))
        vb.addArrangedSubview(psub(L(
            "Vault 在本地解密；服务器只存密文，无法读取你的数据。")))
        brow.addArrangedSubview(ic); brow.addArrangedSubview(vb)
        ban.addArrangedSubview(brow)
        col.addArrangedSubview(ban)

        // 账号卡
        let acct = card()
        acct.widthAnchor.constraint(equalToConstant: 560).isActive = true
        acct.addArrangedSubview(h4(svc.email ?? L("账号")))
        var sub = ""
        if let s = svc.server { sub += s + "  " }
        sub += svc.statusText
        if svc.lastSync > 0 {
            let df = DateFormatter(); df.dateFormat = "MM-dd HH:mm:ss"
            sub += " · " + df.string(
                from: Date(timeIntervalSince1970:
                           TimeInterval(svc.lastSync)))
        }
        if let e = svc.lastError { sub += "  " + e }
        acct.addArrangedSubview(psub(sub))
        let arow = NSStackView()
        arow.orientation = .horizontal; arow.spacing = 8
        arow.addArrangedSubview(btn(L("立即同步"),
                                    act: #selector(syncNow)))
        arow.addArrangedSubview(btn(L("修改密码"),
                                    act: #selector(openChangePw)))
        arow.addArrangedSubview(btn(L("轮换恢复码"),
                                    act: #selector(openRotate)))
        arow.addArrangedSubview(btn(
            svc.totpEnabled ? L("关闭两步验证") : L("开启两步验证"),
            act: #selector(openTotp)))
        arow.addArrangedSubview(btn(L("退出账号"), danger: true,
                                    act: #selector(openLogout)))
        acct.addArrangedSubview(arow)
        col.addArrangedSubview(acct)

        // 设备卡
        let dev = card()
        dev.widthAnchor.constraint(equalToConstant: 560).isActive = true
        dev.addArrangedSubview(h4(L("已登录设备")))
        if devices.isEmpty {
            dev.addArrangedSubview(psub(L("加载中…")))
            // devices() 会 opCount++ → emitChanged → rebuild 同步递归，
            // 必须只允许一个在途请求，否则栈溢出。
            if !devicesRequested {
                devicesRequested = true
                refreshDevices()
            }
        }
        let df = DateFormatter(); df.dateFormat = "MM-dd HH:mm"
        for d in devices {
            let row = NSStackView()
            row.orientation = .horizontal; row.spacing = 10
            let info = NSStackView()
            info.orientation = .vertical; info.spacing = 1
            info.alignment = .leading
            info.addArrangedSubview(label(
                d.name.isEmpty ? L("设备") : d.name, 12.5, bold: true))
            let seen = d.lastSeen > 0
                ? df.string(from: Date(
                    timeIntervalSince1970: TimeInterval(d.lastSeen) / 1000))
                : "—"
            info.addArrangedSubview(psub(
                "\(d.platform.isEmpty ? "macos" : d.platform)" +
                " · \(L("上次活动")) \(seen)"))
            row.addArrangedSubview(info)
            if d.current {
                let tag = label(L("当前"), 10.5)
                tag.textColor = .systemBlue
                row.addArrangedSubview(tag)
            } else {
                let b = NSButton(title: L("吊销"), target: self,
                                 action: #selector(revokeBtn(_:)))
                b.contentTintColor = .systemRed
                b.tag = devices.firstIndex(where: { $0.id == d.id }) ?? 0
                row.addArrangedSubview(b)
            }
            dev.addArrangedSubview(row)
        }
        col.addArrangedSubview(dev)

        // 范围卡
        let sp = card()
        sp.widthAnchor.constraint(equalToConstant: 560).isActive = true
        sp.addArrangedSubview(h4(L("同步范围")))
        sp.addArrangedSubview(psub(L(
            "关闭的项目不参与同步。冲突采用版本向量 + LWW 合并。")))
        let scopes: [(String, String)] = [
            ("hosts",    L("主机与分组")),
            ("keys",     L("密钥与凭据（E2EE）")),
            ("snippets", L("Snippets")),
            ("forwards", L("端口转发规则")),
            ("known",    L("Known Hosts")),
            ("settings", L("外观与设置")),
        ]
        for (key, ttl) in scopes {
            let row = NSStackView()
            row.orientation = .horizontal; row.spacing = 8
            let l = label(ttl); l.setContentHuggingPriority(
                .defaultLow, for: .horizontal)
            let sw = NSSwitch()
            sw.state = svc.scopeGet(key) ? .on : .off
            sw.target = self
            sw.action = #selector(scopeSw(_:))
            sw.identifier = NSUserInterfaceItemIdentifier(key)
            row.addArrangedSubview(l); row.addArrangedSubview(sw)
            sp.addArrangedSubview(row)
        }
        let hrow = NSStackView()
        hrow.orientation = .horizontal; hrow.spacing = 8
        let hl = label(L("Shell History（暂不支持）"))
        hl.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let hsw = NSSwitch(); hsw.isEnabled = false
        hrow.addArrangedSubview(hl); hrow.addArrangedSubview(hsw)
        sp.addArrangedSubview(hrow)
        col.addArrangedSubview(sp)

        // 冲突卡
        if let s = svc.sync {
            var cl: UnsafeMutablePointer<hb_sync_conflict>?
            var n = 0
            if hb_sync_conflicts(s, &cl, &n) == HB_OK, let cl, n > 0 {
                let arr = UnsafeBufferPointer(start: cl, count: n)
                for c in arr { col.addArrangedSubview(conflictCard(c)) }
                hb_sync_conflicts_free(cl, n)
            } else if let cl { hb_sync_conflicts_free(cl, n) }
        }
    }

    private func conflictCard(_ c: hb_sync_conflict) -> NSView {
        let card = card()
        card.widthAnchor.constraint(equalToConstant: 560).isActive = true
        let id = c.id.map { String(cString: $0) } ?? ""
        let tp = c.type.map { String(cString: $0) } ?? ""
        card.addArrangedSubview(h4("\(tp) · \(id.prefix(8))"))
        // 当前版本 data JSON
        var curJson: String?
        if let o = Vault.shared.get(id) {
            defer { hb_obj_free(o) }
            curJson = hb_obj_to_json(o).map { String(cString: $0) }
        }
        let loserJson = c.loser_summary.map { String(cString: $0) }
        let fields = (c.fields_json.flatMap { String(cString: $0) })
            .flatMap { try? JSONDecoder().decode(
                [String].self, from: Data($0.utf8)) } ?? []
        for f in fields {
            let secret = SyncKit.isSecretField(f)
            let cv = secret ? L("••• (已更改)")
                            : (SyncKit.memberStr(curJson, f) ?? "—")
            let ov = secret ? L("••• (已更改)")
                            : (SyncKit.memberStr(loserJson, f) ?? "—")
            card.addArrangedSubview(psub(f))
            let hdr = NSStackView()
            hdr.orientation = .horizontal; hdr.spacing = 8
            let a = psub(L("当前")); let b = psub(L("另一版本"))
            a.setContentHuggingPriority(.defaultLow, for: .horizontal)
            b.setContentHuggingPriority(.defaultLow, for: .horizontal)
            hdr.addArrangedSubview(a); hdr.addArrangedSubview(b)
            card.addArrangedSubview(hdr)
            let row2 = NSStackView()
            row2.orientation = .horizontal; row2.spacing = 8
            let av = label(cv); let bv = label(ov)
            av.setContentHuggingPriority(.defaultLow, for: .horizontal)
            bv.setContentHuggingPriority(.defaultLow, for: .horizontal)
            row2.addArrangedSubview(av); row2.addArrangedSubview(bv)
            card.addArrangedSubview(row2)
        }
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let kb = NSButton(title: L("保留当前"), target: self,
                          action: #selector(conflictKeep(_:)))
        let tb = NSButton(title: L("使用另一版本"), target: self,
                          action: #selector(conflictTake(_:)))
        kb.identifier = .init(id); tb.identifier = .init(id)
        row.addArrangedSubview(kb); row.addArrangedSubview(tb)
        card.addArrangedSubview(row)
        return card
    }

    @objc private func conflictKeep(_ b: NSButton) {
        guard let s = svc.sync, let id = b.identifier?.rawValue else { return }
        id.withCString { _ = hb_sync_conflict_keep_local(s, $0) }
        rebuild()
    }
    @objc private func conflictTake(_ b: NSButton) {
        guard let s = svc.sync, let id = b.identifier?.rawValue else { return }
        id.withCString { _ = hb_sync_conflict_take_other(s, $0) }
        svc.syncNow()
        rebuild()
    }
    @objc private func scopeSw(_ sw: NSSwitch) {
        guard let key = sw.identifier?.rawValue else { return }
        svc.setScope(key, sw.state == .on)
    }
    @objc private func syncNow() { svc.syncNow() }
    @objc private func revokeBtn(_ b: NSButton) {
        let d = devices[b.tag]
        openRevoke(d.id, d.name.isEmpty ? d.id : d.name)
    }

    private func refreshDevices() {
        svc.devices { [weak self] r in
            guard let self else { return }
            self.devicesRequested = false
            if r.ok, let d = r.devices {
                self.devices = d
                self.rebuild()
            }
        }
    }

    // ================= 对话框 =================

    private func sheet(_ title: String, _ v: NSView,
                       _ w: CGFloat, _ h: CGFloat) -> NSWindow {
        let s = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: w, height: h),
            styleMask: [.titled], backing: .buffered, defer: false)
        s.title = title
        s.isReleasedWhenClosed = false
        s.contentView = v
        return s
    }

    private func dlgColumn(_ w: CGFloat) -> NSStackView {
        let col = NSStackView()
        col.orientation = .vertical
        col.alignment = .leading
        col.spacing = 10
        col.edgeInsets = .init(top: 16, left: 18, bottom: 16, right: 18)
        col.frame = NSRect(x: 0, y: 0, width: w, height: 10)
        return col
    }

    // ---- merge 询问 ----
    private func askMerge() {
        let col = dlgColumn(440)
        col.addArrangedSubview(psub(L(
            "本机 Vault 中已有数据。合并后将上传加密副本并与云端保持一致；" +
            "丢弃则以云端内容覆盖本机同步范围内的数据。")))
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let mb = btn(L("合并本机数据到账号"), primary: true,
                     act: #selector(mergeYes))
        let db = btn(L("丢弃本机数据，使用云端"), danger: true,
                     act: #selector(mergeNo))
        row.addArrangedSubview(mb); row.addArrangedSubview(db)
        col.addArrangedSubview(row)
        let s = sheet(L("首次登录"), col, 440, 150)
        mergeSheet = s
        window?.beginSheet(s) { _ in }
    }
    private weak var mergeSheet: NSWindow?
    @objc private func mergeYes() {
        svc.confirmMerge(true)
        if let s = mergeSheet { window?.endSheet(s); mergeSheet = nil }
    }
    @objc private func mergeNo() {
        svc.confirmMerge(false)
        if let s = mergeSheet { window?.endSheet(s); mergeSheet = nil }
    }

    // ---- 注册 ----
    @objc private func openRegister() {
        guard let nsrv = SyncKit.checkServer(srvE.stringValue) else {
            showErr(L("服务器地址仅支持 https:// 或回环 http://")); return
        }
        let col = dlgColumn(430)
        col.addArrangedSubview(hint(L(
            "零知识：Harbor 服务器只存加密数据，无法帮你找回或重置密码。" +
            "忘记密码且没有 24 词恢复码或其他已登录设备时，数据不可恢复。"),
            color: .systemOrange))
        let em = NSTextField(), pw = NSSecureTextField(),
            pw2 = NSSecureTextField()
        col.addArrangedSubview(field(L("邮箱"), em))
        col.addArrangedSubview(field(L("主密码"), pw))
        col.addArrangedSubview(field(L("确认密码"), pw2))
        let err = NSTextField(wrappingLabelWithString: "")
        err.textColor = .systemRed
        col.addArrangedSubview(err)
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let s = sheet(L("注册新账号"), col, 430, 300)
        let cb = btn(L("取消"), act: #selector(closeSheet(_:)))
        cb.identifier = .init("s")
        let ob = btn(L("注册"), primary: true,
                     act: #selector(regGo(_:)))
        cb.target = self; ob.target = self
        objc_setAssociatedObject(ob, "ctx",
            RegCtx(sheet: s, em: em, pw: pw, pw2: pw2, err: err,
                   server: nsrv), .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(cb, "sheet", s, .OBJC_ASSOCIATION_RETAIN)
        row.addArrangedSubview(cb); row.addArrangedSubview(ob)
        col.addArrangedSubview(row)
        window?.beginSheet(s) { _ in }
    }

    private final class RegCtx: NSObject {
        weak var sheet: NSWindow?
        let em: NSTextField, pw: NSSecureTextField, pw2: NSSecureTextField
        let err: NSTextField, server: String
        init(sheet: NSWindow, em: NSTextField, pw: NSSecureTextField,
             pw2: NSSecureTextField, err: NSTextField, server: String) {
            self.sheet = sheet; self.em = em; self.pw = pw
            self.pw2 = pw2; self.err = err; self.server = server
        }
    }

    @objc private func regGo(_ b: NSButton) {
        guard let c = objc_getAssociatedObject(b, "ctx") as? RegCtx
        else { return }
        let em = c.em.stringValue, pw = c.pw.stringValue
        if em.isEmpty { c.err.stringValue = L("请填写邮箱"); return }
        if pw.count < 8 { c.err.stringValue = L("密码至少 8 位"); return }
        if pw != c.pw2.stringValue {
            c.err.stringValue = L("两次密码不一致"); return
        }
        c.err.stringValue = ""
        b.isEnabled = false
        svc.register(server: c.server, email: em, password: pw) {
            [weak self, weak c] r in
            guard let self, let c else { return }
            if !r.ok {
                c.err.stringValue = r.code == "rate_limited"
                    && r.retryAfter > 0
                    ? SyncKit.retryMsg(r.retryAfter)
                    : (r.message ?? L("注册失败"))
                b.isEnabled = true
                return
            }
            if let s = c.sheet { self.window?.endSheet(s) }
            if let w = r.words { self.showWords(w) }
        }
        c.pw.stringValue = ""; c.pw2.stringValue = ""
    }

    @objc private func closeSheet(_ b: NSButton) {
        if let s = objc_getAssociatedObject(b, "sheet") as? NSWindow {
            window?.endSheet(s)
        }
    }

    // ---- 恢复码展示 + 3 词确认 ----
    private func showWords(_ words: String) {
        let arr = SyncKit.splitWords(words)
        let wv = WordsView(words: arr)
        wv.frame = NSRect(x: 0, y: 0, width: 480, height: 420)
        let s = sheet(L("恢复码（请离线抄写）"), wv, 480, 460)
        wv.onDone = {
            if let p = s.sheetParent { p.endSheet(s) }
            wv.wipe()
        }
        window?.beginSheet(s) { _ in }
    }

    // ---- 恢复 ----
    @objc private func openRecover() {
        guard let nsrv = SyncKit.checkServer(srvE.stringValue) else {
            showErr(L("服务器地址仅支持 https:// 或回环 http://")); return
        }
        let col = dlgColumn(460)
        col.addArrangedSubview(hint(L(
            "没有恢复码、也没有其他已登录设备时，数据无法恢复（零知识）。"),
            color: .systemRed))
        let em = NSTextField()
        let wd = NSTextField()
        wd.placeholderString = L("word1 word2 … word24")
        let pw = NSSecureTextField(), pw2 = NSSecureTextField()
        col.addArrangedSubview(field(L("邮箱"), em))
        col.addArrangedSubview(field(L("24 词恢复码（空格分隔）"), wd))
        col.addArrangedSubview(field(L("新密码"), pw))
        col.addArrangedSubview(field(L("确认新密码"), pw2))
        let err = NSTextField(wrappingLabelWithString: "")
        err.textColor = .systemRed
        col.addArrangedSubview(err)
        let s = sheet(L("用恢复码恢复"), col, 460, 330)
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let cb = btn(L("取消"), act: #selector(closeSheet(_:)))
        let ob = btn(L("恢复"), primary: true, act: #selector(recGo(_:)))
        objc_setAssociatedObject(cb, "sheet", s, .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "ctx",
            RegCtx(sheet: s, em: em, pw: pw, pw2: pw2, err: err,
                   server: nsrv), .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "wd", wd, .OBJC_ASSOCIATION_RETAIN)
        row.addArrangedSubview(cb); row.addArrangedSubview(ob)
        col.addArrangedSubview(row)
        window?.beginSheet(s) { _ in }
    }

    @objc private func recGo(_ b: NSButton) {
        guard let c = objc_getAssociatedObject(b, "ctx") as? RegCtx,
              let wd = objc_getAssociatedObject(b, "wd") as? NSTextField
        else { return }
        let words = wd.stringValue
        if SyncKit.splitWords(words).count != 24 {
            c.err.stringValue = L("请输入全部 24 个词"); return
        }
        let pw = c.pw.stringValue
        if pw.count < 8 { c.err.stringValue = L("新密码至少 8 位"); return }
        if pw != c.pw2.stringValue {
            c.err.stringValue = L("两次密码不一致"); return
        }
        b.isEnabled = false
        svc.recover(server: c.server, email: c.em.stringValue,
                    words: words, newPassword: pw) { [weak self, weak c] r in
            guard let self, let c else { return }
            if !r.ok {
                c.err.stringValue = r.message ?? L("恢复失败")
                b.isEnabled = true; return
            }
            if let s = c.sheet { self.window?.endSheet(s) }
            // need_merge 由 rebuild/askMerge 处理
        }
        c.pw.stringValue = ""; c.pw2.stringValue = ""
    }

    // ---- 修改密码 ----
    @objc private func openChangePw() {
        let col = dlgColumn(400)
        let o = NSSecureTextField(), n = NSSecureTextField(),
            n2 = NSSecureTextField()
        col.addArrangedSubview(field(L("当前密码"), o))
        col.addArrangedSubview(field(L("新密码"), n))
        col.addArrangedSubview(field(L("确认新密码"), n2))
        let err = NSTextField(wrappingLabelWithString: "")
        err.textColor = .systemRed
        col.addArrangedSubview(err)
        let s = sheet(L("修改密码"), col, 400, 240)
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let cb = btn(L("取消"), act: #selector(closeSheet(_:)))
        let ob = btn(L("修改"), primary: true, act: #selector(pwGo(_:)))
        objc_setAssociatedObject(cb, "sheet", s, .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "ctx",
            RegCtx(sheet: s, em: NSTextField(), pw: n, pw2: n2, err: err,
                   server: ""), .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "old", o, .OBJC_ASSOCIATION_RETAIN)
        row.addArrangedSubview(cb); row.addArrangedSubview(ob)
        col.addArrangedSubview(row)
        window?.beginSheet(s) { _ in }
    }

    @objc private func pwGo(_ b: NSButton) {
        guard let c = objc_getAssociatedObject(b, "ctx") as? RegCtx,
              let o = objc_getAssociatedObject(b, "old") as? NSSecureTextField
        else { return }
        let np = c.pw.stringValue
        if np.count < 8 { c.err.stringValue = L("新密码至少 8 位"); return }
        if np != c.pw2.stringValue {
            c.err.stringValue = L("两次密码不一致"); return
        }
        b.isEnabled = false
        svc.changePassword(old: o.stringValue, new: np) {
            [weak self, weak c] r in
            guard let self, let c else { return }
            if !r.ok {
                c.err.stringValue = r.message ?? L("修改失败")
                b.isEnabled = true; return
            }
            self.toast(L("密码已修改"))
            if let s = c.sheet { self.window?.endSheet(s) }
        }
        o.stringValue = ""; c.pw.stringValue = ""; c.pw2.stringValue = ""
    }

    // ---- 恢复码轮换 ----
    @objc private func openRotate() {
        guard let w = window else { return }
        let a = NSAlert()
        a.messageText = L("输入主密码以轮换恢复码")
        let pp = NSSecureTextField(
            frame: NSRect(x: 0, y: 0, width: 240, height: 22))
        a.accessoryView = pp
        a.addButton(withTitle: L("轮换"))
        a.addButton(withTitle: L("取消"))
        a.beginSheetModal(for: w) { [weak self] r in
            guard let self, r == .alertFirstButtonReturn else { return }
            let pw = pp.stringValue
            pp.stringValue = ""
            self.svc.rotateRecovery(password: pw) { [weak self] res in
                guard let self else { return }
                if !res.ok {
                    self.toast(res.message ?? L("恢复码轮换失败"))
                    return
                }
                if let w = res.words { self.showWords(w) }
            }
        }
    }

    // ---- TOTP ----
    @objc private func openTotp() {
        if svc.totpEnabled {
            // disable：输入验证码
            guard let w = window else { return }
            let a = NSAlert()
            a.messageText = L("关闭两步验证")
            a.informativeText = L("验证码")
            let e = NSTextField(
                frame: NSRect(x: 0, y: 0, width: 200, height: 22))
            a.accessoryView = e
            a.addButton(withTitle: L("关闭"))
            a.addButton(withTitle: L("取消"))
            a.beginSheetModal(for: w) { [weak self] r in
                guard let self, r == .alertFirstButtonReturn else { return }
                self.svc.totpDisable(e.stringValue) { [weak self] res in
                    self?.toast(res.ok ? L("两步验证已关闭")
                                       : (res.message ??
                                          L("两步验证关闭失败")))
                }
            }
            return
        }
        svc.totpSetup { [weak self] r in
            guard let self else { return }
            if !r.ok {
                self.toast(r.message ?? L("获取两步验证密钥失败")); return
            }
            self.showTotpSetup(secret: r.totpSecret ?? "",
                             uri: r.totpUri ?? "")
        }
    }

    private func showTotpSetup(secret: String, uri: String) {
        let col = dlgColumn(460)
        col.addArrangedSubview(psub(L(
            "在认证器中添加以下密钥（TOTP · SHA1 · 6 位 · 30 秒）。")))
        let srow = NSStackView()
        srow.orientation = .horizontal; srow.spacing = 8
        let sl = label(secret); sl.font = .monospacedSystemFont(
            ofSize: 12, weight: .regular)
        sl.isSelectable = true
        sl.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let cp = btn(L("复制"), act: #selector(copyText(_:)))
        objc_setAssociatedObject(cp, "txt", secret as NSString,
                                 .OBJC_ASSOCIATION_RETAIN)
        srow.addArrangedSubview(sl); srow.addArrangedSubview(cp)
        col.addArrangedSubview(srow)
        let ul = label(uri); ul.font = .monospacedSystemFont(
            ofSize: 10.5, weight: .regular)
        ul.textColor = .secondaryLabelColor
        ul.isSelectable = true
        col.addArrangedSubview(ul)
        let code = NSTextField()
        col.addArrangedSubview(field(L("输入当前验证码以启用"), code))
        let s = sheet(L("开启两步验证"), col, 460, 260)
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let ob = btn(L("启用"), primary: true,
                     act: #selector(totpEnableGo(_:)))
        objc_setAssociatedObject(ob, "code", code,
                                 .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "sheet", s,
                                 .OBJC_ASSOCIATION_RETAIN)
        row.addArrangedSubview(ob)
        col.addArrangedSubview(row)
        window?.beginSheet(s) { _ in }
    }

    @objc private func totpEnableGo(_ b: NSButton) {
        let code = (objc_getAssociatedObject(b, "code") as? NSTextField)?
            .stringValue ?? ""
        svc.totpEnable(code) { [weak self] r in
            self?.toast(r.ok ? L("两步验证已开启")
                             : (r.message ?? L("两步验证启用失败")))
        }
        if let s = objc_getAssociatedObject(b, "sheet") as? NSWindow {
            window?.endSheet(s)
        }
    }

    @objc private func copyText(_ b: NSButton) {
        if let t = objc_getAssociatedObject(b, "txt") as? String {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(t, forType: .string)
        }
    }

    // ---- 退出 ----
    @objc private func openLogout() {
        let col = dlgColumn(430)
        col.addArrangedSubview(psub(L(
            "退出后停止同步，云端密文保留。本地 Vault 可选择保留或擦除。")))
        let s = sheet(L("退出账号"), col, 430, 160)
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let kb = btn(L("保留本地退出"), act: #selector(logoutKeep(_:)))
        let wb = btn(L("擦除并退出"), danger: true,
                     act: #selector(logoutWipe(_:)))
        for b in [kb, wb] {
            objc_setAssociatedObject(b, "sheet", s,
                                     .OBJC_ASSOCIATION_RETAIN)
        }
        row.addArrangedSubview(kb); row.addArrangedSubview(wb)
        col.addArrangedSubview(row)
        window?.beginSheet(s) { _ in }
    }
    @objc private func logoutKeep(_ b: NSButton) {
        svc.logout(false); endBtnSheet(b)
    }
    @objc private func logoutWipe(_ b: NSButton) {
        svc.logout(true); endBtnSheet(b)
    }
    private func endBtnSheet(_ b: NSButton) {
        if let s = objc_getAssociatedObject(b, "sheet") as? NSWindow {
            window?.endSheet(s)
        }
    }

    // ---- 吊销 ----
    private func openRevoke(_ devId: String, _ name: String) {
        let col = dlgColumn(430)
        col.addArrangedSubview(psub(String(format: L(
            "吊销设备「%@」？该设备将立即断开同步。"), name)))
        let wipeCb = NSButton(
            checkboxWithTitle: L("同时远程擦除该设备上的数据"),
            target: nil, action: nil)
        col.addArrangedSubview(wipeCb)
        let s = sheet(L("吊销设备"), col, 430, 200)
        let row = NSStackView()
        row.orientation = .horizontal; row.spacing = 8
        let cb = btn(L("取消"), act: #selector(closeSheet(_:)))
        let ob = btn(L("吊销"), danger: true, act: #selector(revokeGo(_:)))
        objc_setAssociatedObject(cb, "sheet", s, .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "sheet", s, .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "id", devId as NSString,
                                 .OBJC_ASSOCIATION_RETAIN)
        objc_setAssociatedObject(ob, "wipe", wipeCb,
                                 .OBJC_ASSOCIATION_RETAIN)
        row.addArrangedSubview(cb); row.addArrangedSubview(ob)
        col.addArrangedSubview(row)
        window?.beginSheet(s) { _ in }
    }

    @objc private func revokeGo(_ b: NSButton) {
        guard let id = objc_getAssociatedObject(b, "id") as? String
        else { return }
        let wipe = (objc_getAssociatedObject(b, "wipe") as? NSButton)?
            .state == .on
        svc.revoke(id, wipe: wipe, cb: nil)
        endBtnSheet(b)
        refreshDevices()
    }

    private func toast(_ t: String) {
        let tv = SftpToast(text: t)
        tv.translatesAutoresizingMaskIntoConstraints = false
        addSubview(tv)
        NSLayoutConstraint.activate([
            tv.centerXAnchor.constraint(equalTo: centerXAnchor),
            tv.bottomAnchor.constraint(equalTo: bottomAnchor,
                                       constant: -14),
        ])
    }

    // ---------- dev ----------
    var devState: String {
        """
        state=\(svc.state.rawValue) conflicts=\(svc.conflictCount) \
        last=\(svc.lastSync) email=\(svc.email ?? "-") \
        busy=\(svc.isBusy) merge=\(svc.mergePending) \
        totp=\(svc.totpEnabled)
        """
    }

    /// dev：打开恢复码展示对话框（截图用，不经过服务）
    func devWords(_ words: String) { showWords(words) }
    /// dev：弹 merge 询问
    func devMerge() { svc.confirmMerge(true) }
}

/// 恢复码展示 + 3 位置确认页（对齐 Linux words dialog → confirm page）。
private final class WordsView: NSView {
    private let words: [String]
    var onDone: (() -> Void)?
    weak var parentSheet: NSWindow?
    private var entries: [NSTextField] = []
    private var pos: [Int] = []
    private var errL: NSTextField?

    init(words: [String]) {
        self.words = words
        super.init(frame: .zero)
        buildShow()
    }
    required init?(coder: NSCoder) { nil }

    private func col() -> NSStackView {
        subviews.forEach { $0.removeFromSuperview() }
        let c = NSStackView()
        c.orientation = .vertical; c.alignment = .leading; c.spacing = 10
        c.edgeInsets = .init(top: 16, left: 18, bottom: 16, right: 18)
        c.frame = bounds
        c.autoresizingMask = [.width, .height]
        addSubview(c)
        return c
    }
    private func lab(_ s: String, _ sz: CGFloat = 11.5,
                     mono: Bool = false, dim: Bool = true) -> NSTextField {
        let l = NSTextField(wrappingLabelWithString: s)
        l.font = mono ? .monospacedSystemFont(ofSize: sz, weight: .regular)
                      : .systemFont(ofSize: sz)
        if dim { l.textColor = .secondaryLabelColor }
        return l
    }

    private func buildShow() {
        let c = col()
        let warn = lab(L(
            "忘记密码时，24 词恢复码是找回 Vault 的唯一途径——" +
            "服务端零知识，无法帮你重置。请离线抄写并妥善保存。"))
        warn.textColor = .systemOrange
        c.addArrangedSubview(warn)
        // 24 词 4 列网格
        let grid = NSGridView()
        grid.rowSpacing = 4; grid.columnSpacing = 12
        let cols = 4
        var rows: [[NSView]] = []
        for r in 0..<((words.count + cols - 1) / cols) {
            var row: [NSView] = []
            for cc in 0..<cols {
                let i = r * cols + cc
                let l = i < words.count
                    ? lab(String(format: "%2d. %@", i + 1, words[i]),
                          11.5, mono: true, dim: false)
                    : lab("")
                l.alignment = .left
                row.append(l)
            }
            rows.append(row)
        }
        for row in rows { grid.addRow(with: row) }
        c.addArrangedSubview(grid)
        let hr = NSStackView()
        hr.orientation = .horizontal; hr.spacing = 8
        let cp = NSButton(title: L("复制"), target: self,
                          action: #selector(copyAll))
        let nb = NSButton(title: L("我已保存"), target: self,
                          action: #selector(next))
        nb.keyEquivalent = "\r"
        hr.addArrangedSubview(cp); hr.addArrangedSubview(nb)
        c.addArrangedSubview(hr)
    }

    @objc private func copyAll() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(words.joined(separator: " "),
                                       forType: .string)
    }

    @objc private func next() {
        // 随机 3 个不同位置
        var p: [Int] = []
        while p.count < 3 {
            let x = Int.random(in: 0..<24)
            if !p.contains(x) { p.append(x) }
        }
        pos = p
        let c = col()
        c.addArrangedSubview(lab(L(
            "请按编号填写对应的词，确认你已正确抄写。")))
        entries = []
        for i in 0..<3 {
            let e = NSTextField()
            let row = NSStackView()
            row.orientation = .horizontal; row.spacing = 8
            row.addArrangedSubview(
                lab(String(format: L("第 %d 个词"), pos[i] + 1)))
            row.addArrangedSubview(e)
            entries.append(e)
            c.addArrangedSubview(row)
        }
        let er = lab(""); er.textColor = .systemRed
        errL = er
        c.addArrangedSubview(er)
        let ok = NSButton(title: L("确认"), target: self,
                          action: #selector(check))
        ok.keyEquivalent = "\r"
        c.addArrangedSubview(ok)
    }

    @objc private func check() {
        for i in 0..<3 {
            let t = entries[i].stringValue
                .trimmingCharacters(in: .whitespaces)
            if t.isEmpty || t.lowercased() != words[pos[i]].lowercased() {
                errL?.stringValue = L("词不匹配，请核对恢复码")
                return
            }
        }
        done()
    }

    private func done() { onDone?() }
    func wipe() {
        // words 为 Swift 数组（无法 memset）；确认后至少清空引用
        entries = []
    }
}
