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
    /// M8 接缝：默认 no-op（记录调用，dev `sysnotifs` 可读）。
    var system: SystemNotifier = RecordingSystemNotifier()
    private(set) var items: [NotifItem] = []
    var onChange: (() -> Void)?

    var unread: Int { items.filter { $0.unread }.count }

    func post(kind: String, title: String, body: String,
              tabIndex: Int?) {
        let f = DateFormatter(); f.dateFormat = "HH:mm"
        items.insert(NotifItem(kind: kind, title: title, body: body,
                               tabIndex: tabIndex,
                               time: f.string(from: Date())), at: 0)
        onChange?()
    }
    func markAllRead() {
        for i in items.indices { items[i].unread = false }
        onChange?()
    }
    func clear() { items.removeAll(); onChange?() }
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
        layer?.backgroundColor = Theme.card.color.cgColor
        layer?.cornerRadius = 10
        layer?.borderColor = Theme.line.color.cgColor
        layer?.borderWidth = 1
        layer?.shadowOpacity = 0.18
        layer?.shadowRadius = 8
        layer?.shadowOffset = CGSize(width: 0, height: -3)

        let head = NSView()
        let t = NSTextField(labelWithString: L("通知中心"))
        t.font = .systemFont(ofSize: 13, weight: .semibold)
        let read = NSButton(title: L("全部已读"), target: self,
                            action: #selector(onReadAll))
        let clr = NSButton(title: L("清空"), target: self,
                           action: #selector(onClear))
        for b in [read, clr] {
            b.bezelStyle = .texturedRounded
            b.font = .systemFont(ofSize: 11)
            b.isBordered = false
            b.contentTintColor = Theme.blue.color
        }
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
            read.trailingAnchor.constraint(equalTo: clr.leadingAnchor,
                                           constant: -6),
            read.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            listHost.leadingAnchor.constraint(equalTo: leadingAnchor),
            listHost.trailingAnchor.constraint(equalTo: trailingAnchor),
            listHost.topAnchor.constraint(equalTo: head.bottomAnchor),
            listHost.bottomAnchor.constraint(equalTo: bottomAnchor),
            rowsCol.leadingAnchor.constraint(equalTo: doc.leadingAnchor),
            rowsCol.trailingAnchor.constraint(equalTo: doc.trailingAnchor),
            rowsCol.topAnchor.constraint(equalTo: doc.topAnchor),
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
            let e = NSTextField(labelWithString: L("暂无通知"))
            e.textColor = Theme.termDim.color
            e.alignment = .center
            rowsCol.addArrangedSubview(e)
            e.heightAnchor.constraint(equalToConstant: 90).isActive = true
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
    init(item n: NotifItem) {
        super.init(frame: .zero)
        wantsLayer = true
        if n.unread {
            layer?.backgroundColor = Theme.sel.color.cgColor
        }
        let iconName: String
        let tint: NSColor
        switch n.kind {
        case "ok": iconName = "check"; tint = Theme.green.color
        case "err": iconName = "x"; tint = Theme.red.color
        case "share": iconName = "broadcast"; tint = Theme.blue.color
        case "pending": iconName = "info"; tint = Theme.amber.color
        default: iconName = "info"; tint = Theme.termDim.color
        }
        let iv = NSImageView(image: hbIcon(iconName, size: 15, tint: tint))
        let t = NSTextField(labelWithString: n.title)
        t.font = .systemFont(ofSize: 12, weight: .medium)
        t.lineBreakMode = .byTruncatingTail
        let b = NSTextField(wrappingLabelWithString: n.body)
        b.font = .systemFont(ofSize: 11)
        b.textColor = Theme.termDim.color
        let tm = NSTextField(labelWithString: n.time)
        tm.font = .systemFont(ofSize: 10)
        tm.textColor = Theme.termDim.color
        [iv, t, b, tm].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            addSubview($0)
        }
        NSLayoutConstraint.activate([
            iv.leadingAnchor.constraint(equalTo: leadingAnchor,
                                        constant: 10),
            iv.topAnchor.constraint(equalTo: topAnchor, constant: 9),
            iv.widthAnchor.constraint(equalToConstant: 18),
            t.leadingAnchor.constraint(equalTo: iv.trailingAnchor,
                                       constant: 8),
            t.topAnchor.constraint(equalTo: topAnchor, constant: 7),
            t.trailingAnchor.constraint(lessThanOrEqualTo:
                tm.leadingAnchor, constant: -6),
            b.leadingAnchor.constraint(equalTo: t.leadingAnchor),
            b.trailingAnchor.constraint(equalTo: trailingAnchor,
                                        constant: -10),
            b.topAnchor.constraint(equalTo: t.bottomAnchor, constant: 1),
            b.bottomAnchor.constraint(lessThanOrEqualTo: bottomAnchor,
                                      constant: -7),
            tm.trailingAnchor.constraint(equalTo: trailingAnchor,
                                         constant: -10),
            tm.topAnchor.constraint(equalTo: t.topAnchor),
            heightAnchor.constraint(greaterThanOrEqualToConstant: 42),
        ])
        setAccessibilityIdentifier("notif-item")
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseDown(with e: NSEvent) { onClick?() }
}
