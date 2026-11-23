// IconGen —— 把 linux/data/icons 的 dev.harbor.Harbor.svg 渲染成
// AppIcon.icns + 菜单栏模板 PNG（statusIcon.png / @2x）。
// 用法：IconGen <svg路径> <输出目录>
// 构建期由 scripts/build-macos-app.sh 调用，图标始终同源生成。
import AppKit

// ---------- 极简 SVG 子集解析 ----------
// 支持：<linearGradient><stop>、<rect rx transform>、<circle>、
// <path d>（M L H V C Q Z + 真实 A 圆弧）、<g transform opacity fill>、
// fill="url(#id)"、stroke/stroke-width/linecap/linejoin。
struct Grad { var stops: [(Double, NSColor)] = [] }
struct State {
    var xf = CGAffineTransform.identity
    var opacity: CGFloat = 1
    var fill: NSColor? = .white
}

func num(_ tag: String, _ a: String) -> Double? {
    guard let r = tag.range(of: a + "=\"") else { return nil }
    let s = r.upperBound
    guard let e = tag[s...].firstIndex(of: "\"") else { return nil }
    return Double(tag[s..<e])
}
func str(_ tag: String, _ a: String) -> String? {
    guard let r = tag.range(of: a + "=\"") else { return nil }
    let s = r.upperBound
    guard let e = tag[s...].firstIndex(of: "\"") else { return nil }
    return String(tag[s..<e])
}
func color(_ s: String?) -> NSColor? {
    guard let s, s.hasPrefix("#") else { return nil }
    let h = String(s.dropFirst())
    guard h.count == 6, let v = UInt32(h, radix: 16) else { return nil }
    return NSColor(red: CGFloat(v >> 16 & 0xff) / 255,
                   green: CGFloat(v >> 8 & 0xff) / 255,
                   blue: CGFloat(v & 0xff) / 255, alpha: 1)
}
func parseXf(_ s: String?) -> CGAffineTransform {
    var t = CGAffineTransform.identity
    guard let s else { return t }
    var rest = s
    while let p = rest.firstIndex(of: "(") {
        let name = String(rest[rest.startIndex..<p])
            .trimmingCharacters(in: .whitespaces)
        guard let q = rest[p...].firstIndex(of: ")") else { break }
        let nums = rest[rest.index(after: p)..<q]
            .split(whereSeparator: { " ,\t".contains($0) })
            .compactMap { Double($0) }
        switch name {
        case "translate":
            t = t.translatedBy(x: nums[safe: 0] ?? 0, y: nums[safe: 1] ?? 0)
        case "scale":
            t = t.scaledBy(x: nums[safe: 0] ?? 1, y: nums[safe: 1]
                ?? nums[safe: 0] ?? 1)
        case "rotate":
            let a = (nums[safe: 0] ?? 0) * .pi / 180
            if nums.count > 2 {
                t = t.translatedBy(x: nums[1], y: nums[2])
                    .rotated(by: a)
                    .translatedBy(x: -nums[1], y: -nums[2])
            } else { t = t.rotated(by: a) }
        default: break
        }
        rest = String(rest[rest.index(after: q)...])
    }
    return t
}
extension Array { subscript(safe i: Int) -> Element? {
    indices.contains(i) ? self[i] : nil } }

/// path d → CGPath（A 用端点参数化转中心参数化，近似成小步线段）。
func parseD(_ d: String) -> CGPath {
    let p = CGMutablePath()
    var i = d.startIndex, cmd: Character = " "
    var cur = CGPoint.zero, start = CGPoint.zero, ctrl = CGPoint.zero
    func isSep(_ c: Character) -> Bool { c == "," || c == " " || c == "\t" }
    func next() -> Double? {
        while i < d.endIndex && isSep(d[i]) { i = d.index(after: i) }
        guard i < d.endIndex else { return nil }
        var j = i, neg = false
        if d[j] == "-" || d[j] == "+" { neg = d[j] == "-"; j = d.index(after: j) }
        var n = ""
        while j < d.endIndex && (d[j].isNumber || d[j] == ".") {
            n.append(d[j]); j = d.index(after: j)
        }
        i = j
        guard let v = Double(n) else { return nil }
        return neg ? -v : v
    }
    func pt(_ rel: Bool) -> CGPoint? {
        guard let x = next(), let y = next() else { return nil }
        return rel ? CGPoint(x: cur.x + x, y: cur.y + y)
                   : CGPoint(x: x, y: y)
    }
    while i < d.endIndex {
        if d[i].isLetter { cmd = d[i]; i = d.index(after: i) }
        let rel = cmd.isLowercase
        switch Character(cmd.lowercased()) {
        case "m": guard let q = pt(rel) else { return p }
            p.move(to: q); cur = q; start = q; cmd = rel ? "l" : "L"
        case "l": guard let q = pt(rel) else { return p }
            p.addLine(to: q); cur = q
        case "h": guard let x = next() else { return p }
            let q = CGPoint(x: rel ? cur.x + x : x, y: cur.y)
            p.addLine(to: q); cur = q
        case "v": guard let y = next() else { return p }
            let q = CGPoint(x: cur.x, y: rel ? cur.y + y : y)
            p.addLine(to: q); cur = q
        case "c": guard let c1 = pt(rel), let c2 = pt(rel), let q = pt(rel)
            else { return p }
            p.addCurve(to: q, control1: c1, control2: c2)
            cur = q; ctrl = c2
        case "s": guard let c2 = pt(rel), let q = pt(rel) else { return p }
            let c1 = CGPoint(x: 2*cur.x - ctrl.x, y: 2*cur.y - ctrl.y)
            p.addCurve(to: q, control1: c1, control2: c2)
            cur = q; ctrl = c2
        case "q": guard let c = pt(rel), let q = pt(rel) else { return p }
            p.addQuadCurve(to: q, control: c); cur = q; ctrl = c
        case "t": guard let q = pt(rel) else { return p }
            p.addQuadCurve(to: q, control:
                CGPoint(x: 2*cur.x - ctrl.x, y: 2*cur.y - ctrl.y)); cur = q
        case "a":
            // SVG 端点→中心参数化 → 折线近似
            guard let rx0 = next(), let ry0 = next(), let rot = next(),
                  let laf = next(), let swp = next(), let q = pt(rel)
            else { return p }
            let seg = arcSegs(from: cur, to: q, rx: rx0, ry: ry0,
                              rotDeg: rot, large: laf != 0, sweep: swp != 0)
            seg.forEach { p.addLine(to: $0) }
            cur = q
        case "z": p.closeSubpath(); cur = start
        default: _ = next()
        }
    }
    return p
}
/// 端点→中心参数化，返回弧上采样点（含终点）。
func arcSegs(from p0: CGPoint, to p1: CGPoint, rx: Double, ry: Double,
             rotDeg: Double, large: Bool, sweep: Bool) -> [CGPoint] {
    var rx = abs(rx), ry = abs(ry)
    guard rx > 0, ry > 0, p0 != p1 else { return [p1] }
    let phi = rotDeg * .pi / 180, cp = cos(phi), sp = sin(phi)
    let dx = (p0.x - p1.x)/2, dy = (p0.y - p1.y)/2
    let x1p = cp*dx + sp*dy, y1p = -sp*dx + cp*dy
    let lam = x1p*x1p/(rx*rx) + y1p*y1p/(ry*ry)
    if lam > 1 { rx *= sqrt(lam); ry *= sqrt(lam) }
    var num = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p
    num = max(0, num)
    var co = sqrt(num / (rx*rx*y1p*y1p + ry*ry*x1p*x1p))
    if large == sweep { co = -co }
    let cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx
    let cx = cp*cxp - sp*cyp + (p0.x + p1.x)/2
    let cy = sp*cxp + cp*cyp + (p0.y + p1.y)/2
    func ang(_ ux: Double, _ uy: Double, _ vx: Double, _ vy: Double)
        -> Double {
        let d = sqrt(ux*ux + uy*uy) * sqrt(vx*vx + vy*vy)
        var c = (ux*vx + uy*vy) / max(d, 1e-12)
        c = min(1, max(-1, c))
        let s = ux*vy - uy*vx < 0 ? -1.0 : 1.0
        return s * acos(c)
    }
    var th = ang(1, 0, (x1p - cxp)/rx, (y1p - cyp)/ry)
    var dth = ang((x1p - cxp)/rx, (y1p - cyp)/ry,
                  (-x1p - cxp)/rx, (-y1p - cyp)/ry)
    if !sweep && dth > 0 { dth -= 2 * .pi }
    if sweep && dth < 0 { dth += 2 * .pi }
    let n = max(4, Int(abs(dth) / 0.15))
    var out: [CGPoint] = []
    for k in 1...n {
        let a = th + dth * Double(k)/Double(n)
        let x = cp*rx*cos(a) - sp*ry*sin(a) + cx
        let y = sp*rx*cos(a) + cp*ry*sin(a) + cy
        out.append(CGPoint(x: x, y: y))
    }
    return out
}

// ---------- 渲染 ----------
/// 把 SVG 画进 ctx（size×size，SVG 坐标左上原点 → 翻转）。
/// template 模式：跳过渐变背景，所有白色件改黑（菜单栏模板图）。
func render(svg: String, into ctx: CGContext, size: CGFloat,
            template: Bool) {
    ctx.translateBy(x: 0, y: size)
    ctx.scaleBy(x: size/128, y: -size/128)
    var grads: [String: Grad] = [:]
    var stack: [State] = [State()]
    var rest = svg[svg.startIndex...]
    func push(_ s: State) { stack.append(s) }
    func top() -> State { stack.last! }
    while let lt = rest.firstIndex(of: "<") {
        rest = rest[rest.index(after: lt)...]
        guard let gt = rest.firstIndex(of: ">") else { break }
        var tag = String(rest[rest.startIndex..<gt])
        rest = rest[rest.index(after: gt)...]
        if tag.hasPrefix("?") || tag.hasPrefix("!") { continue }
        let close = tag.hasPrefix("/")
        if close {
            let name = String(tag.dropFirst())
            if name == "g" && stack.count > 1 { stack.removeLast() }
            continue
        }
        let selfClose = tag.hasSuffix("/")
        if selfClose { tag = String(tag.dropLast()) }
        let st = top()
        if tag.hasPrefix("g ") || tag == "g" {
            var ns = st
            ns.xf = st.xf.concatenating(parseXf(str(tag, "transform")))
            if let o = num(tag, "opacity") { ns.opacity = st.opacity * o }
            if let f = color(str(tag, "fill")) { ns.fill = f }
            push(ns)
            continue
        }
        if tag.hasPrefix("linearGradient") {
            let id = str(tag, "id") ?? ""
            var g = Grad()
            // 找配对 </linearGradient> 收集 stops
            if let e = rest.firstIndex(of: "</linearGradient>") {
                let body = rest[rest.startIndex..<e]
                var b = body
                while let s = b.firstIndex(of: "<stop") {
                    b = b[b.index(after: s)...]
                    guard let se = b.firstIndex(of: ">") else { break }
                    let stag = String(b[b.startIndex..<se])
                    if let off = num(stag, "offset"),
                       let c = color(str(stag, "stop-color")) {
                        g.stops.append((off, c))
                    }
                    b = b[b.index(after: se)...]
                }
                rest = rest[rest.index(after: e)...]
            }
            grads[id] = g
            continue
        }
        if tag.hasPrefix("stop") || tag.hasPrefix("defs")
            || tag.hasPrefix("/defs") { continue }

        ctx.saveGState()
        ctx.concat(st.xf)
        let xf = parseXf(str(tag, "transform"))
        ctx.concat(xf)
        ctx.setAlpha(st.opacity)
        let strokeC = color(str(tag, "stroke"))
        let fillAttr = str(tag, "fill")
        let fillC = color(fillAttr) ?? (fillAttr == nil ? st.fill : nil)
        let w = num(tag, "stroke-width") ?? 1
        if tag.hasPrefix("rect"), let x = num(tag, "x"), let y = num(tag, "y"),
           let rw = num(tag, "width"), let rh = num(tag, "height") {
            let rx = num(tag, "rx") ?? 0
            let r = CGRect(x: x, y: y, width: rw, height: rh)
            let path = CGPath(roundedRect: r, cornerWidth: rx,
                              cornerHeight: rx, transform: nil)
            if let f = fillAttr, f.hasPrefix("url(#") {
                let id = String(f.dropFirst(5).dropLast())
                if template { /* 模板：跳过渐变底 */ }
                else if let g = grads[id] {
                    ctx.addPath(path); ctx.clip()
                    let cols = g.stops.map(\.1) as CFArray
                    let loc = g.stops.map(\.0)
                    if let cg = CGGradient(colorsSpace:
                        CGColorSpaceCreateDeviceRGB(), colors: cols,
                        locations: loc) {
                        ctx.drawLinearGradient(cg,
                            start: CGPoint(x: 0, y: r.minY),
                            end: CGPoint(x: 0, y: r.maxY), options: [])
                    }
                }
            } else if let f = fillC {
                (template ? NSColor.black : f).setFill()
                ctx.addPath(path); ctx.fillPath()
            }
            if let s = strokeC {
                (template ? NSColor.black : s).setStroke()
                ctx.setLineWidth(w)
                ctx.addPath(path); ctx.strokePath()
            }
        } else if tag.hasPrefix("circle"), let cx = num(tag, "cx"),
                  let cy = num(tag, "cy"), let rr = num(tag, "r") {
            let path = CGPath(ellipseIn:
                CGRect(x: cx-rr, y: cy-rr, width: 2*rr, height: 2*rr),
                transform: nil)
            if let f = fillC {
                (template ? NSColor.black : f).setFill()
                ctx.addPath(path); ctx.fillPath()
            }
            if let s = strokeC {
                (template ? NSColor.black : s).setStroke()
                ctx.setLineWidth(w)
                ctx.setLineCap(.round)
                ctx.addPath(path); ctx.strokePath()
            }
        } else if tag.hasPrefix("path"), let d = str(tag, "d") {
            let path = parseD(d)
            if let f = fillC {
                (template ? NSColor.black : f).setFill()
                ctx.addPath(path); ctx.fillPath()
            }
            if let s = strokeC {
                (template ? NSColor.black : s).setStroke()
                ctx.setLineWidth(w)
                ctx.setLineCap(.round)
                ctx.setLineJoin(.round)
                ctx.addPath(path); ctx.strokePath()
            }
        }
        ctx.restoreGState()
    }
}

func png(svg: String, size: Int, template: Bool, out: String) {
    let s = CGFloat(size)
    let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
        isPlanar: false, colorSpaceName: .deviceRGB,
        bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    render(svg: svg, into: NSGraphicsContext.current!.cgContext,
           size: s, template: template)
    NSGraphicsContext.restoreGraphicsState()
    let d = rep.representation(using: .png, properties: [:])!
    try! d.write(to: URL(fileURLWithPath: out))
}

// ---------- main ----------
let args = CommandLine.arguments
guard args.count == 3,
      let svg = try? String(contentsOfFile: args[1]) else {
    FileHandle.standardError.write("usage: IconGen <svg> <outdir>\n"
        .data(using: .utf8)!)
    exit(2)
}
let out = args[2]
let fm = FileManager.default
let iconset = out + "/AppIcon.iconset"
try? fm.removeItem(atPath: iconset)
try! fm.createDirectory(atPath: iconset, withIntermediateDirectories: true)
// icns 全尺寸
for (base, px) in [("icon_16x16", 16), ("icon_16x16@2x", 32),
                   ("icon_32x32", 32), ("icon_32x32@2x", 64),
                   ("icon_128x128", 128), ("icon_128x128@2x", 256),
                   ("icon_256x256", 256), ("icon_256x256@2x", 512),
                   ("icon_512x512", 512), ("icon_512x512@2x", 1024)] {
    png(svg: svg, size: px, template: false, out: "\(iconset)/\(base).png")
}
let icns = Process()
icns.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
icns.arguments = ["-c", "icns", iconset, "-o", out + "/AppIcon.icns"]
try! icns.run(); icns.waitUntilExit()
guard icns.terminationStatus == 0 else {
    FileHandle.standardError.write("iconutil failed\n"
        .data(using: .utf8)!)
    exit(1)
}
// 菜单栏模板图（无渐变底、白色件→黑）
png(svg: svg, size: 18, template: true, out: out + "/statusIcon.png")
png(svg: svg, size: 36, template: true, out: out + "/statusIcon@2x.png")
print("icns: \(out)/AppIcon.icns")
