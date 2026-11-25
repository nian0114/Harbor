import AppKit
import CHarbor
import HarborKit

// ---------- 标签条（自绘，对齐 prototype .tabstrip/.tab）----------
// 内容定宽 tab：icon 13 + 标题 13px + 瞬时点 + ×；h34 圆角 8 间距 6；
// 会话 tab 激活 = 绿（term.active），Vault/SFTP = 灰蓝；hover 白 6%。
final class TabStrip: NSView {
    var tabs: [TermTab] = [] { didSet { needsDisplay = true } }
    var selected: Int = 0 { didSet { needsDisplay = true } }
    var onSelect: ((Int) -> Void)?
    var onClose: ((Int) -> Void)?
    var onNew: (() -> Void)?
    var onReorder: ((Int, Int) -> Void)?
    private var dragIdx: Int?
    private var hoverIdx: Int?
    private var hoverClose = false
    private var hoverPlus = false
    private var tracking: NSTrackingArea?

    override var isFlipped: Bool { true }

    private static let tabH: CGFloat = 34
    private static let gap: CGFloat = 6
    private static let padX: CGFloat = 12
    private static let innerGap: CGFloat = 7
    private static let minW: CGFloat = 84
    private static let titleFont = NSFont.systemFont(ofSize: 13)

    /// 会话页（term/workspace/connect）激活走绿色；pinned/共享观看走灰蓝。
    private func termStyle(_ t: TermTab) -> Bool {
        t.closable && !(t is ShareTab)
    }
    /// tab 图标：会话页 iconName 缺省回退叶图标（terminal/os tile）。
    private func icon(_ t: TermTab) -> String {
        t.iconName ?? t.wsIconName
    }
    /// 瞬时点：共享蓝点优先，其次 watch 角标（4s 自动清）。
    private func dot(_ t: TermTab) -> Theme.RGBA? {
        if t.shareOn { return Theme.dotShare }
        guard let w = t as? WsTab, let b = w.badgeGlyph else { return nil }
        switch b.0 {
        case "×": return Theme.dotErr
        case "✓": return Theme.dotOk
        default:  return Theme.dotPend
        }
    }
    private func titleW(_ t: TermTab) -> CGFloat {
        (t.title as NSString).size(withAttributes:
            [.font: TabStrip.titleFont]).width
    }
    private func idealW(_ t: TermTab) -> CGFloat {
        var w = TabStrip.padX * 2 + 13 + TabStrip.innerGap + titleW(t)
        if dot(t) != nil { w += 8 + TabStrip.innerGap }
        if t.closable { w += 15 + TabStrip.innerGap }
        return ceil(w)
    }
    /// 布局：内容定宽；超宽时按比例压缩会话 tab（不低于 minW）。
    private func tabRects() -> [NSRect] {
        var ws = tabs.map { idealW($0) }
        let plusW: CGFloat = 31
        let avail = bounds.width - plusW - TabStrip.gap
        let total = ws.reduce(0, +) +
            TabStrip.gap * CGFloat(max(tabs.count - 1, 0))
        if total > avail {
            let pinnedW = tabs.enumerated().reduce(CGFloat(0)) {
                $0 + ($1.element.closable ? 0 : ws[$1.offset]) }
            let flexIdx = tabs.indices.filter { tabs[$0].closable }
            let flexW = flexIdx.reduce(CGFloat(0)) { $0 + ws[$1] }
            let target = avail - pinnedW -
                TabStrip.gap * CGFloat(max(tabs.count - 1, 0))
            if flexW > 0, target > 0 {
                let k = max(target / flexW, 0)
                for i in flexIdx {
                    ws[i] = max(TabStrip.minW, ws[i] * k)
                }
            }
        }
        var rects: [NSRect] = []
        var x: CGFloat = 0
        let y = (bounds.height - TabStrip.tabH) / 2
        for w in ws {
            rects.append(NSRect(x: x, y: y, width: w,
                                height: TabStrip.tabH))
            x += w + TabStrip.gap
        }
        return rects
    }
    private func plusRect() -> NSRect {
        let r = tabRects()
        let x = (r.last?.maxX ?? -TabStrip.gap) + TabStrip.gap
        return NSRect(x: x, y: bounds.height / 2 - 13,
                      width: 31, height: 27)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = tracking { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited,
                      .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil)
        addTrackingArea(t)
        tracking = t
    }
    private func refreshHover(_ p: NSPoint) {
        var idx: Int?
        var close = false
        for (i, rc) in tabRects().enumerated() where rc.contains(p) {
            idx = i
            close = tabs[i].closable && p.x > rc.maxX - 24
        }
        let plus = plusRect().contains(p)
        if idx != hoverIdx || close != hoverClose || plus != hoverPlus {
            hoverIdx = idx; hoverClose = close; hoverPlus = plus
            needsDisplay = true
        }
    }
    override func mouseMoved(with e: NSEvent) {
        refreshHover(convert(e.locationInWindow, from: nil))
    }
    override func mouseExited(with e: NSEvent) {
        hoverIdx = nil; hoverClose = false; hoverPlus = false
        needsDisplay = true
    }

    override func draw(_ r: NSRect) {
        Theme.titlebar.color.setFill(); NSBezierPath.fill(bounds)
        let rects = tabRects()
        for (i, t) in tabs.enumerated() {
            let rc = rects[i]
            let sel = i == selected
            let term = termStyle(t)
            // 底 + 边框
            let bg: NSColor
            let border: NSColor
            if sel && term {
                bg = Theme.tabTermBg.color
                border = Theme.tabTermLine.color
            } else if sel {
                bg = Theme.titlebarTab.color
                border = NSColor.white.withAlphaComponent(0.08)
            } else if i == hoverIdx {
                bg = NSColor.white.withAlphaComponent(0.06)
                border = .clear
            } else {
                bg = .clear; border = .clear
            }
            let path = NSBezierPath(roundedRect: rc.insetBy(dx: 0.5, dy: 0.5),
                                    xRadius: 8, yRadius: 8)
            bg.setFill(); path.fill()
            if border != .clear {
                border.setStroke(); path.lineWidth = 1; path.stroke()
            }
            let fg = (sel && term) ? Theme.tabTermFg.color
                                   : Theme.titlebarText.color
            var x = rc.minX + TabStrip.padX
            // 图标 13px
            let img = hbIcon(icon(t), size: 13,
                             tint: fg.withAlphaComponent(0.9))
            img.draw(in: NSRect(x: x, y: rc.midY - 6.5,
                                width: 13, height: 13))
            x += 13 + TabStrip.innerGap
            // 关闭 ×（右侧固定位）
            let closeW: CGFloat = t.closable ? 15 + TabStrip.innerGap : 0
            let dotW: CGFloat = dot(t) != nil ? 8 + TabStrip.innerGap : 0
            // 标题（截断）
            let tw = max(0, rc.width - TabStrip.padX * 2 - 13 -
                         TabStrip.innerGap - closeW - dotW)
            let para = NSMutableParagraphStyle()
            para.lineBreakMode = .byTruncatingTail
            (t.title as NSString).draw(
                in: NSRect(x: x, y: rc.midY - 8, width: tw, height: 17),
                withAttributes: [.font: TabStrip.titleFont,
                                 .foregroundColor: fg,
                                 .paragraphStyle: para])
            x += tw + TabStrip.innerGap
            // 瞬时点（共享蓝 / watch 绿红琥珀）
            if let d = dot(t) {
                let dr = NSRect(x: x, y: rc.midY - 4, width: 8, height: 8)
                let sh = NSShadow()
                sh.shadowColor = d.color
                sh.shadowBlurRadius = 6
                sh.shadowOffset = .zero
                NSGraphicsContext.saveGraphicsState()
                sh.set()
                d.color.setFill()
                NSBezierPath(ovalIn: dr).fill()
                NSGraphicsContext.restoreGraphicsState()
                x += 8 + TabStrip.innerGap
            }
            // 关闭 ×
            if t.closable {
                let cr = NSRect(x: rc.maxX - TabStrip.padX - 15,
                                y: rc.midY - 8, width: 15, height: 16)
                if i == hoverIdx && hoverClose {
                    NSColor.white.withAlphaComponent(0.12).setFill()
                    NSBezierPath(roundedRect: cr.insetBy(dx: -2, dy: -1),
                                 xRadius: 4, yRadius: 4).fill()
                }
                ("×" as NSString).draw(
                    in: cr, withAttributes: [
                        .font: NSFont.systemFont(ofSize: 12),
                        .foregroundColor: fg.withAlphaComponent(
                            i == hoverIdx && hoverClose ? 1 : 0.5)])
            }
        }
        // ＋ 新建
        let pr = plusRect()
        if hoverPlus {
            NSColor.white.withAlphaComponent(0.08).setFill()
            NSBezierPath(roundedRect: pr, xRadius: 7, yRadius: 7).fill()
        }
        let cp = NSMutableParagraphStyle(); cp.alignment = .center
        ("+" as NSString).draw(
            in: pr, withAttributes: [
                .font: NSFont.systemFont(ofSize: 19),
                .foregroundColor: Theme.titlebarText.color,
                .paragraphStyle: cp])
    }
    override func mouseDown(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        if plusRect().contains(p) { onNew?(); return }
        for (i, rc) in tabRects().enumerated() where rc.contains(p) {
            let t = tabs[i]
            if t.closable && p.x > rc.maxX - 24 { onClose?(i); return }
            onSelect?(i)
            if t.closable { dragIdx = i }   // pinned 页不参与拖动排序
            return
        }
    }
    override func mouseDragged(with e: NSEvent) {
        guard let from = dragIdx else { return }
        let p = convert(e.locationInWindow, from: nil)
        for (i, rc) in tabRects().enumerated()
            where i != from && tabs[i].closable && rc.contains(p) {
            onReorder?(from, i); dragIdx = i; return
        }
    }
    override func mouseUp(with e: NSEvent) { dragIdx = nil }
}