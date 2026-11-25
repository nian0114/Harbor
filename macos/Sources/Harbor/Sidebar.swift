import AppKit
import CHarbor
import HarborKit

// ---------- 侧栏（对齐 prototype .sidebar/.side-item/.account-chip）----------
// 200px、padding 14x10、行圆角 8：active sidebarAct+w600，hover #eef1f5；
// 底部账户 chip：26px 渐变头像 + 邮箱 + 绿色同步状态。
final class SidebarView: NSView {
    var onSelect: ((Int) -> Void)?
    private var items: [SideItem] = []
    private var selected = 0
    private weak var acctL1: NSTextField?
    private weak var acctL2: NSTextField?
    private weak var avatar: AvatarView?
    private var acctObs: NSObjectProtocol?
    private var wideAvatar: NSLayoutConstraint!
    private var compactAvatar: NSLayoutConstraint!
    var compact = false {
        didSet {
            guard oldValue != compact else { return }
            items.forEach { $0.compact = compact }
            acctL1?.isHidden = compact
            acctL2?.isHidden = compact
            (compact ? wideAvatar : compactAvatar).isActive = false
            (compact ? compactAvatar : wideAvatar).isActive = true
        }
    }

    init() {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.hbBg = "sidebar"
        // 右缘 1px 分隔线
        let edge = NSView()
        edge.wantsLayer = true
        edge.layer?.hbBg = "line"
        edge.translatesAutoresizingMaskIntoConstraints = false
        addSubview(edge)
        NSLayoutConstraint.activate([
            edge.trailingAnchor.constraint(equalTo: trailingAnchor),
            edge.topAnchor.constraint(equalTo: topAnchor),
            edge.bottomAnchor.constraint(equalTo: bottomAnchor),
            edge.widthAnchor.constraint(equalToConstant: 1),
        ])
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = 2
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        for (i, s) in kSections.enumerated() {
            let it = SideItem(index: i, icon: s.icon, title: s.title)
            it.onTap = { [weak self] idx in
                self?.select(idx)
                self?.onSelect?(idx)
            }
            items.append(it)
            stack.addArrangedSubview(it)
            it.heightAnchor.constraint(equalToConstant: 34).isActive = true
        }
        // 账户页脚：未登录 / 点击登录 → 登录后显示邮箱与同步状态（M6）
        let foot = NSButton()
        foot.title = ""
        foot.isBordered = false
        foot.translatesAutoresizingMaskIntoConstraints = false
        foot.setAccessibilityIdentifier("sidebar-account")
        foot.setAccessibilityLabel(L("账号与同步"))
        foot.target = self
        foot.action = #selector(accountTap)
        let av = AvatarView()
        av.translatesAutoresizingMaskIntoConstraints = false
        let f1 = NSTextField(labelWithString: L("未登录"))
        f1.font = .systemFont(ofSize: 12.5, weight: .semibold)
        f1.translatesAutoresizingMaskIntoConstraints = false
        let f2 = NSTextField(labelWithString: L("点击登录"))
        f2.font = .systemFont(ofSize: 11)
        f2.translatesAutoresizingMaskIntoConstraints = false
        [f1, f2].forEach { $0.lineBreakMode = .byTruncatingTail }
        acctL1 = f1; acctL2 = f2; avatar = av
        acctObs = NotificationCenter.default.addObserver(
            forName: .hbSyncChanged, object: nil, queue: .main) {
            [weak self] _ in self?.refreshAccount()
        }
        refreshAccount()
        foot.addSubview(av); foot.addSubview(f1); foot.addSubview(f2)
        let sep = NSView()
        sep.wantsLayer = true
        sep.layer?.hbBg = "line"
        sep.translatesAutoresizingMaskIntoConstraints = false
        addSubview(sep); addSubview(foot)
        wideAvatar = av.leadingAnchor.constraint(equalTo: foot.leadingAnchor, constant: 12)
        compactAvatar = av.centerXAnchor.constraint(equalTo: foot.centerXAnchor)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            stack.trailingAnchor.constraint(equalTo: edge.leadingAnchor, constant: -10),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            sep.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            sep.trailingAnchor.constraint(equalTo: edge.leadingAnchor, constant: -16),
            sep.heightAnchor.constraint(equalToConstant: 1),
            sep.bottomAnchor.constraint(equalTo: foot.topAnchor, constant: -8),
            foot.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            foot.trailingAnchor.constraint(equalTo: edge.leadingAnchor, constant: -10),
            foot.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),
            foot.heightAnchor.constraint(equalToConstant: 44),
            wideAvatar,
            av.centerYAnchor.constraint(equalTo: foot.centerYAnchor),
            av.widthAnchor.constraint(equalToConstant: 26),
            av.heightAnchor.constraint(equalToConstant: 26),
            f1.leadingAnchor.constraint(equalTo: av.trailingAnchor, constant: 9),
            f1.topAnchor.constraint(equalTo: foot.topAnchor, constant: 7),
            f2.leadingAnchor.constraint(equalTo: f1.leadingAnchor),
            f2.topAnchor.constraint(equalTo: f1.bottomAnchor, constant: 1),
        ])
        select(0)
    }
    required init?(coder: NSCoder) { nil }
    deinit {
        if let o = acctObs {
            NotificationCenter.default.removeObserver(o)
        }
    }
    @objc private func accountTap() {
        // 账户是侧栏最后一个分区
        onSelect?(kSections.count - 1)
    }
    private func refreshAccount() {
        let s = SyncService.shared
        if s.state == .signedOut {
            acctL1?.stringValue = L("未登录")
            acctL2?.stringValue = L("点击登录")
            acctL2?.textColor = Theme.text2.color
            avatar?.letter = nil
        } else {
            let email = s.email ?? L("账号")
            acctL1?.stringValue = email
            acctL2?.stringValue = s.statusText
            acctL2?.textColor = Theme.green.color
            avatar?.letter = email.first.map { String($0).uppercased() }
        }
    }
    func select(_ i: Int) {
        guard i >= 0 && i < items.count else { return }
        selected = i
        for (j, it) in items.enumerated() { it.isOn = j == i }
    }
}

/// 侧栏行：圆角 8，icon 16 + 13.5px 标签；active 加粗。
private final class SideItem: NSView {
    var onTap: ((Int) -> Void)?
    var isOn = false { didSet { needsDisplay = true } }
    var compact = false { didSet { needsDisplay = true } }
    private let index: Int
    private let iconName: String
    private let title: String
    private var hover = false { didSet { needsDisplay = true } }
    private var tracking: NSTrackingArea?

    init(index: Int, icon: String, title: String) {
        self.index = index; self.iconName = icon; self.title = title
        super.init(frame: .zero)
        toolTip = title
    }
    required init?(coder: NSCoder) { nil }
    override var isFlipped: Bool { true }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = tracking { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds,
            options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil)
        addTrackingArea(t)
        tracking = t
    }
    override func mouseEntered(with e: NSEvent) { hover = true }
    override func mouseExited(with e: NSEvent) { hover = false }
    override func mouseDown(with e: NSEvent) { onTap?(index) }
    override func draw(_ r: NSRect) {
        if isOn {
            Theme.sidebarAct.color.setFill()
            NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
        } else if hover {
            Theme.hover.color.setFill()
            NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
        }
        let tint = isOn ? Theme.text.color : Theme.text2.color
        if iconName == "braces" {
            let p = NSMutableParagraphStyle()
            p.alignment = .center
            ("{ }" as NSString).draw(
                in: NSRect(x: compact ? bounds.midX - 10 : 10,
                           y: bounds.midY - 8, width: 20, height: 16),
                withAttributes: [
                    .font: NSFont.monospacedSystemFont(ofSize: 12, weight: .bold),
                    .foregroundColor: tint,
                    .paragraphStyle: p])
        } else {
            hbIcon(iconName, size: 16, tint: tint).draw(
                in: NSRect(x: compact ? bounds.midX - 8 : 12,
                           y: bounds.midY - 8, width: 16, height: 16))
        }
        guard !compact else { return }
        (title as NSString).draw(
            in: NSRect(x: 38, y: bounds.midY - 8.5,
                       width: bounds.width - 48, height: 18),
            withAttributes: [
                .font: NSFont.systemFont(ofSize: 13.5,
                                         weight: isOn ? .semibold : .regular),
                .foregroundColor: Theme.text.color])
    }
}

/// 26px 圆形渐变头像（135deg #4aa3ff→#7b61ff）；未登录画 user 图标。
private final class AvatarView: NSView {
    var letter: String? { didSet { needsDisplay = true } }
    override var isFlipped: Bool { true }
    override func draw(_ r: NSRect) {
        let path = NSBezierPath(ovalIn: bounds)
        if let letter {
            NSGraphicsContext.saveGraphicsState()
            path.addClip()
            let grad = NSGradient(colors: [
                NSColor(srgbRed: 0x4a / 255, green: 0xa3 / 255,
                        blue: 0xff / 255, alpha: 1),
                NSColor(srgbRed: 0x7b / 255, green: 0x61 / 255,
                        blue: 0xff / 255, alpha: 1)])
            grad?.draw(in: bounds, angle: -45)
            NSGraphicsContext.restoreGraphicsState()
            let p = NSMutableParagraphStyle(); p.alignment = .center
            (letter as NSString).draw(
                in: NSRect(x: 0, y: bounds.midY - 7.5,
                           width: bounds.width, height: 15),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 12, weight: .bold),
                    .foregroundColor: NSColor.white,
                    .paragraphStyle: p])
        } else {
            Theme.chip.color.setFill(); path.fill()
            hbIcon("user", size: 14, tint: Theme.text2.color).draw(
                in: NSRect(x: 6, y: 6, width: 14, height: 14))
        }
    }
}
