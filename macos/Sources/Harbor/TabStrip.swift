import AppKit
import CHarbor
import HarborKit

// ---------- 标签条（自绘）----------
final class TabStrip: NSView {
    var tabs: [TermTab] = [] { didSet { needsDisplay = true } }
    var selected: Int = 0 { didSet { needsDisplay = true } }
    var onSelect: ((Int) -> Void)?
    var onClose: ((Int) -> Void)?
    var onNew: (() -> Void)?
    var onReorder: ((Int, Int) -> Void)?
    private var dragIdx: Int?

    override var isFlipped: Bool { true }

    private func tabRect(_ i: Int) -> NSRect {
        let w = min(160, max(80, (bounds.width - 40) / CGFloat(max(tabs.count,1))))
        return NSRect(x: CGFloat(i)*w + 4, y: 4, width: w - 2,
                      height: bounds.height - 8)
    }
    private func plusRect() -> NSRect {
        let x = tabRect(max(tabs.count-1,0)).maxX + 6
        return NSRect(x: x, y: bounds.height/2 - 10, width: 22, height: 20)
    }
    override func draw(_ r: NSRect) {
        Theme.titlebar.color.setFill(); NSBezierPath.fill(bounds)
        for (i, t) in tabs.enumerated() {
            let rc = tabRect(i)
            let sel = i == selected
            (sel ? Theme.titlebarTab : Theme.titlebar).color.setFill()
            NSBezierPath(roundedRect: rc, xRadius: 6, yRadius: 6).fill()
            // dot
            t.statusDot.color.setFill()
            NSBezierPath(ovalIn: NSRect(x: rc.minX+8, y: rc.midY-3,
                                        width: 6, height: 6)).fill()
            // 标题
            let para = NSMutableParagraphStyle()
            para.lineBreakMode = .byTruncatingTail
            (t.title as NSString).draw(
                in: NSRect(x: rc.minX+20, y: rc.minY+5,
                           width: rc.width-40, height: rc.height-8),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 11.5),
                    .foregroundColor: NSColor.white.withAlphaComponent(
                        sel ? 1 : 0.65),
                    .paragraphStyle: para])
            // 关闭 ×
            ("×" as NSString).draw(at: NSPoint(x: rc.maxX-16, y: rc.minY+4),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 13),
                    .foregroundColor: NSColor.white.withAlphaComponent(0.55)])
        }
        ("+" as NSString).draw(in: plusRect(), withAttributes: [
            .font: NSFont.systemFont(ofSize: 15),
            .foregroundColor: NSColor.white.withAlphaComponent(0.8),
            .paragraphStyle: { let p = NSMutableParagraphStyle();
                               p.alignment = .center; return p }()])
    }
    override func mouseDown(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        if plusRect().contains(p) { onNew?(); return }
        for (i,_) in tabs.enumerated() {
            let rc = tabRect(i)
            if rc.contains(p) {
                if p.x > rc.maxX - 22 { onClose?(i); return }
                onSelect?(i); dragIdx = i; return
            }
        }
    }
    override func mouseDragged(with e: NSEvent) {
        guard let from = dragIdx else { return }
        let p = convert(e.locationInWindow, from: nil)
        for (i,_) in tabs.enumerated() where i != from {
            if tabRect(i).contains(p) { onReorder?(from, i); dragIdx = i; return }
        }
    }
    override func mouseUp(with e: NSEvent) { dragIdx = nil }
}
