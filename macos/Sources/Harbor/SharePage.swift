import AppKit
import CHarbor
import HarborKit

/// FR-12 共享会话 vault 区（对齐 linux/src/hb-share-view.c /
/// prototype sharedView）：本机共享卡片 + 同账号发现的共享 +
/// 配对码 guest 加入。
final class SharePage: NSView {
    private var localBox = NSStackView()
    private var remoteBox = NSStackView()
    private var remoteEmpty: NSTextField!
    private var signedOut: NSView!
    private var content: NSStackView!
    private var obs: [NSObjectProtocol] = []
    /// 卡片 join 元数据（sid+meta）
    private var remoteItems: [ShareService.ShareItem] = []

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.hbBg = "page"

        // 顶部工具行：右端"通过配对码加入"
        let bar = NSView()
        let joinBtn = NSButton(title: L("通过配对码加入"), target: self,
                               action: #selector(onJoinBtn))
        joinBtn.bezelStyle = .rounded
        joinBtn.setAccessibilityIdentifier("share-join-open")
        bar.translatesAutoresizingMaskIntoConstraints = false
        joinBtn.translatesAutoresizingMaskIntoConstraints = false
        bar.addSubview(joinBtn)
        addSubview(bar)
        NSLayoutConstraint.activate([
            bar.leadingAnchor.constraint(equalTo: leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: trailingAnchor),
            bar.topAnchor.constraint(equalTo: topAnchor),
            bar.heightAnchor.constraint(equalToConstant: 44),
            joinBtn.trailingAnchor.constraint(
                equalTo: bar.trailingAnchor, constant: -18),
            joinBtn.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
        ])

        // 滚动内容
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        addSubview(scroll)
        content = NSStackView()
        content.orientation = .vertical
        content.alignment = .leading
        content.spacing = 14
        content.edgeInsets = NSEdgeInsets(top: 4, left: 18,
                                          bottom: 18, right: 18)
        content.translatesAutoresizingMaskIntoConstraints = false
        let clip = NSClipView()
        clip.documentView = content
        scroll.contentView = clip
        content.widthAnchor.constraint(
            equalTo: clip.widthAnchor).isActive = true
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: bar.bottomAnchor),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])

        content.addArrangedSubview(secTitle(L("本机正在共享的会话")))
        localBox = box(); content.addArrangedSubview(localBox)
        content.addArrangedSubview(
            secTitle(L("来自其它设备的共享（同账号自动发现）")))
        remoteEmpty = NSTextField(labelWithString: L("暂无可用共享"))
        remoteEmpty.font = .systemFont(ofSize: 11)
        remoteEmpty.hbText = "termDim"
        content.addArrangedSubview(remoteEmpty)
        remoteBox = box(); content.addArrangedSubview(remoteBox)

        // 未登录
        signedOut = NSView()
        signedOut.translatesAutoresizingMaskIntoConstraints = false
        let l = NSTextField(labelWithString:
            L("登录 Harbor 账号后可共享会话或加入共享"))
        l.font = .systemFont(ofSize: 13)
        let b = NSButton(title: L("前往账号登录"), target: self,
                         action: #selector(onGotoAccount))
        b.bezelStyle = .rounded
        [l, b].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            signedOut.addSubview($0)
        }
        addSubview(signedOut)
        NSLayoutConstraint.activate([
            l.centerXAnchor.constraint(
                equalTo: signedOut.centerXAnchor),
            l.centerYAnchor.constraint(
                equalTo: signedOut.centerYAnchor, constant: -14),
            b.topAnchor.constraint(equalTo: l.bottomAnchor,
                                   constant: 12),
            b.centerXAnchor.constraint(
                equalTo: signedOut.centerXAnchor),
            signedOut.topAnchor.constraint(equalTo: bar.bottomAnchor),
            signedOut.leadingAnchor.constraint(
                equalTo: leadingAnchor),
            signedOut.trailingAnchor.constraint(
                equalTo: trailingAnchor),
            signedOut.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])

        let nc = NotificationCenter.default
        // 对齐 "shares" 信号（SyncService WS 推）+ "share-changed"
        obs.append(nc.addObserver(
            forName: ShareService.nListChanged, object: nil,
            queue: .main) { [weak self] _ in self?.listRemote() })
        obs.append(nc.addObserver(
            forName: ShareService.nChanged, object: nil,
            queue: .main) { [weak self] _ in self?.refresh() })
        refresh()
    }

    required init?(coder: NSCoder) { fatalError() }
    deinit {
        for o in obs { NotificationCenter.default.removeObserver(o) }
    }

    private func box() -> NSStackView {
        let v = NSStackView()
        v.orientation = .vertical
        v.alignment = .leading
        v.spacing = 6
        return v
    }
    private func secTitle(_ t: String) -> NSTextField {
        let l = NSTextField(labelWithString: t)
        l.font = .systemFont(ofSize: 12, weight: .semibold)
        l.hbText = "termDim"
        return l
    }

    // ---------------- 卡片 ----------------

    private func card(title: String, sub: String, pill: String,
                      aid: String,
                      cb: @escaping () -> Void) -> NSView {
        let b = CardButton()
        b.onClick = cb
        b.wantsLayer = true
        b.layer?.hbBg = "card"
        b.layer?.cornerRadius = 8
        b.translatesAutoresizingMaskIntoConstraints = false
        b.setAccessibilityIdentifier(aid)
        let t = NSTextField(labelWithString: title)
        t.font = .systemFont(ofSize: 13, weight: .semibold)
        t.hbText = "text"
        let s = NSTextField(labelWithString: sub)
        s.font = .systemFont(ofSize: 11)
        s.hbText = "termDim"
        let p = NSTextField(labelWithString: pill)
        p.font = .systemFont(ofSize: 11)
        p.hbText = "bluePillFg"
        [t, s, p].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            b.addSubview($0)
        }
        NSLayoutConstraint.activate([
            t.leadingAnchor.constraint(equalTo: b.leadingAnchor,
                                       constant: 14),
            t.topAnchor.constraint(equalTo: b.topAnchor,
                                   constant: 10),
            s.leadingAnchor.constraint(equalTo: t.leadingAnchor),
            s.topAnchor.constraint(equalTo: t.bottomAnchor,
                                   constant: 3),
            s.bottomAnchor.constraint(equalTo: b.bottomAnchor,
                                      constant: -10),
            p.trailingAnchor.constraint(
                equalTo: b.trailingAnchor, constant: -14),
            p.centerYAnchor.constraint(equalTo: b.centerYAnchor),
            b.heightAnchor.constraint(equalToConstant: 56),
            b.widthAnchor.constraint(greaterThanOrEqualToConstant: 320),
        ])
        return b
    }

    // ---------------- 填充 ----------------

    private func fillLocal() {
        localBox.arrangedSubviews.forEach {
            localBox.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }
        let svc = ShareService.shared
        for (i, info) in svc.ownShares().enumerated() {
            let sub = "\(info.host.isEmpty ? "localhost" : info.host) · " +
                (info.mode == HB_SHARE_COLLAB
                 ? L("可协作输入") : L("仅观看"))
            let tab = info.tab
            localBox.addArrangedSubview(card(
                title: info.title.isEmpty ? L("共享") : info.title,
                sub: sub, pill: L("共享中"),
                aid: "share-own-\(i)") {
                    guard let w = self.window as? MainWindow,
                          let tab else { return }
                    w.selectTab(tab)
                })
        }
    }

    private func fillRemote(_ items: [ShareService.ShareItem]) {
        remoteItems = items
        remoteBox.arrangedSubviews.forEach {
            remoteBox.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }
        let dev = SyncService.shared.deviceName
        var vk = [UInt8](repeating: 0, count: 32)
        let haveVk = SyncService.shared.vkCopy(&vk)
        var n = 0
        for (i, it) in items.enumerated() {
            if it.ownerDevice == dev { continue }   // 本机的在上方
            var title = "", host = "", mode = HB_SHARE_VIEW
            // 用引擎解密 meta（title/host/mode）
            if haveVk, !it.metaB64.isEmpty {
                var err = HB_OK
                var peek: OpaquePointer?
                it.shareId.withCString { sid in
                    it.metaB64.withCString { mb in
                        peek = vk.withUnsafeBufferPointer { p in
                            hb_share_peer_new_member(
                                sid, p.baseAddress, mb, "", "", &err)
                        }
                    }
                }
                if let peek {
                    defer { hb_share_peer_free(peek) }
                    title = hb_share_peer_title(peek).map {
                        String(cString: $0) } ?? ""
                    host = hb_share_peer_host(peek).map {
                        String(cString: $0) } ?? ""
                    mode = Int(hb_share_peer_meta_mode(peek))
                }
            }
            let mins = it.created > 0
                ? (Int64(Date().timeIntervalSince1970 * 1000)
                   - it.created) / 60000 : 0
            let when = it.created > 0
                ? String(format: L("%d 分钟前"), mins) : ""
            let sub = "\(it.ownerDevice)@\(host.isEmpty ? "?" : host) · " +
                (mode == HB_SHARE_COLLAB
                 ? L("可协作输入") : L("仅观看")) +
                (when.isEmpty ? "" : " · \(when)")
            let idx = i
            remoteBox.addArrangedSubview(card(
                title: title.isEmpty ? L("共享会话") : title,
                sub: sub, pill: L("加入"),
                aid: "share-join-\(idx)") { [weak self] in
                    self?.joinRemote(idx)
                })
            n += 1
        }
        vk.withUnsafeMutableBufferPointer {
            $0.baseAddress?.update(repeating: 0, count: 32) }
        remoteEmpty.isHidden = n > 0
    }

    private func joinRemote(_ i: Int) {
        guard i < remoteItems.count else { return }
        let it = remoteItems[i]
        ShareService.shared.joinMember(it.shareId, it.metaB64) {
            [weak self] ctx, err in
            self?.joinCb(ctx: ctx, err: err)
        }
    }

    private func joinCb(ctx: ShareService.PeerCtx?, err: String?) {
        if let err {
            NotifyCenter.shared.post(kind: "err",
                title: L("加入失败"), body: err, tabIndex: nil)
            return
        }
        guard let ctx,
              let w = window as? MainWindow else { return }
        w.openViewer(ctx, title: ctx.peerTitle ?? L("共享会话"))
    }

    private func listRemote() {
        ShareService.shared.list { [weak self] items, err in
            guard let self, err == nil, let items else { return }
            self.fillRemote(items)
        }
    }

    func refresh() {
        let inAcct = SyncService.shared.state != .signedOut
        signedOut.isHidden = inAcct
        content.isHidden = !inAcct
        fillLocal()
        if inAcct { listRemote() }
    }

    @objc private func onGotoAccount() {
        (window as? MainWindow)?.showSectionByName("account")
    }

    // ---------------- join 对话框 ----------------

    @objc private func onJoinBtn() {
        let a = NSAlert()
        a.messageText = L("通过配对码加入")
        a.addButton(withTitle: L("加入"))
        a.addButton(withTitle: L("取消"))
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false
        let l1 = NSTextField(labelWithString: L("粘贴共享链接："))
        l1.font = .systemFont(ofSize: 12)
        let link = NSTextField()
        link.placeholderString = "harbor://share/..."
        link.setAccessibilityIdentifier("join-link")
        let l2 = NSTextField(labelWithString: L("6 位配对码："))
        l2.font = .systemFont(ofSize: 12)
        let code = NSTextField()
        code.placeholderString = "482913"
        code.setAccessibilityIdentifier("join-code")
        [l1, link, l2, code].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            acc.addSubview($0)
        }
        NSLayoutConstraint.activate([
            acc.widthAnchor.constraint(equalToConstant: 340),
            l1.topAnchor.constraint(equalTo: acc.topAnchor),
            l1.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            link.topAnchor.constraint(equalTo: l1.bottomAnchor,
                                      constant: 4),
            link.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            link.trailingAnchor.constraint(equalTo: acc.trailingAnchor),
            l2.topAnchor.constraint(equalTo: link.bottomAnchor,
                                    constant: 10),
            l2.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            code.topAnchor.constraint(equalTo: l2.bottomAnchor,
                                      constant: 4),
            code.leadingAnchor.constraint(equalTo: acc.leadingAnchor),
            code.widthAnchor.constraint(equalToConstant: 140),
            code.bottomAnchor.constraint(equalTo: acc.bottomAnchor),
        ])
        a.accessoryView = acc
        DialogRegistry.shared.present(alert: a, kind: "share-join",
                                      in: window) { [weak self] ok in
            guard ok, let self else { return }
            let l = link.stringValue.trimmingCharacters(
                in: .whitespacesAndNewlines)
            let c = code.stringValue.replacingOccurrences(
                of: " ", with: "")
            ShareService.shared.joinGuest(l, c) { [weak self] ctx, err in
                self?.joinCb(ctx: ctx, err: err)
            }
        }
    }
}

/// 整张可点的卡片（对齐 hb-share-card 的 GtkButton）。
private final class CardButton: NSView {
    var onClick: (() -> Void)?
    override func mouseUp(with e: NSEvent) { onClick?() }
    override var acceptsFirstResponder: Bool { true }
    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .pointingHand)
    }
}
