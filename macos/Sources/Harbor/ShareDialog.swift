import AppKit
import CHarbor
import HarborKit

/// FR-12 owner 共享对话框（对齐 linux/src/hb-share-dialog.c /
/// prototype openShareModal）：开启云端共享开关、参与权限、链接 + 配对码
/// （可复制/重新生成）、参与者列表（移除）、停止共享。
final class ShareDialog: NSObject {
    /// 自持有直到 sheet 关闭。
    private static var live: [ShareDialog] = []

    private weak var window: MainWindow?
    private weak var sess: Session?
    private let title: String
    private let host: String
    private var shareId: String?
    private var starting = false
    private var updating = false

    private var sw: NSButton!
    private var modePop: NSPopUpButton!
    private var bodyBox: NSStackView!
    private var linkLbl: NSTextField!
    private var codeLbl: NSTextField!
    private var plist: NSStackView!
    private var emptyLbl: NSTextField!
    private var signedOut: NSView!
    private var obs: [NSObjectProtocol] = []

    /// 引擎无参与者枚举 API → 跟随事件/快照维护（对齐 dialog 的
    /// parts 表 + service participants() 种子）。
    private var parts: [ShareService.Part] = []

    private init(sess: Session, title: String, host: String,
                 window: MainWindow?) {
        self.sess = sess; self.title = title; self.host = host
        self.window = window
    }

    static func present(sess: Session, title: String, host: String,
                        in window: MainWindow?) {
        let d = ShareDialog(sess: sess, title: title, host: host,
                            window: window)
        live.append(d)
        d.show()
    }

    // ---------------- 构造 ----------------

    private func rowLabel(_ t: String, size: CGFloat = 12,
                          bold: Bool = false) -> NSTextField {
        let l = NSTextField(labelWithString: t)
        l.font = .systemFont(ofSize: size,
                             weight: bold ? .semibold : .regular)
        l.hbText = "text"
        return l
    }

    private func show() {
        let a = NSAlert()
        a.messageText = L("共享会话")
        a.addButton(withTitle: L("完成"))
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false

        // 未登录提示
        signedOut = NSView()
        let so1 = rowLabel(L("共享需要登录 Harbor 账号"), bold: true)
        let goBtn = NSButton(title: L("前往账号登录"), target: self,
                             action: #selector(onGotoAccount))
        goBtn.bezelStyle = .rounded
        goBtn.setAccessibilityIdentifier("share-goto-account")
        [so1, goBtn].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            signedOut.addSubview($0)
        }
        NSLayoutConstraint.activate([
            so1.topAnchor.constraint(equalTo: signedOut.topAnchor,
                                     constant: 4),
            so1.centerXAnchor.constraint(
                equalTo: signedOut.centerXAnchor),
            goBtn.topAnchor.constraint(equalTo: so1.bottomAnchor,
                                       constant: 8),
            goBtn.centerXAnchor.constraint(
                equalTo: signedOut.centerXAnchor),
            goBtn.bottomAnchor.constraint(
                equalTo: signedOut.bottomAnchor, constant: -4),
        ])

        // 开启云端共享 + 参与权限
        sw = NSButton(checkboxWithTitle: L("开启云端共享"),
                      target: self, action: #selector(onSwitch))
        sw.setButtonType(.switch)
        sw.setAccessibilityIdentifier("share-enable")
        modePop = NSPopUpButton()
        modePop.addItems(withTitles: [L("仅观看"), L("可协作输入")])
        modePop.target = self; modePop.action = #selector(onMode)
        modePop.setAccessibilityIdentifier("share-mode")
        let mrow = NSView()
        let mlbl = rowLabel(L("参与权限"))
        [mlbl, modePop].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            mrow.addSubview($0)
        }
        mrow.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            mlbl.leadingAnchor.constraint(equalTo: mrow.leadingAnchor),
            mlbl.centerYAnchor.constraint(equalTo: mrow.centerYAnchor),
            modePop.trailingAnchor.constraint(
                equalTo: mrow.trailingAnchor),
            modePop.centerYAnchor.constraint(equalTo: mrow.centerYAnchor),
            mrow.heightAnchor.constraint(equalToConstant: 26),
        ])
        let swRow = NSStackView(views: [sw])
        swRow.orientation = .horizontal

        // 共享信息区
        bodyBox = NSStackView()
        bodyBox.orientation = .vertical
        bodyBox.alignment = .leading
        bodyBox.spacing = 6

        let lrow = NSView()
        let llbl = rowLabel(L("共享链接"))
        linkLbl = NSTextField(labelWithString: "")
        linkLbl.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        linkLbl.hbText = "termDim"
        linkLbl.lineBreakMode = .byTruncatingMiddle
        linkLbl.setAccessibilityIdentifier("share-link")
        let cp = NSButton(image: hbIcon("copy", size: 13,
                                        tint: Theme.dyn("termDim")),
                          target: self, action: #selector(onCopy))
        cp.isBordered = false
        cp.toolTip = L("复制")
        cp.setAccessibilityIdentifier("share-copy")
        [llbl, linkLbl, cp].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            lrow.addSubview($0)
        }
        NSLayoutConstraint.activate([
            llbl.leadingAnchor.constraint(equalTo: lrow.leadingAnchor),
            llbl.centerYAnchor.constraint(equalTo: lrow.centerYAnchor),
            linkLbl.leadingAnchor.constraint(
                equalTo: llbl.trailingAnchor, constant: 8),
            linkLbl.centerYAnchor.constraint(equalTo: lrow.centerYAnchor),
            cp.trailingAnchor.constraint(equalTo: lrow.trailingAnchor),
            cp.centerYAnchor.constraint(equalTo: lrow.centerYAnchor),
            cp.widthAnchor.constraint(equalToConstant: 22),
            lrow.heightAnchor.constraint(equalToConstant: 24),
            lrow.widthAnchor.constraint(equalToConstant: 400),
        ])
        bodyBox.addArrangedSubview(lrow)

        let crow = NSView()
        let clbl = rowLabel(L("配对码"))
        codeLbl = NSTextField(labelWithString: "")
        codeLbl.font = .monospacedSystemFont(ofSize: 14, weight: .semibold)
        codeLbl.hbText = "blue"
        codeLbl.setAccessibilityIdentifier("share-code")
        let ttl = rowLabel(L("30 分钟内有效"), size: 11)
        ttl.hbText = "termDim"
        let rg = NSButton(title: L("重新生成"), target: self,
                          action: #selector(onRegen))
        rg.bezelStyle = .texturedRounded
        rg.font = .systemFont(ofSize: 11.5)
        rg.setAccessibilityIdentifier("share-regen")
        [clbl, codeLbl, ttl, rg].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            crow.addSubview($0)
        }
        NSLayoutConstraint.activate([
            clbl.leadingAnchor.constraint(equalTo: crow.leadingAnchor),
            clbl.centerYAnchor.constraint(equalTo: crow.centerYAnchor),
            codeLbl.leadingAnchor.constraint(
                equalTo: clbl.trailingAnchor, constant: 8),
            codeLbl.centerYAnchor.constraint(equalTo: crow.centerYAnchor),
            ttl.leadingAnchor.constraint(
                equalTo: codeLbl.trailingAnchor, constant: 8),
            ttl.centerYAnchor.constraint(equalTo: crow.centerYAnchor),
            rg.trailingAnchor.constraint(equalTo: crow.trailingAnchor),
            rg.centerYAnchor.constraint(equalTo: crow.centerYAnchor),
            crow.heightAnchor.constraint(equalToConstant: 26),
            crow.widthAnchor.constraint(equalToConstant: 400),
        ])
        bodyBox.addArrangedSubview(crow)

        bodyBox.addArrangedSubview(rowLabel(L("参与者"), bold: true))
        emptyLbl = rowLabel(L("还没有人加入"), size: 11)
        emptyLbl.hbText = "termDim"
        bodyBox.addArrangedSubview(emptyLbl)
        plist = NSStackView()
        plist.orientation = .vertical
        plist.alignment = .leading
        plist.spacing = 4
        bodyBox.addArrangedSubview(plist)

        let hint = rowLabel(L("共享期间对方可以看到终端内容；可随时停止。"
                              + "端到端加密。"), size: 11)
        hint.hbText = "termDim"
        hint.lineBreakMode = .byWordWrapping
        hint.maximumNumberOfLines = 2
        bodyBox.addArrangedSubview(hint)

        let stop = NSButton(title: L("停止共享"), target: self,
                            action: #selector(onStop))
        stop.bezelStyle = .rounded
        stop.contentTintColor = Theme.red.color
        stop.setAccessibilityIdentifier("share-stop")
        bodyBox.addArrangedSubview(stop)

        // 组装
        [signedOut, swRow, mrow, bodyBox].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            acc.addSubview($0)
        }
        NSLayoutConstraint.activate([
            acc.widthAnchor.constraint(equalToConstant: 420),
            signedOut.topAnchor.constraint(equalTo: acc.topAnchor),
            signedOut.leadingAnchor.constraint(
                equalTo: acc.leadingAnchor),
            signedOut.trailingAnchor.constraint(
                equalTo: acc.trailingAnchor),
            swRow.topAnchor.constraint(
                equalTo: signedOut.bottomAnchor, constant: 8),
            swRow.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            mrow.topAnchor.constraint(equalTo: swRow.bottomAnchor,
                                      constant: 4),
            mrow.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            mrow.trailingAnchor.constraint(equalTo: acc.trailingAnchor),
            bodyBox.topAnchor.constraint(equalTo: mrow.bottomAnchor,
                                         constant: 8),
            bodyBox.leadingAnchor.constraint(
                equalTo: acc.leadingAnchor),
            bodyBox.trailingAnchor.constraint(
                equalTo: acc.trailingAnchor),
            bodyBox.bottomAnchor.constraint(equalTo: acc.bottomAnchor),
        ])
        a.accessoryView = acc

        let nc = NotificationCenter.default
        obs.append(nc.addObserver(
            forName: ShareService.nChanged, object: nil,
            queue: .main) { [weak self] n in
                guard let self else { return }
                if self.shareId == nil ||
                   (n.object as? String) == self.shareId {
                    self.refresh()
                }
            })
        obs.append(nc.addObserver(
            forName: ShareService.nOwnerEvent, object: nil,
            queue: .main) { [weak self] n in
                guard let self,
                      (n.object as? String) == self.shareId,
                      let ev = n.userInfo?["ev"]
                          as? ShareService.ShareEv else { return }
                self.onOwnerEv(ev)
            })

        if let s = sess, let sid = ShareService.shared.shareForSession(s) {
            shareId = sid
        }
        refresh()
        DialogRegistry.shared.present(alert: a, kind: "share",
                                      in: window) { [weak self] _ in
            if let s = self {
                s.unsub()
                ShareDialog.live.removeAll { $0 === s }
            }
        }
    }

    private func unsub() {
        for o in obs { NotificationCenter.default.removeObserver(o) }
        obs = []
    }

    // ---------------- 参与者 ----------------

    private func refreshPlist() {
        plist.arrangedSubviews.forEach { plist.removeArrangedSubview($0)
            $0.removeFromSuperview() }
        let svc = ShareService.shared
        let mode = shareId.map { svc.mode($0) } ?? 0
        for p in parts {
            let row = NSView()
            let nm = rowLabel(p.name +
                (p.device.isEmpty ? "" : " · \(p.device)"))
            let pill = rowLabel(ShareKit.partPermName(
                mode == HB_SHARE_COLLAB), size: 11)
            pill.textColor = mode == HB_SHARE_COLLAB
                ? Theme.green.color : Theme.termDim.color
            let rm = NSButton(title: L("移除"), target: self,
                              action: #selector(onKick(_:)))
            rm.bezelStyle = .texturedRounded
            rm.font = .systemFont(ofSize: 11)
            rm.tag = Int(p.id)
            rm.setAccessibilityIdentifier(
                "share-kick-\(p.id)")
            [nm, pill, rm].forEach {
                $0.translatesAutoresizingMaskIntoConstraints = false
                row.addSubview($0)
            }
            row.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                nm.leadingAnchor.constraint(equalTo: row.leadingAnchor),
                nm.centerYAnchor.constraint(equalTo: row.centerYAnchor),
                rm.trailingAnchor.constraint(equalTo: row.trailingAnchor),
                rm.centerYAnchor.constraint(equalTo: row.centerYAnchor),
                pill.trailingAnchor.constraint(
                    equalTo: rm.leadingAnchor, constant: -8),
                pill.centerYAnchor.constraint(equalTo: row.centerYAnchor),
                row.heightAnchor.constraint(equalToConstant: 24),
                row.widthAnchor.constraint(equalToConstant: 400),
            ])
            plist.addArrangedSubview(row)
        }
        emptyLbl.isHidden = !parts.isEmpty
    }

    private func addPart(id: UInt32, name: String, device: String,
                         guest: Bool) {
        if let i = parts.firstIndex(where: { $0.id == id }) {
            parts[i].name = name; parts[i].device = device
            parts[i].guest = guest
        } else {
            parts.append(ShareService.Part(
                id: id, name: name.isEmpty ? "guest" : name,
                device: device, guest: guest))
        }
        refreshPlist()
    }

    private func delPart(_ id: UInt32) {
        parts.removeAll { $0.id == id }
        refreshPlist()
    }

    // ---------------- service 事件 ----------------

    private func onOwnerEv(_ ev: ShareService.ShareEv) {
        switch ev.type {
        case HB_SH_EV_JOINED:
            addPart(id: ev.id,
                    name: ev.name.isEmpty ? "guest" : ev.name,
                    device: ev.device, guest: ev.device.isEmpty)
        case HB_SH_EV_PAKE_OK:
            addPart(id: ev.id,
                    name: ev.name.isEmpty ? "guest" : ev.name,
                    device: "", guest: true)
        case HB_SH_EV_LEFT:
            delPart(ev.id)
        default: break
        }
    }

    // ---------------- 动作 ----------------

    @objc private func onGotoAccount() {
        window?.showSectionByName("account")
        // sheet 留着 —— 对齐 linux adw_dialog_close
        if let w = signedOut.window?.sheetParent {
            w.endSheet(signedOut.window!)
        }
    }

    @objc private func onSwitch() {
        if updating { return }
        guard let sess else { return }
        let svc = ShareService.shared
        if sw.state == .on && shareId == nil && !starting {
            starting = true
            let mode = modePop.indexOfSelectedItem == 1
                ? HB_SHARE_COLLAB : HB_SHARE_VIEW
            svc.start(sess: sess, title: title, host: host, mode: mode) {
                [weak self] sid, err in
                guard let self else { return }
                self.starting = false
                if let err {
                    self.updating = true
                    self.sw.state = .off
                    self.updating = false
                    NotifyCenter.shared.post(
                        kind: "err", title: L("共享失败"),
                        body: err, tabIndex: nil)
                    return
                }
                self.shareId = sid
                self.refresh()
            }
        } else if sw.state == .off, let sid = shareId {
            svc.stop(sid)
            shareId = nil
            parts = []
            refresh()
        }
    }

    @objc private func onMode() {
        if updating || shareId == nil { return }
        guard let sid = shareId else { return }
        ShareService.shared.setMode(
            sid, modePop.indexOfSelectedItem == 1
                ? HB_SHARE_COLLAB : HB_SHARE_VIEW)
        refreshPlist()
    }

    @objc private func onCopy() {
        guard let sid = shareId,
              let l = ShareService.shared.link(sid) else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(l, forType: .string)
    }

    @objc private func onRegen() {
        guard let sid = shareId else { return }
        ShareService.shared.regen(sid)
        refresh()
    }

    @objc private func onStop() {
        if let sid = shareId {
            ShareService.shared.stop(sid)
            shareId = nil
        }
        if let w = bodyBox.window?.sheetParent {
            w.endSheet(bodyBox.window!)
        }
    }

    @objc private func onKick(_ b: NSButton) {
        guard let sid = shareId else { return }
        ShareService.shared.kick(sid, UInt32(b.tag))
    }

    // ---------------- refresh ----------------

    private func refresh() {
        let svc = ShareService.shared
        let inAcct = SyncService.shared.state != .signedOut
        signedOut.isHidden = inAcct
        sw.isEnabled = inAcct && !starting
        sw.title = starting ? L("正在开启…") : L("开启云端共享")
        bodyBox.isHidden = shareId == nil
        guard let sid = shareId else { return }

        // 参与者表以 service 快照为准，事件继续增量更新
        parts = svc.participants(sid)
        refreshPlist()

        if let l = svc.link(sid) { linkLbl.stringValue = l }
        if let c = svc.code(sid), c.count >= 6 {
            codeLbl.stringValue = ShareKit.codeFmt(c)
        }
        updating = true
        modePop.selectItem(
            at: svc.mode(sid) == HB_SHARE_COLLAB ? 1 : 0)
        sw.state = .on
        updating = false
    }
}
