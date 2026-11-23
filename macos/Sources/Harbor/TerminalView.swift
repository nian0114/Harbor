import AppKit
import CHarbor
import HarborKit

/// hb_cell 的 Swift 快照（绘制时不再持 vt 锁）。
struct VCell {
    var chars: (UInt32, UInt32, UInt32, UInt32, UInt32, UInt32)
    var width: UInt8
    var attrs: UInt16
    var fg: Theme.RGBA
    var bg: Theme.RGBA
    var hashable: [UInt32] {
        func q(_ c: Theme.RGBA) -> UInt32 {
            (UInt32(c.r*255) << 24) | (UInt32(c.g*255) << 16)
                | (UInt32(c.b*255) << 8) | UInt32(c.a*255)
        }
        return [chars.0, chars.1, chars.2, chars.3, chars.4, chars.5,
                UInt32(width), UInt32(attrs), q(fg), q(bg)]
    }
}

/// 一行的已布局内容：背景 run 矩形 + 文本 run（CTLine）。
struct RowLayout {
    struct Run {
        var x: CGFloat
        var line: CTLine
        var text: NSAttributedString
        var underline: Int   // 0 none 1 single 2 double 3 curly
        var strike: Bool
        var color: Theme.RGBA
        var bgRect: CGRect
        var hasBG: Bool
        var bgColor: Theme.RGBA
    }
    var runs: [Run]
    var hash: Int
}

final class TerminalView: NSView, NSTextInputClient {
    var fontName = "Menlo" { didSet { rebuildFont() } }
    var fontSize: CGFloat = 14 { didSet { rebuildFont(); fitNotify() } }
    var lineHeight: CGFloat = 1.15 { didSet { rebuildFont(); fitNotify() } }
    var cursorStyle = 1 { didSet { needsDisplay = true } }
    var theme = TermTheme.for("harbor") { didSet { invalidateLayout(); needsDisplay = true } }
    var padX: CGFloat = 12, padY: CGFloat = 10
    var optionIsMeta = false
    var session: Session?
    var onSizeChange: ((Int, Int) -> Void)?
    /// 广播输入镜像（Workspace）：用户输入在发给自己 session 之外，
    /// 经此回调转发给其它 pane（对齐 hb_terminal_set_input_mirror）。
    var inputMirror: ((MirrorEv) -> Void)?
    /// 点击终端 → 工作区聚焦本 pane。
    var onFocusRequest: (() -> Void)?

    private var geom = CellGeom(cellW: 8, cellH: 16, padX: 12, padY: 10,
                                baseline: 13)
    private var font: NSFont = .monospacedSystemFont(ofSize: 14, weight: .regular)
    private var boldFont: NSFont?
    private var italicFont: NSFont?

    private(set) var scrollOff = 0
    private var scrollPixels: CGFloat = 0

    struct SelPt: Equatable { var row: Int; var col: Int }
    var selAnchor: SelPt?
    var selCaret: SelPt?
    var blockSel = false
    private var hasSel: Bool { selAnchor != nil && selAnchor != selCaret }

    private var markedText = NSMutableAttributedString()
    private var markedRng = NSRange(location: NSNotFound, length: 0)

    // 快照：主线程只在短临界区内复制 vt 格子。
    private var grid: [[VCell]] = []
    private var gridRows = 0, gridCols = 0
    private var curRow: Int32 = 0, curCol: Int32 = 0
    private var curVis = false, curBlink = false, curShape: Int32 = 1
    private var sbCount = 0
    private var rowLayouts: [Int: RowLayout] = [:]   // visRow → layout（含 hash）
    private var selHash = 0
    private var layoutGen = 0

    private var dirty = true
    private var redrawTimer: Timer?
    private var blinkTimer: Timer?
    private var cursorOn = true
    private var flashUntil = Date.distantPast

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    init() {
        super.init(frame: .zero)
        rebuildFont()
        blinkTimer = Timer.scheduledTimer(withTimeInterval: 0.53,
                                        repeats: true) { [weak self] _ in
            self?.cursorOn.toggle()
            self?.dirty = true
            self?.needsDisplay = true
        }
    }
    required init?(coder: NSCoder) { nil }
    deinit { blinkTimer?.invalidate(); redrawTimer?.invalidate() }

    private func rebuildFont() {
        font = NSFont(name: fontName, size: fontSize)
            ?? .monospacedSystemFont(ofSize: fontSize, weight: .regular)
        let fd = font.fontDescriptor
        boldFont = NSFont(descriptor: fd.withSymbolicTraits(.bold),
                          size: fontSize) ?? font
        italicFont = NSFont(descriptor: fd.withSymbolicTraits(.italic),
                            size: fontSize) ?? font
        var w = font.advancement(forGlyph: font.glyph(withName: "M")).width
        if w <= 0 {
            w = ("M" as NSString).size(withAttributes: [.font: font]).width
        }
        let ch = ceil((font.ascender - font.descender + font.leading)
                      * lineHeight)
        geom = CellGeom(cellW: max(1, w), cellH: ch,
                        padX: padX, padY: padY,
                        baseline: font.ascender
                            + (ch - (font.ascender - font.descender)) / 2)
        invalidateLayout()
        needsDisplay = true
    }
    private func invalidateLayout() {
        rowLayouts.removeAll(); layoutGen &+= 1
    }

    func rows() -> Int { geom.rows(in: bounds.height) }
    func cols() -> Int { geom.cols(in: bounds.width) }

    #if DEBUG
    func gridInfo() -> String {
        snapshot()
        var out = "rows=\(gridRows) cols=\(gridCols) scrollOff=\(scrollOff) sb=\(sbCount) grid=\(grid.count) cur=(\(curRow),\(curCol))\n"
        for (i, line) in grid.enumerated() {
            let n = line.filter { $0.chars.0 != 0 && $0.chars.0 != 32 }.count
            let txt = line.prefix(20).map { c -> Character in
                c.chars.0 == 0 ? " " : (UnicodeScalar(c.chars.0).map(Character.init) ?? "?") }
            out += "row \(i): nonblank=\(n) '\(String(txt))'\n"
        }
        out += "--- layouts ---\n"
        for (i, line) in grid.enumerated() {
            let lay = layoutRow(line, visRow: i, hash: 0)
            let descr = lay.runs.map {
                "x=\(Int($0.x)) cells=\(Int($0.bgRect.width/geom.cellW)) bg=\($0.hasBG ? 1:0)"
            }.joined(separator: "; ")
            out += "row \(i): runs=\(lay.runs.count) baseY=\(geom.baselineY(row: i)) [\(descr)]\n"
        }
        return out
    }
    #endif

    // ---- 事件 → 合流重绘 ----
    func poke() {
        dirty = true
        if redrawTimer == nil {
            redrawTimer = Timer.scheduledTimer(
                withTimeInterval: 1.0/60.0, repeats: true) { [weak self] _ in
                guard let self, self.dirty else { self?.stopRedraw(); return }
                self.dirty = false
                self.needsDisplay = true
            }
        }
    }
    private func stopRedraw() {
        redrawTimer?.invalidate(); redrawTimer = nil
    }
    func bell() {
        flashUntil = Date().addingTimeInterval(0.18); poke()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        invalidateLayout(); fitNotify()
    }
    private var lastGrid = (0, 0)
    private func fitNotify() {
        let g = (rows(), cols())
        if g != lastGrid { lastGrid = g; onSizeChange?(g.0, g.1) }
    }

    // ---- vt 快照（锁内只做 memcpy）----
    private func snapshot() {
        guard let vt = session?.vt else { return }
        let t0 = ProcessInfo.processInfo.systemUptime
        hb_vt_lock(vt)
        var vr: Int32 = 0, vc: Int32 = 0
        hb_vt_size(vt, &vr, &vc)
        sbCount = Int(hb_vt_scrollback_count(vt))
        var v = false, b = false
        hb_vt_cursor(vt, &curRow, &curCol, &v, &b, &curShape)
        curVis = v; curBlink = b
        gridRows = Int(vr); gridCols = Int(vc)
        let nRows = gridRows + sbCount   // row -sb..rows-1
        if grid.count != nRows { grid = [[VCell]]() }
        grid.reserveCapacity(nRows)
        // 只拷可见窗口：scrollOff..scrollOff+viewRows
        let visRows = min(gridRows, rows())
        let first = -(scrollOff)
        grid.removeAll(keepingCapacity: true)
        for r in first..<(first + visRows) {
            var line = [VCell](repeating: VCell(
                chars: (0,0,0,0,0,0), width: 1, attrs: 0,
                fg: theme.fg, bg: theme.bg), count: gridCols)
            for c in 0..<gridCols {
                var cell = hb_cell()
                guard hb_vt_get_cell(vt, Int32(r), Int32(c), &cell)
                else { break }
                let fg = theme.color(cell.fg.type, idx: cell.fg.idx,
                                     r: cell.fg.r, g: cell.fg.g, b: cell.fg.b,
                                     def: theme.fg)
                let bg = theme.color(cell.bg.type, idx: cell.bg.idx,
                                     r: cell.bg.r, g: cell.bg.g, b: cell.bg.b,
                                     def: theme.bg)
                line[c] = VCell(chars: cell.chars, width: cell.width,
                                attrs: cell.attrs, fg: fg, bg: bg)
            }
            grid.append(line)
        }
        hb_vt_unlock(vt)
        _ = t0
    }

    // ---- 行布局：按内容 hash 缓存 ----
    private func hashRow(_ line: [VCell]) -> Int {
        var h = 1469598103934665603
        for cell in line {
            for v in cell.hashable {
                h = (h ^ Int(bitPattern: UInt(v))) &* 1099511628211
            }
        }
        return h
    }
    private func cellText(_ cell: VCell) -> String {
        if cell.chars.0 == 0 { return " " }
        var s = ""
        for cp in [cell.chars.0, cell.chars.1, cell.chars.2,
                   cell.chars.3, cell.chars.4, cell.chars.5] {
            if cp == 0 { break }
            s.unicodeScalars.append(UnicodeScalar(cp) ?? "\u{FFFD}")
        }
        return s
    }
    private func layoutRow(_ line: [VCell], visRow: Int, hash: Int) -> RowLayout {
        var runs: [RowLayout.Run] = []
        var col = 0
        var runText = ""
        var runStart = 0
        var runKey: (UInt16, Theme.RGBA, Theme.RGBA) = (0, .init(0), .init(0))
        var inRun = false
        func flush() {
            guard inRun else { return }
            let x = geom.padX + CGFloat(runStart) * geom.cellW
            let wCells = col - runStart
            let bgc = runKey.2
            let hasBG = bgc != theme.bg
            let fg = runKey.1
            let a = runKey.0
            var f = font
            if a & UInt16(HB_ATTR_BOLD) != 0 { f = boldFont ?? font }
            if a & UInt16(HB_ATTR_ITALIC) != 0 { f = italicFont ?? f }
            var ul = 0
            if a & UInt16(HB_ATTR_DUNDERLINE) != 0 { ul = 2 }
            else if a & UInt16(HB_ATTR_UNDERLINE) != 0 { ul = 1 }
            else if a & UInt16(HB_ATTR_CURLY) != 0 { ul = 3 }
            let strike = a & UInt16(HB_ATTR_STRIKE) != 0
            let attr: [NSAttributedString.Key: Any] =
                [.font: f, .foregroundColor: fg.color]
            let ns = NSAttributedString(string: runText, attributes: attr)
            let bgRect = geom.cellRect(row: visRow, col: runStart,
                                       width: wCells)
            runs.append(RowLayout.Run(
                x: x, line: CTLineCreateWithAttributedString(ns),
                text: ns,
                underline: ul, strike: strike, color: fg,
                bgRect: bgRect, hasBG: hasBG || selCovered(runStart, wCells, visRow),
                bgColor: bgc))
            runText = ""; inRun = false
        }
        while col < line.count {
            let cell = line[col]
            if cell.width == 0 { col += 1; continue }
            var s = cellText(cell)
            var fg = cell.fg, bgc = cell.bg
            if cell.attrs & UInt16(HB_ATTR_REVERSE) != 0 { swap(&fg, &bgc) }
            if cell.attrs & UInt16(HB_ATTR_DIM) != 0 {
                fg = Theme.RGBA(r: fg.r*0.55, g: fg.g*0.55, b: fg.b*0.55)
            }
            if cell.attrs & UInt16(HB_ATTR_CONCEAL) != 0 { s = " " }
            let w = cell.width == 2 ? 2 : 1
            if selCovered(col, w, visRow) { bgc = theme.selection }
            let key = (cell.attrs, fg, bgc)
            if inRun && (key != runKey) { flush() }
            if !inRun { runStart = col; runKey = key; inRun = true }
            runText += s
            col += w
        }
        flush()
        return RowLayout(runs: runs, hash: hash)
    }
    private func selCovered(_ col: Int, _ w: Int, _ visRow: Int) -> Bool {
        inSelection(srow: visRow - scrollOff, col: col, width: w)
    }

    // ---- 绘制（无 vt 锁）----
    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        theme.bg.color.setFill(); ctx.fill(bounds)
        if Date() < flashUntil {
            theme.fg.color.withAlphaComponent(0.08).setFill()
            ctx.fill(bounds)
        }
        snapshot()
        let visRows = min(gridRows, rows())
        for vis in 0..<visRows where vis < grid.count {
            let line = grid[vis]
            let h = hashRow(line) ^ selHash
            var lay = rowLayouts[vis]
            if lay == nil || lay!.hash != h {
                lay = layoutRow(line, visRow: vis, hash: h)
                rowLayouts[vis] = lay
            }
            drawRuns(ctx: ctx, lay: lay!, visRow: vis)
        }
        if rowLayouts.count > visRows * 2 {
            for k in rowLayouts.keys where k >= visRows { rowLayouts[k] = nil }
        }
        drawCursor(ctx: ctx)
        drawMarked(ctx: ctx)
        drawScroller(ctx: ctx)
    }

    private func drawRuns(ctx: CGContext, lay: RowLayout, visRow: Int) {
        for run in lay.runs {
            if run.hasBG {
                run.bgColor.color.setFill()
                ctx.fill(run.bgRect)
            }
            ctx.saveGState()
            ctx.translateBy(x: run.x, y: geom.baselineY(row: visRow))
            ctx.scaleBy(x: 1, y: -1)
            ctx.textMatrix = .identity
            ctx.textPosition = .zero
            CTLineDraw(run.line, ctx)
            ctx.restoreGState()
            if run.underline > 0 || run.strike {
                run.color.color.setStroke()
                let r = run.bgRect
                if run.underline == 1 || run.underline == 2 {
                    ctx.setLineWidth(run.underline == 2 ? 2 : 1)
                    ctx.stroke(CGRect(x: r.minX, y: r.maxY - 2,
                                      width: r.width, height: 1))
                } else if run.underline == 3 {
                    ctx.setLineWidth(1)
                    var x = r.minX; var up = false
                    ctx.move(to: CGPoint(x: x, y: r.maxY - 2))
                    while x < r.maxX {
                        x += 2; up.toggle()
                        ctx.addLine(to: CGPoint(x: x,
                                                y: r.maxY - (up ? 3 : 2)))
                    }
                    ctx.strokePath()
                }
                if run.strike {
                    ctx.setLineWidth(1)
                    ctx.stroke(CGRect(x: r.minX, y: r.midY,
                                      width: r.width, height: 1))
                }
            }
        }
    }

    private func drawCursor(ctx: CGContext) {
        guard curVis, scrollOff == 0, curRow >= 0, curRow < gridRows else { return }
        // 光标所在格：宽字符头占 2 列；落在尾格时回到头格。
        var col = Int(curCol)
        var w = 1
        if Int(curRow) < grid.count {
            let line = grid[Int(curRow)]
            if col < line.count, line[col].width == 0, col > 0 { col -= 1 }
            if col < line.count, line[col].width == 2 { w = 2 }
        }
        let r = geom.cellRect(row: Int(curRow), col: col, width: w)
        let style = curShape == 0 ? Int32(cursorStyle) : curShape
        switch style {
        case 2:
            guard !curBlink || cursorOn else { return }
            theme.cursor.color.setFill()
            ctx.fill(CGRect(x: r.minX, y: r.maxY - 2,
                            width: r.width, height: 2))
        case 3:
            guard !curBlink || cursorOn else { return }
            theme.cursor.color.setFill()
            ctx.fill(CGRect(x: r.minX, y: r.minY, width: 2, height: r.height))
        default:
            if !curBlink || cursorOn {
                theme.cursor.color.withAlphaComponent(0.85).setFill()
                ctx.fill(r)
                // 反色重绘光标下的字符（含宽字符两列）
                if Int(curRow) < grid.count, col < grid[Int(curRow)].count {
                    let c = grid[Int(curRow)][col]
                    let txt = cellText(c)
                    if c.chars.0 != 0 {
                        let ns = NSAttributedString(
                            string: txt,
                            attributes: [.font: font,
                                         .foregroundColor: theme.bg.color])
                        let line = CTLineCreateWithAttributedString(ns)
                        ctx.saveGState()
                        ctx.translateBy(x: r.minX,
                                        y: geom.baselineY(row: Int(curRow)))
                        ctx.scaleBy(x: 1, y: -1)
                        ctx.textMatrix = .identity
                        ctx.textPosition = .zero
                        CTLineDraw(line, ctx)
                        ctx.restoreGState()
                    }
                }
            } else {
                theme.cursor.color.setStroke()
                ctx.stroke(r.insetBy(dx: 0.5, dy: 0.5))
            }
        }
    }

    private func drawMarked(ctx: CGContext) {
        guard markedRng.location != NSNotFound, markedText.length > 0,
              scrollOff == 0 else { return }
        let str = NSMutableAttributedString(attributedString: markedText)
        str.addAttributes([.font: font,
                           .foregroundColor: theme.fg.color,
                           .underlineStyle: NSUnderlineStyle.single.rawValue],
                          range: NSRange(0..<str.length))
        let x = geom.padX + CGFloat(curCol)*geom.cellW
        let y = geom.rowTop(Int(curRow))
        let w = str.size().width
        theme.selection.color.withAlphaComponent(0.5).setFill()
        ctx.fill(CGRect(x: x, y: y, width: w, height: geom.cellH))
        let line = CTLineCreateWithAttributedString(str)
        ctx.saveGState()
        ctx.translateBy(x: x, y: geom.baselineY(row: Int(curRow)))
        ctx.scaleBy(x: 1, y: -1)
        CTLineDraw(line, ctx)
        ctx.restoreGState()
    }

    private func drawScroller(ctx: CGContext) {
        guard sbCount > 0 else { return }
        let visRows = min(gridRows, rows())
        let total = CGFloat(sbCount + visRows)
        let h = bounds.height * CGFloat(visRows) / total
        let off = CGFloat(sbCount - scrollOff) / total
        let y = bounds.height - padY - h - off * bounds.height
        theme.dim.color.withAlphaComponent(0.4).setFill()
        ctx.fill(CGRect(x: bounds.width - 5, y: y, width: 3, height: h))
    }

    // ---- 选择 ----
    private func inSelection(srow: Int, col: Int, width: Int) -> Bool {
        guard hasSel, let a = selAnchor, let c = selCaret else { return false }
        if blockSel {
            let r0 = min(a.row, c.row), r1 = max(a.row, c.row)
            let c0 = min(a.col, c.col), c1 = max(a.col, c.col)
            return srow >= r0 && srow <= r1 && col >= c0 && col < c1 + width
        }
        let (p0, p1) = (a.row, a.col) <= (c.row, c.col) ? (a, c) : (c, a)
        if srow < p0.row || srow > p1.row { return false }
        if srow == p0.row && srow == p1.row {
            return col >= p0.col && col < p1.col + max(width-1, 0)
        }
        if srow == p0.row { return col >= p0.col }
        if srow == p1.row { return col <= p1.col }
        return true
    }
    func bumpSelHash() { selHash &+= 1 }

    func copySelection() {
        guard hasSel, let a = selAnchor, let c = selCaret,
              let vt = session?.vt else { return }
        hb_vt_lock(vt); defer { hb_vt_unlock(vt) }
        let p0 = (a.row, a.col) <= (c.row, c.col) ? a : c
        let p1 = (a.row, a.col) <= (c.row, c.col) ? c : a
        let c0 = blockSel ? min(a.col, c.col) : p0.col
        let c1 = blockSel ? max(a.col, c.col) : p1.col
        guard let s = hb_vt_get_text(vt, Int32(p0.row), Int32(c0),
                                     Int32(p1.row), Int32(c1),
                                     blockSel) else { return }
        defer { free(s) }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(String(cString: s), forType: .string)
    }

    private func cellAt(_ p: NSPoint) -> SelPt {
        let (r, c) = geom.cellAt(p)
        return SelPt(row: r - scrollOff, col: c)
    }

    override func mouseDown(with e: NSEvent) {
        onFocusRequest?()
        let p = convert(e.locationInWindow, from: nil)
        blockSel = e.modifierFlags.contains(.option)
        let c = cellAt(p)
        if e.clickCount == 2, let vt = session?.vt {
            hb_vt_lock(vt)
            var l = c.col, r = c.col
            while l > 0 && cellNonSpace(vt, srow: c.row, col: l-1) { l -= 1 }
            while cellNonSpace(vt, srow: c.row, col: r+1) { r += 1 }
            hb_vt_unlock(vt)
            selAnchor = SelPt(row: c.row, col: l)
            selCaret = SelPt(row: c.row, col: r)
        } else if e.clickCount >= 3 {
            selAnchor = SelPt(row: c.row, col: 0)
            selCaret = SelPt(row: c.row, col: cols() - 1)
        } else if e.modifierFlags.contains(.shift), selAnchor != nil {
            selCaret = c
        } else {
            selAnchor = c; selCaret = c
            sendMouse(p, button: 1, pressed: true, mods: e.modifierFlags)
        }
        bumpSelHash(); poke()
    }
    private func cellNonSpace(_ vt: OpaquePointer, srow: Int, col: Int) -> Bool {
        var c = hb_cell()
        guard hb_vt_get_cell(vt, Int32(srow), Int32(col), &c) else { return false }
        return c.chars.0 != 0 && c.chars.0 != 32
    }
    override func mouseDragged(with e: NSEvent) {
        selCaret = cellAt(convert(e.locationInWindow, from: nil))
        if selAnchor == nil { selAnchor = selCaret }
        bumpSelHash(); poke()
    }
    override func mouseUp(with e: NSEvent) {
        sendMouse(convert(e.locationInWindow, from: nil),
                  button: 1, pressed: false, mods: e.modifierFlags)
        if !hasSel { selAnchor = nil; selCaret = nil }
        bumpSelHash(); poke()
    }
    override func rightMouseDown(with e: NSEvent) {
        sendMouse(convert(e.locationInWindow, from: nil),
                  button: 3, pressed: true, mods: e.modifierFlags)
    }
    override func rightMouseUp(with e: NSEvent) {
        sendMouse(convert(e.locationInWindow, from: nil),
                  button: 3, pressed: false, mods: e.modifierFlags)
    }

    private func sendMouse(_ p: NSPoint, button: Int32, pressed: Bool,
                           mods: NSEvent.ModifierFlags) {
        guard let s = session, let vt = s.vt else { return }
        guard hb_vt_mouse_mode(vt) != 0 else { return }
        var (row, col) = geom.cellAt(p)
        row = min(max(row, 0), gridRows - 1)
        col = min(max(col, 0), gridCols - 1)
        hb_vt_mouse(vt, Int32(row), Int32(col), button, pressed,
                    KeyMap.mods(mods.rawValue, optionIsMeta: true))
    }

    override func scrollWheel(with e: NSEvent) {
        guard let s = session, let vt = s.vt else { return }
        let delta = e.hasPreciseScrollingDeltas
            ? e.scrollingDeltaY : e.scrollingDeltaY * geom.cellH
        if hb_vt_altscreen(vt) {
            if hb_vt_mouse_mode(vt) != 0 {
                let (row, col) = geom.cellAt(
                    convert(e.locationInWindow, from: nil))
                let btn: Int32 = delta > 0 ? 4 : 5
                for _ in 0..<max(1, Int(abs(delta)/geom.cellH)) {
                    hb_vt_mouse(vt, Int32(row), Int32(col), btn, true, 0)
                }
            } else {
                for _ in 0..<max(1, Int(abs(delta)/geom.cellH)) {
                    hb_vt_key(vt, delta > 0 ? hb_key(rawValue: 5)
                                            : hb_key(rawValue: 6), 0)
                }
            }
            return
        }
        scrollPixels -= delta
        let off = Int(scrollPixels / geom.cellH)
        scrollPixels -= CGFloat(off) * geom.cellH
        scrollOff = min(max(scrollOff + off, 0), sbCount)
        invalidateLayout(); poke()
    }

    // ---- 键盘 ----
    override func keyDown(with e: NSEvent) {
        if inputContext?.handleEvent(e) == true { return }
        handleKey(e)
    }
    func handleKey(_ e: NSEvent) {
        guard let s = session else { return }
        let mods = KeyMap.mods(e.modifierFlags.rawValue,
                               optionIsMeta: optionIsMeta)
        if let k = KeyMap.specialKey(e.keyCode) {
            hb_session_key(s.sess, hb_key(rawValue: UInt32(k)), mods)
            inputMirror?(.key(k, mods))
            return
        }
        if mods & KeyMap.HB_MOD_CTRL != 0,
           let ch = e.charactersIgnoringModifiers?.lowercased().first,
           ch >= "a" && ch <= "z" {
            var b = UInt8(ch.asciiValue! - 96)
            withUnsafeBytes(of: &b) {
                hb_session_write(s.sess, $0.baseAddress!
                    .assumingMemoryBound(to: CChar.self), 1)
            }
            inputMirror?(.raw(Data([b])))
            return
        }
        guard let str = e.characters, let uni = str.unicodeScalars.first
        else { return }
        var m2 = mods
        if optionIsMeta && e.modifierFlags.contains(.option) {
            m2 |= KeyMap.HB_MOD_ALT
        }
        hb_session_unichar(s.sess, uni.value, m2)
        inputMirror?(.unichar(uni.value, m2))
    }

    func paste() {
        guard let str = NSPasteboard.general.string(forType: .string),
              let s = session else { return }
        str.withCString { hb_session_paste(s.sess, $0) }
        inputMirror?(.paste(str))
    }
    override func selectAll(_ sender: Any?) {
        guard let vt = session?.vt else { return }
        hb_vt_lock(vt)
        var r: Int32 = 0, c: Int32 = 0
        hb_vt_size(vt, &r, &c)
        let sb = Int(hb_vt_scrollback_count(vt))
        hb_vt_unlock(vt)
        selAnchor = SelPt(row: -sb, col: 0)
        selCaret = SelPt(row: Int(r) - 1, col: Int(c) - 1)
        bumpSelHash(); poke()
    }
    func clearSelection() {
        selAnchor = nil; selCaret = nil; bumpSelHash(); poke()
    }

    // ---- NSTextInputClient ----
    func insertText(_ string: Any, replacementRange r: NSRange) {
        let str = (string as? NSAttributedString)?.string
            ?? (string as? String) ?? ""
        unmarkText()
        guard let s = session else { return }
        for scalar in str.unicodeScalars {
            hb_session_unichar(s.sess, scalar.value, 0)
            inputMirror?(.unichar(scalar.value, 0))
        }
    }
    func setMarkedText(_ string: Any, selectedRange: NSRange,
                       replacementRange: NSRange) {
        if let a = string as? NSAttributedString {
            markedText = NSMutableAttributedString(attributedString: a)
        } else {
            markedText = NSMutableAttributedString(string: string as? String ?? "")
        }
        markedRng = markedText.length > 0
            ? NSRange(0..<markedText.length)
            : NSRange(location: NSNotFound, length: 0)
        poke()
    }
    func unmarkText() {
        markedText = NSMutableAttributedString()
        markedRng = NSRange(location: NSNotFound, length: 0)
        poke()
    }
    func hasMarkedText() -> Bool { markedRng.location != NSNotFound }
    func markedRange() -> NSRange { markedRng }
    func selectedRange() -> NSRange { NSRange(location: 0, length: 0) }
    func validAttributesForMarkedText() -> [NSAttributedString.Key] {
        [.font, .foregroundColor, .underlineStyle]
    }
    func attributedSubstring(forProposedRange range: NSRange,
                             actualRange: NSRangePointer?)
        -> NSAttributedString? {
        guard range.location != NSNotFound,
              range.location < markedText.length else { return nil }
        let r = NSIntersectionRange(range, NSRange(0..<markedText.length))
        actualRange?.pointee = r
        return markedText.attributedSubstring(from: r)
    }
    func characterIndex(for point: NSPoint) -> Int {
        Int(point.x / max(geom.cellW, 1))
    }
    func firstRect(forCharacterRange range: NSRange,
                   actualRange: NSRangePointer?) -> NSRect {
        let rc = geom.cellRect(row: Int(curRow), col: Int(curCol))
        let wr = convert(rc, to: nil)
        return window?.convertToScreen(wr) ?? wr
    }
    override func doCommand(by selector: Selector) {
        guard let s = session else { return }
        if selector == #selector(cancelOperation(_:)) {
            hb_session_key(s.sess, hb_key(rawValue: 4), 0)
            inputMirror?(.key(4, 0))
        } else if selector == #selector(deleteBackward(_:)) {
            hb_session_key(s.sess, hb_key(rawValue: 3), 0)
            inputMirror?(.key(3, 0))
        }
    }
}
