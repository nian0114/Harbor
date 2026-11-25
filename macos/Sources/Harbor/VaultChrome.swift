import AppKit
import HarborKit

/// 原型 Vault 页共享构件：.toolbar / .field / .btn-split / .table 卡片 /
/// .pill / .icon-btn / .switch.green / .hintbox / .empty / .section-title。
/// 浅色数值逐条取自 prototype/styles.css；深色走 Theme 同名 token。

final class VFlippedStack: NSStackView {
    override var isFlipped: Bool { true }
}

final class VPanel: NSStackView {
    override var isFlipped: Bool { true }

    init() {
        super.init(frame: .zero)
        orientation = .vertical
        alignment = .centerX
        spacing = 10
        edgeInsets = NSEdgeInsets(top: 18, left: 18, bottom: 18, right: 18)
        wantsLayer = true
        layer?.cornerRadius = 12
        layer?.hbBg = "card"
    }

    required init?(coder: NSCoder) { nil }

    func append(_ view: NSView) {
        view.translatesAutoresizingMaskIntoConstraints = false
        addArrangedSubview(view)
        view.widthAnchor.constraint(equalTo: widthAnchor, constant: -36).isActive = true
    }
}

final class VPanelColumns: NSStackView {
    override var isFlipped: Bool { true }
    private var stackedWidth: NSLayoutConstraint!
    private var stacked = true

    init(left: NSView, right: NSView) {
        super.init(frame: .zero)
        orientation = .vertical
        alignment = .centerX
        spacing = 14
        for panel in [left, right] {
            panel.translatesAutoresizingMaskIntoConstraints = false
            addArrangedSubview(panel)
            setVisibilityPriority(.mustHold, for: panel)
        }
        // The stack supplies the width on its main axis in horizontal mode.
        stackedWidth = left.widthAnchor.constraint(equalTo: widthAnchor)
        NSLayoutConstraint.activate([
            stackedWidth, left.widthAnchor.constraint(equalTo: right.widthAnchor),
        ])
    }

    required init?(coder: NSCoder) { nil }

    override func layout() {
        let shouldStack = bounds.width < 710
        if shouldStack != stacked {
            stackedWidth.isActive = false
            orientation = shouldStack ? .vertical : .horizontal
            alignment = shouldStack ? .centerX : .top
            stackedWidth.isActive = shouldStack
            stacked = shouldStack
        }
        super.layout()
    }
}

func vPanelTitle(_ title: String, icon: String) -> NSView {
    let label = vSectionTitle(title)
    let image = NSImageView(image: hbIcon(icon, size: 16, tint: Theme.dyn("text2")))
    image.setContentHuggingPriority(.required, for: .horizontal)
    let row = NSStackView(views: [image, label])
    row.alignment = .centerY
    row.spacing = 8
    return row
}

// ---------- .field 搜索框 ----------
final class VField: NSView, NSTextFieldDelegate {
    let entry = NSTextField()
    var onChange: (() -> Void)?
    private var focused = false
    init(placeholder: String, width: CGFloat) {
        super.init(frame: NSRect(x: 0, y: 0, width: width, height: 34))
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.borderWidth = 1
        layer?.hbBg = "tableHead"
        layer?.hbBorderResolver = { [weak self] in
            guard let self else { return nil }
            return (self.focused ? Theme.blue : Theme.line).color.cgColor
        }
        entry.frame = NSRect(x: 12, y: 8, width: width - 24, height: 18)
        entry.autoresizingMask = [.width]
        entry.isBordered = false
        entry.drawsBackground = false
        entry.focusRingType = .none
        entry.lineBreakMode = .byTruncatingTail
        entry.font = .systemFont(ofSize: 13)
        entry.hbText = "text"
        entry.placeholderAttributedString = NSAttributedString(
            string: placeholder, attributes: [
                .font: NSFont.systemFont(ofSize: 13),
                .foregroundColor: Theme.dyn("text3")])
        entry.delegate = self
        addSubview(entry)
    }
    required init?(coder: NSCoder) { fatalError() }
    override func layout() {
        super.layout()
        entry.frame = NSRect(x: 12, y: 8, width: max(0, bounds.width - 24), height: 18)
    }
    var stringValue: String {
        get { entry.stringValue }
        set { entry.stringValue = newValue }
    }
    func controlTextDidBeginEditing(_ obj: Notification) {
        focused = true
        layer?.shadowColor = Theme.blue.color.cgColor
        layer?.shadowOpacity = 0.13
        layer?.shadowRadius = 3
        layer?.shadowOffset = .zero
        layer?.hbBorderResolver = { [weak self] in
            guard let self else { return nil }
            return (self.focused ? Theme.blue : Theme.line).color.cgColor
        }
    }
    func controlTextDidEndEditing(_ obj: Notification) {
        focused = false
        layer?.shadowOpacity = 0
        layer?.hbBorderResolver = { [weak self] in
            guard let self else { return nil }
            return (self.focused ? Theme.blue : Theme.line).color.cgColor
        }
    }
    func controlTextDidChange(_ obj: Notification) { onChange?() }
}

// ---------- .btn-split + 下拉 .menu ----------
struct VMenuEntry {
    let title: String
    var red = false
    var enabled = true
    var sep = false
    var fire: (() -> Void)?
    init(_ title: String, red: Bool = false, enabled: Bool = true,
         fire: (() -> Void)? = nil) {
        self.title = title; self.red = red
        self.enabled = enabled; self.fire = fire
    }
    static var separator: VMenuEntry {
        var e = VMenuEntry(""); e.sep = true; e.enabled = false
        return e
    }
}

enum VMenu {
    private static var panel: NSView?
    private static var monitor: Any?
    static func close() {
        panel?.removeFromSuperview()
        panel = nil
        if let m = monitor { NSEvent.removeMonitor(m); monitor = nil }
    }
    static func show(anchor: NSView, entries: [VMenuEntry]) {
        close()
        guard let cv = anchor.window?.contentView else { return }
        let p = NSView()
        p.wantsLayer = true
        p.layer?.cornerRadius = 10
        p.layer?.hbBg = "card"
        p.layer?.shadowColor = Theme.RGBA(0x141e37).color.cgColor
        p.layer?.shadowOpacity = 0.22
        p.layer?.shadowRadius = 20
        p.layer?.shadowOffset = CGSize(width: 0, height: -6)
        let stack = VFlippedStack()
        stack.orientation = .vertical
        stack.spacing = 0
        stack.edgeInsets = NSEdgeInsets(top: 6, left: 6,
                                        bottom: 6, right: 6)
        var h: CGFloat = 12
        for (i, e) in entries.enumerated() {
            if e.sep {
                let wrap = NSView()
                let ln = NSView()
                ln.wantsLayer = true
                ln.layer?.hbBg = "line"
                ln.translatesAutoresizingMaskIntoConstraints = false
                wrap.addSubview(ln)
                NSLayoutConstraint.activate([
                    ln.leadingAnchor.constraint(
                        equalTo: wrap.leadingAnchor, constant: 8),
                    ln.trailingAnchor.constraint(
                        equalTo: wrap.trailingAnchor, constant: -8),
                    ln.centerYAnchor.constraint(
                        equalTo: wrap.centerYAnchor),
                    ln.heightAnchor.constraint(equalToConstant: 1),
                ])
                wrap.translatesAutoresizingMaskIntoConstraints = false
                stack.addArrangedSubview(wrap)
                wrap.widthAnchor.constraint(
                    equalToConstant: 198).isActive = true
                wrap.heightAnchor.constraint(
                    equalToConstant: 11).isActive = true
                h += 11
                continue
            }
            let it = VMenuItem(e.title, red: e.red, enabled: e.enabled)
            it.fire = {
                VMenu.close()
                e.fire?()
            }
            it.setAccessibilityIdentifier("vmenu-item-\(i)")
            it.translatesAutoresizingMaskIntoConstraints = false
            stack.addArrangedSubview(it)
            it.widthAnchor.constraint(equalToConstant: 198).isActive = true
            it.heightAnchor.constraint(equalToConstant: 30).isActive = true
            h += 30
        }
        p.frame = NSRect(x: 0, y: 0, width: 210, height: h)
        stack.frame = p.bounds
        stack.autoresizingMask = [.width, .height]
        p.addSubview(stack)
        let r = anchor.convert(anchor.bounds, to: cv)
        var x = r.maxX - 210
        x = max(8, min(x, cv.bounds.width - 218))
        let below = cv.isFlipped ? r.maxY : cv.bounds.height - r.minY
        let top = max(8, min(below + 4, cv.bounds.height - h - 8))
        let oy = cv.isFlipped ? top : cv.bounds.height - top - h
        p.frame.origin = NSPoint(x: x, y: oy)
        cv.addSubview(p)
        panel = p
        monitor = NSEvent.addLocalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown, .keyDown]) { ev in
            guard let p = VMenu.panel else { return ev }
            if ev.type == .keyDown {
                if ev.keyCode == 53 { VMenu.close(); return nil }
                return ev
            }
            let pt = p.convert(ev.locationInWindow, from: nil)
            if !p.bounds.contains(pt) { VMenu.close() }
            return ev
        }
    }
}

private final class VMenuItem: NSView {
    var fire: (() -> Void)?
    private let label = NSTextField(labelWithString: "")
    private let red: Bool
    private let enabled_: Bool
    private var hovered = false
    init(_ title: String, red: Bool, enabled: Bool) {
        self.red = red; enabled_ = enabled
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 7
        label.stringValue = title
        label.font = .systemFont(ofSize: 13)
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(
                equalTo: leadingAnchor, constant: 12),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        restyle()
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; restyle()
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; restyle()
    }
    override func mouseDown(with e: NSEvent) {
        if enabled_ { fire?() }
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        restyle()
    }
    private func restyle() {
        label.hbText = enabled_ ? (red ? "red" : "text") : "text3"
        layer?.hbBgResolver = { [weak self] in
            guard let self, self.hovered, self.enabled_ else { return nil }
            return Theme.hover.color.cgColor
        }
    }
}

final class VSplit: NSView {
    let main = HBtn("")
    let dd = HBtn("▾")
    var entries: [VMenuEntry] = []
    init(_ title: String, icon: String? = nil) {
        super.init(frame: .zero)
        main.titleText = title
        main.icon = icon
        main.layer?.maskedCorners = [.layerMinXMinYCorner,
                                     .layerMinXMaxYCorner]
        dd.layer?.maskedCorners = [.layerMaxXMinYCorner,
                                   .layerMaxXMaxYCorner]
        dd.onTap = { [weak self] in
            guard let self else { return }
            VMenu.show(anchor: self.dd, entries: self.entries)
        }
        addSubview(main)
        addSubview(dd)
        let sep = NSView()
        sep.wantsLayer = true
        sep.layer?.hbBgResolver = {
            Theme.dyn("text").withAlphaComponent(0.08).cgColor
        }
        addSubview(sep)
        self.sep = sep
    }
    required init?(coder: NSCoder) { fatalError() }
    private weak var sep: NSView?
    func size() {
        let mw = main.intrinsicContentSize.width + 28
        main.frame = NSRect(x: 0, y: 0, width: mw, height: 33)
        sep?.frame = NSRect(x: mw, y: 0, width: 1, height: 33)
        dd.frame = NSRect(x: mw + 1, y: 0, width: 30, height: 33)
        frame.size = NSSize(width: mw + 31, height: 33)
    }
}

// ---------- .table 卡片 ----------
final class VCard: NSView {
    let box = VFlippedStack()
    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.masksToBounds = true
        layer?.hbBg = "card"
        box.orientation = .vertical
        box.spacing = 0
        box.alignment = .leading
        box.translatesAutoresizingMaskIntoConstraints = false
        addSubview(box)
        NSLayoutConstraint.activate([
            box.topAnchor.constraint(equalTo: topAnchor),
            box.leadingAnchor.constraint(equalTo: leadingAnchor),
            box.trailingAnchor.constraint(equalTo: trailingAnchor),
            box.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }
    func fill(header: NSView, rows: [NSView]) {
        box.arrangedSubviews.forEach {
            box.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }
        header.translatesAutoresizingMaskIntoConstraints = false
        box.addArrangedSubview(header)
        header.widthAnchor.constraint(
            equalTo: box.widthAnchor).isActive = true
        for (i, r) in rows.enumerated() {
            if let vr = r as? VRow { vr.isLast = i == rows.count - 1 }
            r.translatesAutoresizingMaskIntoConstraints = false
            box.addArrangedSubview(r)
            r.widthAnchor.constraint(
                equalTo: box.widthAnchor).isActive = true
        }
    }
}

/// .table th：12px semibold text2，bg tableHead，padding 11x14，底线。
func vHead(_ cols: [(String, CGFloat)]) -> NSView {
    vHeadViews(cols.map { (vLabel($0.0, head: true), $0.1) })
}
func vHeadViews(_ cols: [(NSView, CGFloat)]) -> NSView {
    let row = NSStackView()
    row.orientation = .horizontal
    row.spacing = 28
    row.edgeInsets = NSEdgeInsets(top: 11, left: 14,
                                  bottom: 11, right: 14)
    row.wantsLayer = true
    row.layer?.hbBg = "tableHead"
    for (v, w) in cols {
        v.translatesAutoresizingMaskIntoConstraints = false
        if w > 0 {
            v.widthAnchor.constraint(equalToConstant: w).isActive = true
        } else {
            v.widthAnchor.constraint(
                greaterThanOrEqualToConstant: 110).isActive = true
            v.setContentHuggingPriority(.init(1), for: .horizontal)
        }
        row.addArrangedSubview(v)
    }
    let line = NSView()
    line.wantsLayer = true
    line.layer?.hbBg = "line"
    line.translatesAutoresizingMaskIntoConstraints = false
    row.addSubview(line)
    NSLayoutConstraint.activate([
        line.leadingAnchor.constraint(equalTo: row.leadingAnchor),
        line.trailingAnchor.constraint(equalTo: row.trailingAnchor),
        line.bottomAnchor.constraint(equalTo: row.bottomAnchor),
        line.heightAnchor.constraint(equalToConstant: 1),
        row.heightAnchor.constraint(equalToConstant: 37),
    ])
    return row
}

/// .table td 行：padding 11x14，13px，底分隔线（末行无）。
class VRow: NSStackView {
    var isLast = false { didSet { needsDisplay = true } }
    override init(frame f: NSRect) {
        super.init(frame: f)
        orientation = .horizontal
        spacing = 28
        edgeInsets = NSEdgeInsets(top: 11, left: 14,
                                  bottom: 11, right: 14)
        heightAnchor.constraint(
            greaterThanOrEqualToConstant: 40).isActive = true
    }
    required init?(coder: NSCoder) { fatalError() }
    override func draw(_ dirty: NSRect) {
        super.draw(dirty)
        guard !isLast else { return }
        Theme.rowLine.color.setFill()
        NSRect(x: 0, y: bounds.height - 1,
               width: bounds.width, height: 1).fill()
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
}

func vEmptyRow(_ text: String) -> NSView {
    let r = VRow()
    let l = vLabel(text, dim: true)
    l.font = .systemFont(ofSize: 12.5)
    l.alignment = .center
    l.translatesAutoresizingMaskIntoConstraints = false
    r.addArrangedSubview(l)
    l.widthAnchor.constraint(
        equalTo: r.widthAnchor, constant: -28).isActive = true
    r.heightAnchor.constraint(equalToConstant: 65).isActive = true
    r.isLast = true
    return r
}

// ---------- 标签 / pill / icon-btn / switch ----------
func vLabel(_ s: String, mono: Bool = false, bold: Bool = false,
            dim: Bool = false, head: Bool = false) -> NSTextField {
    let l = NSTextField(labelWithString: s)
    if head {
        l.font = .systemFont(ofSize: 12, weight: .semibold)
        l.hbText = "text2"
    } else if mono {
        l.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        l.hbText = dim ? "text2" : "text"
    } else {
        l.font = .systemFont(ofSize: 13,
                             weight: bold ? .semibold : .regular)
        l.hbText = dim ? "text3" : "text"
    }
    l.lineBreakMode = .byTruncatingTail
    return l
}

/// .pill：dot + 11.5 semibold，radius 20，padding 3x9。
final class VPill: NSView {
    enum Kind { case on, off, err, run, wait }
    private let dot = NSView()
    private let label = NSTextField(labelWithString: "")
    private let hasDot: Bool
    init(_ text: String, kind: Kind, dot hasDot: Bool = true) {
        self.hasDot = hasDot
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 11
        let bg: String, fg: String
        switch kind {
        case .on: bg = "greenPillBg"; fg = "greenPillFg"
        case .off: bg = "rowLine"; fg = "text3"
        case .err: bg = "pillErrBg"; fg = "pillErrFg"
        case .run: bg = "pillRunBg"; fg = "pillRunFg"
        case .wait: bg = "pillWaitBg"; fg = "pillWaitFg"
        }
        layer?.hbBg = bg
        label.stringValue = text
        label.font = .systemFont(ofSize: 11.5, weight: .semibold)
        label.hbText = fg
        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.spacing = 5
        stack.edgeInsets = NSEdgeInsets(top: 3, left: 9,
                                        bottom: 3, right: 9)
        stack.translatesAutoresizingMaskIntoConstraints = false
        if hasDot {
            dot.wantsLayer = true
            dot.layer?.cornerRadius = 3
            dot.layer?.hbBgResolver = { [weak self] in
                self?.label.textColor?.cgColor
            }
            dot.translatesAutoresizingMaskIntoConstraints = false
            dot.widthAnchor.constraint(equalToConstant: 6).isActive = true
            dot.heightAnchor.constraint(equalToConstant: 6).isActive = true
            stack.addArrangedSubview(dot)
        }
        stack.addArrangedSubview(label)
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }
    override var intrinsicContentSize: NSSize {
        let size = label.intrinsicContentSize
        return NSSize(width: ceil(size.width) + (hasDot ? 29 : 18),
                      height: ceil(size.height) + 6)
    }
    func setText(_ t: String, kind: Kind) {
        label.stringValue = t
        invalidateIntrinsicContentSize()
        let bg: String, fg: String
        switch kind {
        case .on: bg = "greenPillBg"; fg = "greenPillFg"
        case .off: bg = "rowLine"; fg = "text3"
        case .err: bg = "pillErrBg"; fg = "pillErrFg"
        case .run: bg = "pillRunBg"; fg = "pillRunFg"
        case .wait: bg = "pillWaitBg"; fg = "pillWaitFg"
        }
        layer?.hbBg = bg
        label.hbText = fg
    }
}

/// .icon-btn：26x26 radius 6；del hover 红底红字；bm on 琥珀。
final class VIconBtn: NSButton {
    enum Kind { case plain, del, bm }
    private let kind: Kind
    private let icon: String
    var bmOn = false { didSet { restyle() } }
    private var hovered = false
    init(icon: String, kind: Kind = .plain) {
        self.kind = kind; self.icon = icon
        super.init(frame: NSRect(x: 0, y: 0, width: 26, height: 26))
        isBordered = false
        wantsLayer = true
        layer?.cornerRadius = 6
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        restyle()
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; restyle()
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; restyle()
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        restyle()
    }
    private func restyle() {
        let tint: NSColor
        switch kind {
        case .plain: tint = Theme.dyn("text2")
        case .del: tint = hovered ? Theme.dyn("red") : Theme.dyn("text2")
        case .bm: tint = bmOn ? Theme.dyn("amber") : Theme.dyn("text3")
        }
        image = hbIcon(icon, size: 15, tint: tint)
        imagePosition = .imageOnly
        layer?.hbBgResolver = { [weak self] in
            guard let self, self.hovered else { return nil }
            switch self.kind {
            case .del: return Theme.dangerBg.color.cgColor
            default: return Theme.btn.color.cgColor
            }
        }
    }
}

/// .switch.green：38x22，轨道 pending→green，白色圆钮滑动。
final class VSwitch: NSControl {
    var isOn = false {
        didSet {
            guard isOn != oldValue else { return }
            place(animated: true)
            sendAction(action, to: target)
        }
    }
    private let track = CALayer()
    private let knob = CALayer()
    override init(frame f: NSRect) {
        super.init(frame: NSRect(x: f.origin.x, y: f.origin.y,
                                 width: 38, height: 22))
        wantsLayer = true
        track.cornerRadius = 11
        knob.cornerRadius = 9
        knob.backgroundColor = NSColor.white.cgColor
        knob.shadowColor = NSColor.black.cgColor
        knob.shadowOpacity = 0.25
        knob.shadowRadius = 1.5
        knob.shadowOffset = CGSize(width: 0, height: -1)
        layer?.addSublayer(track)
        layer?.addSublayer(knob)
        track.frame = bounds
        place(animated: false)
    }
    required init?(coder: NSCoder) { fatalError() }
    override func mouseDown(with e: NSEvent) { isOn.toggle() }
    override func accessibilityPerformPress() -> Bool {
        isOn.toggle()
        return true
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        place(animated: false)
    }
    private func place(animated: Bool) {
        track.backgroundColor =
            (isOn ? Theme.green : Theme.pending).color.cgColor
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = animated ? 0.18 : 0
            ctx.allowsImplicitAnimation = true
            knob.frame = CGRect(x: isOn ? 18 : 2, y: 2,
                                width: 18, height: 18)
        }
    }
}

// ---------- hintbox / empty / section-title ----------
func vHintBox(_ text: String) -> NSView {
    let v = NSView()
    v.wantsLayer = true
    v.layer?.cornerRadius = 9
    v.layer?.borderWidth = 1
    v.layer?.hbBg = "hintBlueBg"
    v.layer?.hbBorder = "hintBlueLine"
    let l = NSTextField(wrappingLabelWithString: text)
    l.font = .systemFont(ofSize: 12)
    l.hbText = "hintBlueFg"
    l.translatesAutoresizingMaskIntoConstraints = false
    l.preferredMaxLayoutWidth = 600
    v.addSubview(l)
    NSLayoutConstraint.activate([
        l.topAnchor.constraint(equalTo: v.topAnchor, constant: 10),
        l.leadingAnchor.constraint(equalTo: v.leadingAnchor,
                                   constant: 13),
        l.trailingAnchor.constraint(equalTo: v.trailingAnchor,
                                    constant: -13),
        l.bottomAnchor.constraint(equalTo: v.bottomAnchor,
                                  constant: -10),
    ])
    return v
}

private final class VGradTile: NSView {
    let g = CAGradientLayer()
    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.cornerRadius = 14
        g.frame = bounds
        g.startPoint = CGPoint(x: 0.5, y: 0)
        g.endPoint = CGPoint(x: 0.5, y: 1)
        layer?.addSublayer(g)
        recolor()
    }
    required init?(coder: NSCoder) { fatalError() }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        recolor()
    }
    private func recolor() {
        g.colors = [Theme.scroll1.color.cgColor,
                    Theme.scroll2.color.cgColor]
    }
}

func vEmptyState(icon: String, title: String,
                 sub: String? = nil) -> NSView {
    let stack = VFlippedStack()
    stack.orientation = .vertical
    stack.alignment = .centerX
    stack.spacing = 8
    let tile = VGradTile(frame: NSRect(x: 0, y: 0,
                                       width: 58, height: 58))
    let iv = NSImageView(frame: NSRect(x: 18, y: 18, width: 22, height: 22))
    iv.image = hbIcon(icon, size: 22, tint: Theme.dyn("text2"))
    tile.addSubview(iv)
    tile.translatesAutoresizingMaskIntoConstraints = false
    tile.widthAnchor.constraint(equalToConstant: 58).isActive = true
    tile.heightAnchor.constraint(equalToConstant: 58).isActive = true
    stack.addArrangedSubview(tile)
    let t = vLabel(title, bold: true)
    t.font = .systemFont(ofSize: 15, weight: .bold)
    stack.addArrangedSubview(t)
    if let sub {
        let s = NSTextField(wrappingLabelWithString: sub)
        s.font = .systemFont(ofSize: 12.5)
        s.hbText = "text3"
        s.alignment = .center
        s.preferredMaxLayoutWidth = 380
        s.translatesAutoresizingMaskIntoConstraints = false
        s.widthAnchor.constraint(
            lessThanOrEqualToConstant: 380).isActive = true
        stack.addArrangedSubview(s)
    }
    return stack
}

func vSectionTitle(_ t: String) -> NSTextField {
    let l = vLabel(t, bold: true)
    l.font = .systemFont(ofSize: 13, weight: .bold)
    return l
}

/// .table 行：底细线 rowLine，无系统选中高亮（原型非 rowlink 无 hover）。
final class VTableRowView: NSTableRowView {
    override func drawSeparator(in dirtyRect: NSRect) {
        Theme.rowLine.color.setFill()
        NSRect(x: 0, y: bounds.maxY - 1, width: bounds.width,
               height: 1).fill()
    }
    override func drawSelection(in dirtyRect: NSRect) {}
}

/// 圆角白卡片包 NSTableView（共享 TableHeaderView），高度随行数；
/// 0 行时显示居中空态文案。外层再套 scroll 以便与 hintbox 堆叠。
final class VTableCard: NSView {
    let table = NSTableView()
    let scroll = NSScrollView()
    var sizeColumns: ((CGFloat) -> Void)?
    private var hc: NSLayoutConstraint!
    private let emptyL = NSTextField(labelWithString: "")
    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.masksToBounds = true
        layer?.hbBg = "card"
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = false
        scroll.hasHorizontalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.wantsLayer = true
        scroll.layer?.hbBg = "card"
        scroll.translatesAutoresizingMaskIntoConstraints = false
        addSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        table.backgroundColor = Theme.dyn("card")
        table.headerView = TableHeaderView(
            frame: NSRect(x: 0, y: 0, width: 600, height: 37))
        table.selectionHighlightStyle = .none
        table.columnAutoresizingStyle = .noColumnAutoresizing
        table.intercellSpacing = .zero
        table.rowHeight = 40
        table.autoresizingMask = [.width, .height]
        scroll.documentView = table
        emptyL.font = .systemFont(ofSize: 12.5)
        emptyL.hbText = "text3"
        emptyL.alignment = .center
        emptyL.translatesAutoresizingMaskIntoConstraints = false
        addSubview(emptyL)
        NSLayoutConstraint.activate([
            emptyL.centerXAnchor.constraint(equalTo: centerXAnchor),
            emptyL.topAnchor.constraint(equalTo: topAnchor,
                                        constant: 48),
        ])
        hc = heightAnchor.constraint(equalToConstant: 102)
        hc.isActive = true
    }
    required init?(coder: NSCoder) { fatalError() }
    override func layout() {
        super.layout()
        sizeColumns?(scroll.contentSize.width)
        table.tile()
    }
    func setRows(_ n: Int, emptyText: String) {
        hc.constant = 37 + (n == 0 ? 65 : CGFloat(n) * table.rowHeight)
        emptyL.stringValue = emptyText
        emptyL.isHidden = n != 0
    }
}
