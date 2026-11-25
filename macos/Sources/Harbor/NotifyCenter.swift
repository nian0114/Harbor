import AppKit
import HarborKit

/// 应用内通知中心（FR-13，对齐 linux hb-notify.c + hb-window.c 通知面板）。
/// 数据：newest-first 列表 + 未读数；UI：铃铛弹层 + 红色计数角标。
/// 系统通知（UNUserNotificationCenter）是 M8 —— 此处仅留接缝。

/// M8 接缝：macOS 系统通知实现（UNUserNotificationCenter）。
/// 当前为 no-op —— 实现方在 AppDelegate 启动时替换 NotifyCenter.system。
protocol SystemNotifier {
    /// 命中后由 WatchRoute 在通过 focus/quiet 过滤后调用。
    func post(title: String, body: String, tabIndex: Int?,
              priorityHigh: Bool)
}
/// M8 前的 no-op 实现。
final class NoopSystemNotifier: SystemNotifier {
    func post(title: String, body: String, tabIndex: Int?,
              priorityHigh: Bool) {}
}

struct NotifItem {
    var kind: String     // ok | err | info | pending | share
    var title: String
    var body: String
    var tabIndex: Int?   // 点击跳转目标（-1/nil 无）
    var time: String     // "HH:MM"
    var unread = true
}

/// dev 可观测的 no-op 系统通知（DEBUG/测试：记录本应弹出的系统通知）。
final class RecordingSystemNotifier: SystemNotifier {
    private(set) var calls: [String] = []
    func post(title: String, body: String, tabIndex: Int?,
              priorityHigh: Bool) {
        calls.append("\(priorityHigh ? "high" : "norm")|\(title)|\(body)")
        if calls.count > 200 { calls.removeFirst(calls.count - 200) }
    }
}

final class NotifyCenter {
    static let shared = NotifyCenter()
    /// M8 实现：UNUserNotificationCenter（UNSystemNotifier.swift）。
    /// 仍记录调用，dev `sysnotifs` 可读。
    var system: SystemNotifier = UNSystemNotifier()
    private(set) var items: [NotifItem] = []
    var onChange: (() -> Void)?

    var unread: Int { items.filter { $0.unread }.count }

    private func changed() {
        onChange?()
        NotificationCenter.default.post(
            name: .init("hb-notif-changed"), object: nil)
    }
    func post(kind: String, title: String, body: String,
              tabIndex: Int?) {
        let f = DateFormatter(); f.dateFormat = "HH:mm"
        items.insert(NotifItem(kind: kind, title: title, body: body,
                               tabIndex: tabIndex,
                               time: f.string(from: Date())), at: 0)
        changed()
    }
    func markAllRead() {
        for i in items.indices { items[i].unread = false }
        changed()
    }
    func clear() { items.removeAll(); changed() }
    /// dev hook：JSON 摘要
    var json: String {
        let arr: [[String: Any]] = items.map {
            ["kind": $0.kind, "title": $0.title, "body": $0.body,
             "time": $0.time, "unread": $0.unread,
             "tab": $0.tabIndex ?? -1] as [String: Any]
        }
        let d = try? JSONSerialization.data(withJSONObject: arr)
        return d.flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
    }
}

/// 铃铛弹层：头部（标题/全部已读/清空）+ 滚动列表。
/// 由 MainWindow 挂在铃铛下方，点击行 → onJump(tabIndex)。
final class NotifyPanel: NSView {
    var onJump: ((Int) -> Void)?
    var onClose: (() -> Void)?
    private let listHost = NSScrollView()
    private let rowsCol = NSStackView()

    init() {
        super.init(frame: NSRect(x: 0, y: 0, width: 380, height: 460))
        wantsLayer = true
        layer?.hbBg = "card"
        layer?.cornerRadius = 12
        layer?.shadowOpacity = 0.25
        layer?.shadowRadius = 18
        layer?.shadowOffset = CGSize(width: 0, height: -6)

        let head = NSView()
        let t = NSTextField(labelWithString: L("通知中心"))
        t.font = .systemFont(ofSize: 13.5, weight: .bold)
        t.hbText = "text"
        let read = HBtn(L("全部已读"), style: .ghost, sm: true)
        read.target = self; read.action = #selector(onReadAll)
        let clr = HBtn(L("清空"), style: .ghost, sm: true)
        clr.target = self; clr.action = #selector(onClear)
        read.setAccessibilityIdentifier("notif-readall")
        clr.setAccessibilityIdentifier("notif-clear")
        [head, t, read, clr, listHost].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false }
        head.addSubview(t); head.addSubview(read); head.addSubview(clr)
        addSubview(head); addSubview(listHost)
        listHost.hasVerticalScroller = true
        listHost.borderType = .noBorder
        listHost.drawsBackground = false
        rowsCol.orientation = .vertical
        rowsCol.spacing = 2
        rowsCol.translatesAutoresizingMaskIntoConstraints = false
        let doc = NSView()
        doc.translatesAutoresizingMaskIntoConstraints = false
        doc.addSubview(rowsCol)
        listHost.documentView = doc
        NSLayoutConstraint.activate([
            head.leadingAnchor.constraint(equalTo: leadingAnchor),
            head.trailingAnchor.constraint(equalTo: trailingAnchor),
            head.topAnchor.constraint(equalTo: topAnchor),
            head.heightAnchor.constraint(equalToConstant: 38),
            t.leadingAnchor.constraint(equalTo: head.leadingAnchor,
                                       constant: 12),
            t.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            clr.trailingAnchor.constraint(equalTo: head.trailingAnchor,
                                          constant: -10),
            clr.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            clr.widthAnchor.constraint(equalToConstant: 46),
            clr.heightAnchor.constraint(equalToConstant: 24),
            read.trailingAnchor.constraint(equalTo: clr.leadingAnchor,
                                           constant: -4),
            read.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            read.widthAnchor.constraint(equalToConstant: 68),
            read.heightAnchor.constraint(equalToConstant: 24),
            listHost.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
            listHost.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            listHost.topAnchor.constraint(equalTo: head.bottomAnchor),
            listHost.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
            rowsCol.leadingAnchor.constraint(equalTo: doc.leadingAnchor),
            rowsCol.trailingAnchor.constraint(equalTo: doc.trailingAnchor),
            rowsCol.topAnchor.constraint(equalTo: doc.topAnchor),
            // 缺此约束 doc 高度塌成 0 → 列表永远空白
            rowsCol.bottomAnchor.constraint(equalTo: doc.bottomAnchor),
            doc.widthAnchor.constraint(equalTo: listHost.widthAnchor),
        ])
        rebuild()
        NotificationCenter.default.addObserver(
            self, selector: #selector(rebuild),
            name: .init("hb-notif-changed"), object: nil)
    }
    required init?(coder: NSCoder) { fatalError() }

    @objc private func onReadAll() {
        NotifyCenter.shared.markAllRead(); rebuild()
    }
    @objc private func onClear() {
        NotifyCenter.shared.clear(); rebuild()
    }
    @objc func rebuild() {
        rowsCol.arrangedSubviews.forEach { rowsCol.removeArrangedSubview($0)
            $0.removeFromSuperview() }
        let items = NotifyCenter.shared.items
        if items.isEmpty {
            let wrap = NSStackView()
            wrap.orientation = .vertical; wrap.spacing = 4; wrap.alignment = .centerX
            let e = NSTextField(labelWithString: L("暂无通知"))
            e.font = .systemFont(ofSize: 13, weight: .semibold)
            e.hbText = "text2"
            let sub = NSTextField(labelWithString: L("会话完成提醒会出现在这里（FR-13）"))
            sub.font = .systemFont(ofSize: 11.5)
            sub.hbText = "text3"
            wrap.addArrangedSubview(NSView())
            wrap.addArrangedSubview(e); wrap.addArrangedSubview(sub)
            wrap.addArrangedSubview(NSView())
            rowsCol.addArrangedSubview(wrap)
            wrap.heightAnchor.constraint(equalToConstant: 110).isActive = true
            return
        }
        for (i, n) in items.enumerated() {
            let r = NotifRow(item: n)
            r.onClick = { [weak self] in
                if let ti = n.tabIndex, ti >= 0 { self?.onJump?(ti) }
            }
            rowsCol.addArrangedSubview(r)
            _ = i
        }
    }
    /// 面板外点击 → 关闭（由 MainWindow 的 event monitor 调）。
    func outsideClick() { onClose?() }
}

private final class NotifRow: NSView {
    var onClick: (() -> Void)?
    private let unread: Bool
    private var hovered = false { didSet { restyle() } }
    init(item n: NotifItem) {
        self.unread = n.unread
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 9
        restyle()
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))

        let iconName: String
        let bgToken: String
        switch n.kind {
        case "ok": iconName = "check"; bgToken = "green"
        case "err": iconName = "x"; bgToken = "red"
        case "share": iconName = "broadcast"; bgToken = "blue"
        case "pending": iconName = "info"; bgToken = "amber"
        default: iconName = "info"; bgToken = "blue"
        }
        let tile = NSView()
        tile.wantsLayer = true
        tile.layer?.cornerRadius = 8
        tile.layer?.hbBg = bgToken
        let iv = NSImageView(image: hbIcon(iconName, size: 15, tint: .white))
        iv.translatesAutoresizingMaskIntoConstraints = false
        tile.addSubview(iv)

        let t = NSTextField(labelWithString: n.title)
        t.font = .systemFont(ofSize: 12.5, weight: .semibold)
        t.hbText = "text"
        t.lineBreakMode = .byTruncatingTail
        let b = NSTextField(wrappingLabelWithString: n.body)
        b.font = .systemFont(ofSize: 12)
        b.hbText = "text2"
        let tm = NSTextField(labelWithString: n.time)
        tm.font = .systemFont(ofSize: 11)
        tm.hbText = "text3"
        [tile, t, b, tm].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            addSubview($0)
        }
        NSLayoutConstraint.activate([
            iv.centerXAnchor.constraint(equalTo: tile.centerXAnchor),
            iv.centerYAnchor.constraint(equalTo: tile.centerYAnchor),
            tile.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            tile.topAnchor.constraint(equalTo: topAnchor, constant: 10),
            tile.widthAnchor.constraint(equalToConstant: 30),
            tile.heightAnchor.constraint(equalToConstant: 30),
            t.leadingAnchor.constraint(equalTo: tile.trailingAnchor, constant: 10),
            t.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            t.topAnchor.constraint(equalTo: topAnchor, constant: 9),
            b.leadingAnchor.constraint(equalTo: t.leadingAnchor),
            b.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            b.topAnchor.constraint(equalTo: t.bottomAnchor, constant: 2),
            tm.leadingAnchor.constraint(equalTo: t.leadingAnchor),
            tm.topAnchor.constraint(equalTo: b.bottomAnchor, constant: 3),
            tm.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -9),
        ])
        setAccessibilityIdentifier("notif-item")
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseEntered(with e: NSEvent) { hovered = true }
    override func mouseExited(with e: NSEvent) { hovered = false }
    override func mouseDown(with e: NSEvent) { onClick?() }
    private func restyle() {
        layer?.hbBgResolver = { [weak self] in
            guard let self else { return nil }
            if self.unread {
                return (self.hovered ? Theme.sel : Theme.notifUnread).color.cgColor
            }
            return self.hovered ? Theme.notifHover.color.cgColor : nil
        }
    }
}
