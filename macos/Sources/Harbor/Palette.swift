import AppKit
import CHarbor
import HarborKit

/// 命令面板（FR-20，对齐 linux/src/hb-palette.c）：
///   窗口顶部浮层 scrim + 搜索框 + 结果列表（≤9），
///   fuzzy 搜索 hosts / Vault sections / snippets / commands，
///   ↑↓ 选择、Enter 执行、Esc 关、点 scrim 空白关。
final class Palette: NSView, NSTextFieldDelegate {
    /// 当前打开的实例（单例式：重复 ⌘K 只聚焦输入框）。
    private static weak var current: Palette?
    static func open(in w: MainWindow) {
        if let p = current, p.superview != nil {
            w.makeFirstResponder(p.field)
            return
        }
        let p = Palette(frame: w.contentView!.bounds)
        p.autoresizingMask = [.width, .height]
        w.contentView!.addSubview(p)
        current = p
        p.rebuild()
        w.makeFirstResponder(p.field)
    }
    static var isOpen: Bool { current?.superview != nil }
    static func closeCurrent() { current?.close() }
    // ---- dev hooks（DEBUG 命令走这里） ----
    static func devType(_ s: String) {
        guard let p = current else { return }
        p.field.stringValue = s
        p.sel = 0
        p.rebuild()
    }
    static var devItems: [(kind: String, title: String, id: String)] {
        (current?.items ?? []).map {
            ("\($0.kind)", $0.title, $0.id)
        }
    }
    static func devExec(_ i: Int) { current?.exec(i) }
    static var devSel: Int { current?.sel ?? -1 }

    private let field = NSTextField()
    private let listHost = NSView()
    private var items: [PalItem] = []
    private var sel = 0

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black
            .withAlphaComponent(0.32).cgColor   // pal-scrim
        // 顶部面板：宽 560，上缘 ~90（对齐 .pal-box）
        let panel = NSView()
        panel.wantsLayer = true
        panel.layer?.hbBg = "card"
        panel.layer?.cornerRadius = 12
        panel.layer?.hbBorder = "line"
        panel.layer?.borderWidth = 1
        panel.translatesAutoresizingMaskIntoConstraints = false
        addSubview(panel)
        // 输入行：search icon + field + esc badge
        let row = NSView()
        row.wantsLayer = true
        row.layer?.hbBg = "subtle"
        row.layer?.cornerRadius = 8
        row.translatesAutoresizingMaskIntoConstraints = false
        let ic = NSImageView(image: hbIcon("search", size: 15,
                                           tint: Theme.dyn("termDim")))
        ic.translatesAutoresizingMaskIntoConstraints = false
        field.placeholderString =
            L("搜索主机、页面、Snippet 或命令…")
        field.font = .systemFont(ofSize: 14)
        field.isBezeled = false
        field.drawsBackground = false
        field.focusRingType = .none
        field.delegate = self
        field.translatesAutoresizingMaskIntoConstraints = false
        field.setAccessibilityIdentifier("pal-field")
        let esc = NSTextField(labelWithString: "esc")
        esc.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        esc.hbText = "termDim"
        esc.translatesAutoresizingMaskIntoConstraints = false
        row.addSubview(ic); row.addSubview(field); row.addSubview(esc)
        listHost.translatesAutoresizingMaskIntoConstraints = false
        panel.addSubview(row); panel.addSubview(listHost)
        NSLayoutConstraint.activate([
            panel.centerXAnchor.constraint(equalTo: centerXAnchor),
            panel.topAnchor.constraint(equalTo: topAnchor, constant: 90),
            panel.widthAnchor.constraint(equalToConstant: 560),
            panel.bottomAnchor.constraint(lessThanOrEqualTo: bottomAnchor,
                                          constant: -40),
            row.leadingAnchor.constraint(equalTo: panel.leadingAnchor,
                                         constant: 12),
            row.trailingAnchor.constraint(equalTo: panel.trailingAnchor,
                                          constant: -12),
            row.topAnchor.constraint(equalTo: panel.topAnchor,
                                     constant: 12),
            row.heightAnchor.constraint(equalToConstant: 34),
            ic.leadingAnchor.constraint(equalTo: row.leadingAnchor,
                                        constant: 10),
            ic.centerYAnchor.constraint(equalTo: row.centerYAnchor),
            field.leadingAnchor.constraint(equalTo: ic.trailingAnchor,
                                           constant: 9),
            field.trailingAnchor.constraint(equalTo: esc.leadingAnchor,
                                            constant: -8),
            field.centerYAnchor.constraint(equalTo: row.centerYAnchor),
            esc.trailingAnchor.constraint(equalTo: row.trailingAnchor,
                                          constant: -10),
            esc.centerYAnchor.constraint(equalTo: row.centerYAnchor),
            listHost.leadingAnchor.constraint(equalTo: panel.leadingAnchor,
                                              constant: 8),
            listHost.trailingAnchor.constraint(equalTo: panel.trailingAnchor,
                                               constant: -8),
            listHost.topAnchor.constraint(equalTo: row.bottomAnchor,
                                          constant: 8),
            listHost.bottomAnchor.constraint(equalTo: panel.bottomAnchor,
                                             constant: -8),
            listHost.heightAnchor.constraint(lessThanOrEqualToConstant: 340),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }

    // ---- 收集条目（每次 rebuild 重取，数据量小） ----
    private func collect() -> [PalItem] {
        var out: [PalItem] = []
        var ord = 0
        let v = Vault.shared
        // 1. hosts
        for h in v.list("host") {
            defer { hb_obj_free(h) }
            let id = hb_obj_id(h).str
            let name = hb_obj_get_str(h, "name").str
            let addr = hb_obj_get_str(h, "address").str
            let user = hb_obj_get_str(h, "user").str
            let proto = hb_obj_get_str(h, "proto").str
            let group = hb_obj_get_str(h, "group").str
            let tags = hb_obj_get_str(h, "tags").str
            let pup = (proto.isEmpty ? "ssh" : proto).uppercased()
            var av = "generic"
            if ["local", "rdp", "vnc"].contains(proto) { av = proto }
            else {
                let os = hb_obj_get_str(h, "os").str
                if !os.isEmpty && os != "auto" { av = os }
            }
            out.append(PalItem(kind: .host, avatar: av,
                title: name.isEmpty ? (addr.isEmpty ? "host" : addr) : name,
                sub: String(format: L("连接 · %@%@"), pup, user.isEmpty ? "" : " \(user)"),
                kw: "\(name) \(addr) \(user) \(group) \(tags)",
                id: id, ord: ord))
            ord += 1
        }
        // 2. sections（与 VaultTab.kSections 顺序一致）
        for (i, sec) in kSections.enumerated() {
            let label = sec.title
            out.append(PalItem(kind: .section, icon: sec.icon,
                title: label, sub: L("Vault 页面"),
                kw: "\(label) \(sec.id)", id: "\(i)", ord: ord))
            ord += 1
        }
        // 3. snippets（scope 匹配焦点 host）
        var fhost: String?, fgroup: String?
        if let w = window as? MainWindow {
            fhost = w.focusedHostId
            fgroup = w.focusedHostGroup
        }
        for s in v.list("snippet") {
            defer { hb_obj_free(s) }
            let scope = hb_obj_get_str(s, "scope").str
            guard PaletteRank.snippetScopeMatch(scope, hostId: fhost,
                                                group: fgroup)
            else { continue }
            let title = hb_obj_get_str(s, "title").str
            let body = hb_obj_get_str(s, "body").str
            out.append(PalItem(kind: .snippet, icon: "list",
                title: title.isEmpty ? "snippet" : title,
                sub: L("发送 Snippet"), kw: "\(title) \(body)",
                id: hb_obj_id(s).str, ord: ord))
            ord += 1
        }
        // 4. commands（对齐 linux PAL_CMD 组 + workspace 子命令）
        let cmds: [(String, String, String, String, String)] = [
            ("terminal", L("Local 终端"), L("命令"),
             L("Local 终端 local terminal"), "local"),
            ("panel", L("Workspace 分屏…"), L("命令"),
             L("Workspace 分屏 workspace split panes"), "workspace"),
            ("broadcast", L("切换输入广播"), L("命令"),
             L("切换输入广播 broadcast input 广播"), "broadcast"),
            ("search", L("Focus 模式"), L("命令"),
             L("Focus 模式 focus mode 专注"), "focus"),
            ("clock", L("导出连接日志 CSV"), L("命令"),
             L("导出连接日志 CSV export logs csv"), "export-logs"),
            ("palette", L("外观与终端"), L("命令"),
             L("外观与终端 appearance settings theme"), "appearance"),
        ]
        for c in cmds {
            out.append(PalItem(kind: .cmd, icon: c.0, title: c.1,
                               sub: c.2, kw: c.3, id: c.4, ord: ord))
            ord += 1
        }
        return out
    }

    // ---- 过滤 + 渲染 ----
    private func rebuild() {
        items = PaletteRank.filter(collect(), query: field.stringValue)
        if sel >= items.count { sel = max(0, items.count - 1) }
        listHost.subviews.forEach { $0.removeFromSuperview() }
        var prev: NSView?
        for (i, it) in items.enumerated() {
            let r = PalRow(item: it, on: i == sel)
            r.translatesAutoresizingMaskIntoConstraints = false
            r.onClick = { [weak self] in self?.exec(i) }
            listHost.addSubview(r)
            NSLayoutConstraint.activate([
                r.leadingAnchor.constraint(equalTo: listHost.leadingAnchor),
                r.trailingAnchor.constraint(equalTo: listHost.trailingAnchor),
                prev == nil
                    ? r.topAnchor.constraint(equalTo: listHost.topAnchor)
                    : r.topAnchor.constraint(
                        equalTo: prev!.bottomAnchor, constant: 1),
                r.heightAnchor.constraint(equalToConstant: 34),
            ])
            prev = r
        }
        prev?.bottomAnchor.constraint(equalTo: listHost.bottomAnchor)
            .isActive = true
        if items.isEmpty {
            let e = NSTextField(labelWithString: L("没有匹配项"))
            e.hbText = "termDim"
            e.alignment = .center
            e.translatesAutoresizingMaskIntoConstraints = false
            listHost.addSubview(e)
            NSLayoutConstraint.activate([
                e.centerXAnchor.constraint(equalTo: listHost.centerXAnchor),
                e.topAnchor.constraint(equalTo: listHost.topAnchor,
                                       constant: 14),
                e.bottomAnchor.constraint(equalTo: listHost.bottomAnchor,
                                          constant: -14),
            ])
        }
        listHost.needsLayout = true
        layoutSubtreeIfNeeded()
    }

    func close() {
        removeFromSuperview()
        if let w = window as? MainWindow {
            w.restoreFocusToTab()
        }
    }

    // ---- 执行 ----
    private func exec(_ i: Int) {
        guard i >= 0 && i < items.count,
              let w = window as? MainWindow else { return }
        let it = items[i]
        close()
        w.paletteExec(it)
    }

    // ---- 输入 ----
    func controlTextDidChange(_ n: Notification) {
        sel = 0
        rebuild()
    }
    func control(_ control: NSControl, textView: NSTextView,
                 doCommandBy sel_: Selector) -> Bool {
        switch sel_ {
        case #selector(NSResponder.moveDown(_:)):
            if sel < items.count - 1 { sel += 1; rebuild() }
            return true
        case #selector(NSResponder.moveUp(_:)):
            if sel > 0 { sel -= 1; rebuild() }
            return true
        case #selector(NSResponder.insertNewline(_:)):
            if !items.isEmpty { exec(sel) }
            return true
        case #selector(NSResponder.cancelOperation(_:)):
            close()
            return true
        default:
            return false
        }
    }
    /// OS/proto key → 彩色圆角砖（对齐 osTile + 卡片样式）。
    static func osTileView(_ os: String, size: CGFloat) -> NSView {
        let t = osTile(os)
        let v = NSView()
        v.wantsLayer = true
        v.layer?.cornerRadius = size * 0.24
        v.layer?.backgroundColor = t.bg.color.cgColor
        let iv = NSImageView(image: hbIcon(t.icon, size: size * 0.62,
                                           tint: .white))
        iv.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(iv)
        NSLayoutConstraint.activate([
            v.widthAnchor.constraint(equalToConstant: size),
            v.heightAnchor.constraint(equalToConstant: size),
            iv.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            iv.centerYAnchor.constraint(equalTo: v.centerYAnchor),
        ])
        return v
    }

    /// 点击 scrim 空白 → 关（panel 内点击不外传）。
    override func mouseDown(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        // 落在任何子控件之外 = scrim
        var hit = false
        for s in subviews {
            if s.frame.contains(p) { hit = true; break }
        }
        if !hit { close() }
    }
    // 面板点击不关闭（事件被子视图消化）
}

/// 结果行：avatar/图标 + 标题 + 右侧 sub + ⏎（对齐 row_widget）。
private final class PalRow: NSView {
    var onClick: (() -> Void)?
    init(item: PalItem, on: Bool) {
        super.init(frame: .zero)
        wantsLayer = true
        if on {
            layer?.hbBg = "sel"
            layer?.cornerRadius = 7
        }
        let ico: NSView
        if item.kind == .host {
            ico = Palette.osTileView(item.avatar, size: 22)
        } else {
            let iv = NSImageView(image: hbIcon(
                item.icon.isEmpty ? "terminal" : item.icon,
                size: 15, tint: Theme.dyn("termDim")))
            ico = iv
        }
        ico.translatesAutoresizingMaskIntoConstraints = false
        let t = NSTextField(labelWithString: item.title)
        t.font = .systemFont(ofSize: 13)
        t.lineBreakMode = .byTruncatingTail
        t.translatesAutoresizingMaskIntoConstraints = false
        let s = NSTextField(labelWithString: item.sub)
        s.font = .systemFont(ofSize: 11)
        s.hbText = "termDim"
        s.alignment = .right
        s.lineBreakMode = .byTruncatingTail
        s.translatesAutoresizingMaskIntoConstraints = false
        addSubview(ico); addSubview(t); addSubview(s)
        NSLayoutConstraint.activate([
            ico.leadingAnchor.constraint(equalTo: leadingAnchor,
                                         constant: 10),
            ico.centerYAnchor.constraint(equalTo: centerYAnchor),
            ico.widthAnchor.constraint(equalToConstant: 24),
            t.leadingAnchor.constraint(equalTo: ico.trailingAnchor,
                                       constant: 10),
            t.centerYAnchor.constraint(equalTo: centerYAnchor),
            s.leadingAnchor.constraint(greaterThanOrEqualTo:
                t.trailingAnchor, constant: 8),
            s.trailingAnchor.constraint(equalTo: trailingAnchor,
                                        constant: -10),
            s.centerYAnchor.constraint(equalTo: centerYAnchor),
            s.widthAnchor.constraint(lessThanOrEqualToConstant: 220),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseDown(with e: NSEvent) { onClick?() }
}
