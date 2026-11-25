import AppKit
import CHarbor
import HarborKit

/// FR-12 共享会话 vault 区（对齐 linux/src/hb-share-view.c /
/// prototype sharedView）：本机共享卡片 + 同账号发现的共享 +
/// 配对码 guest 加入。
final class SharePage: NSView {
    private let localCards = ShareGrid()
    private let remoteCards = ShareGrid()
    private let localEmpty = VFlippedStack()
    private let remoteEmpty = vLabel(L("暂无可用共享"), dim: true)
    private let signedOut = NSStackView()
    private var obs: [NSObjectProtocol] = []
    private var accountEmail: String?
    /// 卡片 join 元数据（sid+meta）
    private var remoteItems: [ShareService.ShareItem] = []

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.hbBg = "content"

        let joinBtn = HBtn(L("通过配对码加入"), icon: "link")
        joinBtn.onTap = { [weak self] in self?.onJoinBtn() }
        joinBtn.setAccessibilityIdentifier("share-join-open")
        joinBtn.translatesAutoresizingMaskIntoConstraints = false
        addSubview(joinBtn)
        NSLayoutConstraint.activate([
            joinBtn.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 18),
            joinBtn.topAnchor.constraint(equalTo: topAnchor, constant: 12),
            joinBtn.heightAnchor.constraint(equalToConstant: 33),
            joinBtn.widthAnchor.constraint(equalToConstant: joinBtn.intrinsicContentSize.width + 28),
        ])

        // 滚动内容
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        addSubview(scroll)
        let content = VFlippedStack()
        content.orientation = .vertical
        content.alignment = .centerX
        content.spacing = 10
        content.edgeInsets = NSEdgeInsets(top: 14, left: 18, bottom: 20, right: 18)
        content.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = content
        content.widthAnchor.constraint(
            equalTo: scroll.contentView.widthAnchor).isActive = true
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: joinBtn.bottomAnchor, constant: 12),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])

        let localTitle = vSectionTitle(L("本机正在共享的会话"))
        let remoteTitle = vSectionTitle(L("来自其它设备的共享（同账号自动发现）"))
        signedOut.orientation = .horizontal
        signedOut.spacing = 10
        signedOut.addArrangedSubview(vLabel(L("登录 Harbor 账号后可共享会话或加入共享"), dim: true))
        let account = HBtn(L("前往账号登录"), sm: true)
        account.onTap = { [weak self] in self?.onGotoAccount() }
        signedOut.addArrangedSubview(account)
        localEmpty.orientation = .vertical
        localEmpty.alignment = .centerX
        localEmpty.edgeInsets = NSEdgeInsets(top: 40, left: 0, bottom: 40, right: 0)
        localEmpty.addArrangedSubview(vEmptyState(icon: "broadcast", title: L("暂无共享会话")))
        for child in [localTitle, localEmpty, localCards, remoteTitle, remoteEmpty, signedOut, remoteCards] {
            child.translatesAutoresizingMaskIntoConstraints = false
            content.addArrangedSubview(child)
            child.widthAnchor.constraint(equalTo: content.widthAnchor, constant: -36).isActive = true
        }
        content.setCustomSpacing(22, after: localEmpty)
        content.setCustomSpacing(22, after: localCards)

        let nc = NotificationCenter.default
        // 对齐 "shares" 信号（SyncService WS 推）+ "share-changed"
        obs.append(nc.addObserver(
            forName: ShareService.nListChanged, object: nil,
            queue: .main) { [weak self] _ in self?.listRemote() })
        obs.append(nc.addObserver(
            forName: ShareService.nChanged, object: nil,
            queue: .main) { [weak self] _ in self?.fillLocal() })
        obs.append(nc.addObserver(
            forName: ShareService.nOwnerEvent, object: nil,
            queue: .main) { [weak self] _ in self?.fillLocal() })
        obs.append(nc.addObserver(
            forName: .hbSyncChanged, object: nil,
            queue: .main) { [weak self] _ in self?.refreshAccount() })
        refresh()
    }

    required init?(coder: NSCoder) { fatalError() }
    deinit {
        for o in obs { NotificationCenter.default.removeObserver(o) }
    }

    // ---------------- 填充 ----------------

    private func fillLocal() {
        let svc = ShareService.shared
        let cards = svc.ownShares().sorted { $0.shareId < $1.shareId }.enumerated().map { i, info in
            let n = svc.participants(info.shareId).count
            let sub = String(format: L("%d 个参与者 · E2EE"), n)
            return ShareCard(
                title: info.title.isEmpty ? L("共享") : info.title,
                subtitle: sub, badge: info.mode == HB_SHARE_COLLAB ? L("可协作输入") : L("仅观看"),
                local: true, aid: "share-own-\(i)") { [weak self, weak tab = info.tab] in
                    guard let w = self?.window as? MainWindow,
                          let tab else { return }
                    w.selectTab(tab)
                }
        }
        localCards.setCards(cards)
        localEmpty.isHidden = !cards.isEmpty
    }

    private func fillRemote(_ items: [ShareService.ShareItem]) {
        remoteItems = items
        let dev = SyncService.shared.deviceName
        var vk = [UInt8](repeating: 0, count: 32)
        let haveVk = SyncService.shared.vkCopy(&vk)
        var cards: [ShareCard] = []
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
                ? String(format: L("%d 分钟前"), Int32(clamping: max(0, mins))) : ""
            let sub = [host, mode == HB_SHARE_COLLAB ? L("可协作输入") : L("仅观看"), when]
                .filter { !$0.isEmpty }.joined(separator: " · ")
            cards.append(ShareCard(
                title: [it.ownerDevice, title.isEmpty ? L("共享会话") : title].filter { !$0.isEmpty }.joined(separator: " · "),
                subtitle: sub, badge: nil,
                local: false, aid: "share-join-\(i)") { [weak self] in
                    self?.joinRemote(i)
                })
        }
        vk.withUnsafeMutableBufferPointer {
            $0.baseAddress?.update(repeating: 0, count: 32) }
        remoteCards.setCards(cards)
        remoteEmpty.isHidden = !cards.isEmpty || SyncService.shared.state == .signedOut
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
        guard let email = accountEmail else { return }
        ShareService.shared.list { [weak self] items, err in
            guard let self, self.accountEmail == email, err == nil, let items else { return }
            self.fillRemote(items)
        }
    }

    func refresh() {
        fillLocal()
        refreshAccount()
    }

    private func refreshAccount() {
        let sync = SyncService.shared
        let email = sync.state == .signedOut ? nil : sync.email
        signedOut.isHidden = email != nil
        guard email != accountEmail else {
            if email == nil { remoteEmpty.isHidden = true }
            return
        }
        accountEmail = email
        fillRemote([])
        listRemote()
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

private final class ShareGrid: NSView {
    override var isFlipped: Bool { true }
    private var columns = 1

    override var intrinsicContentSize: NSSize {
        let rows = (subviews.count + columns - 1) / columns
        return NSSize(width: NSView.noIntrinsicMetric, height: CGFloat(max(0, rows * 78 - 12)))
    }

    func setCards(_ cards: [ShareCard]) {
        subviews.forEach { $0.removeFromSuperview() }
        cards.forEach { addSubview($0) }
        isHidden = cards.isEmpty
        invalidateIntrinsicContentSize()
        needsLayout = true
    }

    override func layout() {
        super.layout()
        let count = max(1, Int((bounds.width + 12) / 312))
        if count != columns {
            columns = count
            invalidateIntrinsicContentSize()
        }
        let width = max(0, floor((bounds.width - CGFloat(columns - 1) * 12) / CGFloat(columns)))
        for (i, card) in subviews.enumerated() {
            card.frame = NSRect(x: CGFloat(i % columns) * (width + 12),
                                y: CGFloat(i / columns) * 78, width: width, height: 66)
        }
    }
}

private final class ShareCard: NSButton {
    private let tile = NSView()
    private let name: NSTextField
    private let detail: NSTextField
    private let badge: NSTextField?
    private var hovered = false

    init(title: String, subtitle: String, badge: String?, local: Bool,
         aid: String, action: @escaping () -> Void) {
        name = vLabel(title, bold: true)
        detail = vLabel(subtitle, dim: true)
        self.badge = badge.map { hbTagLabel($0) }
        super.init(frame: .zero)
        self.title = ""
        isBordered = false
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.hbBg = "card"
        layer?.borderWidth = 1
        layer?.hbBorderResolver = { [weak self] in
            self?.hovered == true ? Theme.cardHover.color.cgColor : nil
        }
        setAccessibilityIdentifier(aid)
        setAccessibilityLabel([title, badge, subtitle].compactMap { $0 }.joined(separator: ", "))
        onTap = action
        tile.wantsLayer = true
        tile.layer?.cornerRadius = 10
        tile.layer?.hbBg = local ? "ubuntu" : "avatarLocal"
        let icon = NSImageView(image: hbIcon(local ? "broadcast" : "phone", size: 18, tint: .white))
        icon.frame = NSRect(x: 10, y: 10, width: 18, height: 18)
        tile.addSubview(icon)
        addSubview(tile)
        name.font = .systemFont(ofSize: 13.5, weight: .semibold)
        detail.font = .systemFont(ofSize: 12)
        addSubview(name)
        addSubview(detail)
        if let badge = self.badge { addSubview(badge) }
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    required init?(coder: NSCoder) { nil }
    override func hitTest(_ point: NSPoint) -> NSView? { super.hitTest(point) == nil ? nil : self }
    override func mouseEntered(with event: NSEvent) { hovered = true; restyle() }
    override func mouseExited(with event: NSEvent) { hovered = false; restyle() }
    private func restyle() {
        layer?.hbBorderResolver = { [weak self] in
            self?.hovered == true ? Theme.cardHover.color.cgColor : nil
        }
    }

    override func layout() {
        super.layout()
        tile.frame = NSRect(x: 15, y: 14, width: 38, height: 38)
        let textWidth = max(0, bounds.width - 80)
        let badgeWidth = (badge?.frame.width ?? 0) + (badge == nil ? 0 : 7)
        let nameWidth = min(name.intrinsicContentSize.width, max(0, textWidth - badgeWidth))
        name.frame = NSRect(x: 65, y: 34, width: nameWidth, height: 18)
        badge?.frame.origin = NSPoint(x: name.frame.maxX + 7, y: 34)
        detail.frame = NSRect(x: 65, y: 15, width: textWidth, height: 16)
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .pointingHand)
    }
}
