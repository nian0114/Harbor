import AppKit
import CHarbor
import HarborKit

/// 会话侧「助手栏」—— 对齐 prototype workspaceView 的 assist 栏：
/// a-tabs（Snippets/主题/Autocomplete）+ a-search + 分页内容；
/// 颜色随终端主题（--t-bar/--t-line/--t-border/--t-title/--t-muted）。
final class AssistPanel: NSView {
    var onSend: ((String) -> Void)?
    var onSearch: (() -> Void)?
    private var theme = TermTheme.for("harbor")
    private var at = "snip"
    private let leftSep = NSView()
    private let tabsStack = NSStackView()
    private var tabBtns: [String: TBtn] = [:]
    private let searchRow = NSView()
    private let searchLbl = NSTextField(
        labelWithString: L("Search terminal · ⌘F"))
    private let searchKeys = NSTextField(labelWithString: "⌃ ⌄")
    private let scroll = NSScrollView()
    private let stack = FlippedStack()
    private var items: [OpaquePointer] = []

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        leftSep.wantsLayer = true
        searchRow.wantsLayer = true
        searchRow.layer?.cornerRadius = 7
        searchRow.layer?.borderWidth = 1
        tabsStack.orientation = .horizontal
        tabsStack.spacing = 6
        for (id, icon, mono) in [("snip", nil as String?, true),
                                 ("theme", "palette", false),
                                 ("auto", "wand", false)] {
            let b = TBtn(id == "snip" ? "{ }" : "", icon: icon,
                         target: self, action: #selector(tabHit(_:)))
            b.mono = mono
            b.identifier = NSUserInterfaceItemIdentifier(id)
            b.setAccessibilityIdentifier("assist-tab-" + id)
            tabBtns[id] = b
            tabsStack.addArrangedSubview(b)
        }
        searchLbl.font = .systemFont(ofSize: 12)
        searchKeys.font = .systemFont(ofSize: 12)
        searchRow.addGestureRecognizer(NSClickGestureRecognizer(
            target: self, action: #selector(searchHit)))
        searchRow.setAccessibilityIdentifier("assist-search")
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        scroll.documentView = stack
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.drawsBackground = false
        [leftSep, tabsStack, searchRow, searchLbl, searchKeys,
         scroll, stack].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false }
        addSubview(leftSep); addSubview(tabsStack); addSubview(searchRow)
        searchRow.addSubview(searchLbl); searchRow.addSubview(searchKeys)
        addSubview(scroll)
        NSLayoutConstraint.activate([
            leftSep.leadingAnchor.constraint(equalTo: leadingAnchor),
            leftSep.topAnchor.constraint(equalTo: topAnchor),
            leftSep.bottomAnchor.constraint(equalTo: bottomAnchor),
            leftSep.widthAnchor.constraint(equalToConstant: 1),
            tabsStack.topAnchor.constraint(equalTo: topAnchor, constant: 12),
            tabsStack.leadingAnchor.constraint(equalTo: leadingAnchor,
                                               constant: 12),
            searchRow.topAnchor.constraint(equalTo: tabsStack.bottomAnchor,
                                           constant: 10),
            searchRow.leadingAnchor.constraint(equalTo: leadingAnchor,
                                               constant: 12),
            searchRow.trailingAnchor.constraint(equalTo: trailingAnchor,
                                                constant: -12),
            searchRow.heightAnchor.constraint(equalToConstant: 30),
            searchLbl.leadingAnchor.constraint(equalTo: searchRow.leadingAnchor,
                                               constant: 10),
            searchLbl.centerYAnchor.constraint(equalTo: searchRow.centerYAnchor),
            searchKeys.trailingAnchor.constraint(
                equalTo: searchRow.trailingAnchor, constant: -10),
            searchKeys.centerYAnchor.constraint(equalTo: searchRow.centerYAnchor),
            scroll.topAnchor.constraint(equalTo: searchRow.bottomAnchor,
                                        constant: 10),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor,
                                            constant: 12),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor,
                                             constant: -12),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor,
                                           constant: -12),
            stack.topAnchor.constraint(equalTo: scroll.contentView.topAnchor),
            stack.leadingAnchor.constraint(
                equalTo: scroll.contentView.leadingAnchor),
            stack.widthAnchor.constraint(
                equalTo: scroll.contentView.widthAnchor),
        ])
        applyTheme(theme)
        reload()
    }
    required init?(coder: NSCoder) { fatalError() }

    func applyTheme(_ t: TermTheme) {
        theme = t
        layer?.backgroundColor = t.bar.color.cgColor
        leftSep.layer?.backgroundColor = t.line.color.cgColor
        searchRow.layer?.backgroundColor = t.line.color.cgColor
        searchRow.layer?.borderColor = t.border.color.cgColor
        searchLbl.textColor = theme.dim.color
        searchKeys.textColor = theme.dim.color
        scroll.appearance = NSAppearance(
            named: t.id == "light" ? .aqua : .darkAqua)
        for b in tabBtns.values { b.applyTheme(t) }
        reload()
    }

    @objc private func tabHit(_ b: NSButton) {
        guard let id = b.identifier?.rawValue else { return }
        at = id
        reload()
    }
    @objc private func searchHit() { onSearch?() }

    private func muted(_ s: String) -> NSTextField {
        let l = NSTextField(labelWithString: s)
        l.font = .systemFont(ofSize: 11.5)
        l.textColor = theme.muted.color
        l.maximumNumberOfLines = 0
        l.lineBreakMode = .byWordWrapping
        l.translatesAutoresizingMaskIntoConstraints = false
        return l
    }
    /// 先入 stack 再约宽 —— 约束需共同祖先。
    private var bodyCons: [NSLayoutConstraint] = []
    private func addFull(_ v: NSView) {
        stack.addArrangedSubview(v)
        let c = v.widthAnchor.constraint(equalTo: stack.widthAnchor)
        c.isActive = true
        bodyCons.append(c)
    }

    func reload() {
        for o in items { hb_obj_free(o) }
        items = []
        bodyCons.forEach { $0.isActive = false }
        bodyCons = []
        stack.arrangedSubviews.forEach {
            stack.removeArrangedSubview($0); $0.removeFromSuperview() }
        for (id, b) in tabBtns { b.mode = id == at ? .on : .plain }
        switch at {
        case "theme":
            addFull(muted(L("终端主题（实时生效）：")))
            let cur = Vault.shared.setting("theme") ?? "harbor"
            for t in TermTheme.all {
                let b = TBtn(t.name, target: self, action: #selector(themeHit(_:)))
                b.identifier = NSUserInterfaceItemIdentifier(t.id)
                b.setAccessibilityIdentifier("assist-theme-" + t.id)
                b.leadingImage = swatch(t.cursor.color)
                b.mode = t.id == cur ? .on : .plain
                b.applyTheme(theme)
                b.translatesAutoresizingMaskIntoConstraints = false
                addFull(b)
            }
        case "auto":
            let row = NSView()
            row.translatesAutoresizingMaskIntoConstraints = false
            let lbl = NSTextField(labelWithString: "Autocomplete")
            lbl.font = .systemFont(ofSize: 12)
            lbl.textColor = theme.title.color
            let betaWrap = NSView()
            betaWrap.wantsLayer = true
            betaWrap.layer?.borderWidth = 1
            betaWrap.layer?.borderColor = Theme.RGBA(0x3a4763).color.cgColor
            betaWrap.layer?.cornerRadius = 4
            let beta = NSTextField(labelWithString: "BETA")
            beta.font = .systemFont(ofSize: 9.5)
            beta.textColor = Theme.RGBA(0x8b96ad).color
            beta.translatesAutoresizingMaskIntoConstraints = false
            betaWrap.addSubview(beta)
            NSLayoutConstraint.activate([
                beta.leadingAnchor.constraint(equalTo: betaWrap.leadingAnchor, constant: 5),
                beta.trailingAnchor.constraint(equalTo: betaWrap.trailingAnchor, constant: -5),
                beta.topAnchor.constraint(equalTo: betaWrap.topAnchor, constant: 1),
                beta.bottomAnchor.constraint(equalTo: betaWrap.bottomAnchor, constant: -1),
            ])
            let sw = GSwitch()
            sw.isOn = Vault.shared.settingsBool("autocomplete", false)
            sw.onChange = { on in
                Vault.shared.setSettingsBool("autocomplete", on)
            }
            [lbl, betaWrap, sw].forEach {
                $0.translatesAutoresizingMaskIntoConstraints = false
                row.addSubview($0) }
            NSLayoutConstraint.activate([
                row.heightAnchor.constraint(equalToConstant: 22),
                lbl.leadingAnchor.constraint(equalTo: row.leadingAnchor),
                lbl.centerYAnchor.constraint(equalTo: row.centerYAnchor),
                betaWrap.leadingAnchor.constraint(equalTo: lbl.trailingAnchor,
                                                  constant: 6),
                betaWrap.centerYAnchor.constraint(equalTo: row.centerYAnchor),
                sw.trailingAnchor.constraint(equalTo: row.trailingAnchor),
                sw.centerYAnchor.constraint(equalTo: row.centerYAnchor),
            ])
            addFull(row)
            addFull(muted(
                L("基于 Shell History 与 OSC 133 上下文的本地补全候选；默认关闭，历史不出设备（隐私）")))
        default:
            addFull(muted(L("Snippets 快捷发送：")))
            items = Vault.shared.list("snippet")
            for o in items {
                let id = String(cString: hb_obj_id(o))
                let title = hb_obj_get_str(o, "title").str
                let b = TBtn("{ } \(title.isEmpty ? "snippet" : title)",
                             target: self, action: #selector(send(_:)))
                b.setAccessibilityIdentifier("assist-snip-" + id)
                b.tag = items.firstIndex(where: {
                    String(cString: hb_obj_id($0)) == id }) ?? 0
                b.applyTheme(theme)
                b.translatesAutoresizingMaskIntoConstraints = false
                addFull(b)
            }
            if items.isEmpty {
                addFull(muted(L("暂无 Snippet")))
            }
        }
    }

    private func swatch(_ c: NSColor) -> NSImage {
        NSImage(size: NSSize(width: 12, height: 12), flipped: false) { r in
            let p = NSBezierPath(roundedRect: r.insetBy(dx: 0.5, dy: 0.5),
                                 xRadius: 4, yRadius: 4)
            c.setFill(); p.fill()
            NSColor.white.withAlphaComponent(0.15).setStroke()
            p.lineWidth = 1; p.stroke()
            return true
        }
    }

    @objc private func themeHit(_ b: NSButton) {
        guard let id = b.identifier?.rawValue else { return }
        Vault.shared.setSetting("theme", id)
        (window as? MainWindow)?.applySettings()
        reload()
    }
    @objc private func send(_ b: NSButton) {
        guard b.tag >= 0, b.tag < items.count else { return }
        onSend?(String(cString: hb_obj_id(items[b.tag])))
    }

    deinit { for o in items { hb_obj_free(o) } }
}

/// 顶向下 stack（scroll documentView 用）。
private final class FlippedStack: NSStackView {
    override var isFlipped: Bool { true }
}

/// prototype .switch.green：38x22 轨道 + 18 圆钮，on=绿。
final class GSwitch: NSControl {
    var isOn = false { didSet { lay() } }
    var onChange: ((Bool) -> Void)?
    private let track = CALayer()
    private let knob = CALayer()

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        track.cornerRadius = 11
        knob.cornerRadius = 9
        knob.backgroundColor = NSColor.white.cgColor
        knob.shadowColor = NSColor.black.cgColor
        knob.shadowOpacity = 0.25
        knob.shadowRadius = 1.5
        knob.shadowOffset = CGSize(width: 0, height: 1)
        layer?.addSublayer(track)
        layer?.addSublayer(knob)
        translatesAutoresizingMaskIntoConstraints = false
        widthAnchor.constraint(equalToConstant: 38).isActive = true
        heightAnchor.constraint(equalToConstant: 22).isActive = true
        lay()
    }
    required init?(coder: NSCoder) { fatalError() }

    private func lay() {
        CATransaction.begin()
        CATransaction.setAnimationDuration(0.18)
        track.frame = bounds
        track.backgroundColor = (isOn ? Theme.green.color
                                 : Theme.RGBA(0xcfd6de).color).cgColor
        knob.frame = CGRect(x: isOn ? 18 : 2, y: 2, width: 18, height: 18)
        CATransaction.commit()
    }
    override func layout() { super.layout(); lay() }
    override func mouseDown(with event: NSEvent) {
        isOn.toggle()
        onChange?(isOn)
    }
}