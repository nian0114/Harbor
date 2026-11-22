import Foundation
#if canImport(AppKit)
import AppKit
#else
import CoreGraphics
#endif

/// prototype/app.js 同款 24×24 stroke 图标集的 SVG 子集解析。
/// 支持 <path d>（M L H V C S Q T A→折线 Z）、<rect rx>、<circle>、
/// fill="currentColor" stroke="none" 的填充件；其余描边。
/// 输出 CGPath + 每子路径是否填充，供 AppKit 层按 tint 着色。
public struct IconPath {
    public struct Sub {
        public var path: CGPath
        public var fill: Bool
    }
    public var subs: [Sub] = []

    // -- 解析入口：一段 "<rect .../><path .../>" 串 --
    public init(svgFragment: String) {
        var rest = svgFragment[svgFragment.startIndex...]
        while let lt = rest.firstIndex(of: "<") {
            let gt = rest.index(after: lt)
            guard let end = rest[gt...].firstIndex(of: ">") else { break }
            let tag = String(rest[gt..<end])
            parseTag(tag)
            rest = rest[rest.index(after: end)...]
        }
    }

    private mutating func parseTag(_ tag: String) {
        func attr(_ name: String) -> String? {
            guard let r = tag.range(of: name + "=\"") else { return nil }
            let s = r.upperBound
            guard let e = tag[s...].firstIndex(of: "\"") else { return nil }
            return String(tag[s..<e])
        }
        let fill = attr("fill") != nil || attr("stroke") == "none"
        let p = CGMutablePath()
        if tag.hasPrefix("rect"), let x = num(attr("x")), let y = num(attr("y")),
           let w = num(attr("width")), let h = num(attr("height")) {
            let rx = num(attr("rx")) ?? 0
            p.addRoundedRect(in: CGRect(x: x, y: y, width: w, height: h),
                             cornerWidth: rx, cornerHeight: rx)
            subs.append(Sub(path: p.copy()!, fill: fill))
        } else if tag.hasPrefix("circle"), let cx = num(attr("cx")),
                  let cy = num(attr("cy")), let r = num(attr("r")) {
            p.addEllipse(in: CGRect(x: cx-r, y: cy-r, width: 2*r, height: 2*r))
            subs.append(Sub(path: p.copy()!, fill: fill))
        } else if tag.hasPrefix("path"), let d = attr("d") {
            IconPath.parseD(d, into: p)
            subs.append(Sub(path: p.copy()!, fill: fill))
        }
    }

    private func num(_ s: String?) -> Double? {
        s.flatMap { Double($0) }
    }

    // -- path d 解析（M/L/H/V/C/S/Q/T/A/Z，绝对+相对）--
    static func parseD(_ d: String, into p: CGMutablePath) {
        var i = d.startIndex
        var cmd: Character = " "; var cur = CGPoint.zero; var start = CGPoint.zero
        var lastCtrl: CGPoint? = nil
        func skipSep() {
            while i < d.endIndex && (d[i] == " " || d[i] == "," || d[i] == "\n") {
                i = d.index(after: i) }
        }
        func next() -> Double? {
            skipSep()
            var s = i
            if s < d.endIndex && (d[s] == "-" || d[s] == "+") {
                s = d.index(after: s) }
            var has = false
            while s < d.endIndex && (d[s].isNumber || d[s] == ".") {
                if d[s] == "." && s > d.startIndex &&
                   d[d.index(before: s)] == "." { break }
                s = d.index(after: s); has = true
            }
            // 处理 "1.2.3" 与科学计数 e-4
            if s < d.endIndex && (d[s] == "e" || d[s] == "E") {
                var t = d.index(after: s)
                if t < d.endIndex && (d[t] == "-" || d[t] == "+") {
                    t = d.index(after: t) }
                while t < d.endIndex && d[t].isNumber { t = d.index(after: t) }
                s = t
            }
            guard has else { return nil }
            let v = Double(d[i..<s]); i = s; return v
        }
        func pt(_ rel: Bool) -> CGPoint? {
            guard let x = next(), let y = next() else { return nil }
            return rel ? CGPoint(x: cur.x + x, y: cur.y + y)
                       : CGPoint(x: x, y: y)
        }
        while true {
            skipSep()
            if i >= d.endIndex { break }
            if d[i].isLetter { cmd = d[i]; i = d.index(after: i); lastCtrl = nil }
            let rel = cmd.isLowercase
            switch Character(cmd.lowercased()) {
            case "m":
                guard let q = pt(rel) else { return }
                p.move(to: q); cur = q; start = q
                // 隐式 lineto 后续
                while let q = pt(rel) { p.addLine(to: q); cur = q }
            case "l":
                guard let q = pt(rel) else { return }
                p.addLine(to: q); cur = q
            case "h":
                guard let x = next() else { return }
                cur = CGPoint(x: rel ? cur.x + x : x, y: cur.y)
                p.addLine(to: cur)
            case "v":
                guard let y = next() else { return }
                cur = CGPoint(x: cur.x, y: rel ? cur.y + y : y)
                p.addLine(to: cur)
            case "c":
                guard let c1 = pt(rel), let c2 = pt(rel), let q = pt(rel)
                else { return }
                p.addCurve(to: q, control1: c1, control2: c2)
                cur = q; lastCtrl = c2
            case "s":
                guard let c2 = pt(rel), let q = pt(rel) else { return }
                let c1 = lastCtrl.map {
                    CGPoint(x: 2*cur.x - $0.x, y: 2*cur.y - $0.y)
                } ?? cur
                p.addCurve(to: q, control1: c1, control2: c2)
                cur = q; lastCtrl = c2
            case "q":
                guard let c = pt(rel), let q = pt(rel) else { return }
                p.addQuadCurve(to: q, control: c)
                cur = q; lastCtrl = c
            case "t":
                guard let q = pt(rel) else { return }
                let c = lastCtrl.map {
                    CGPoint(x: 2*cur.x - $0.x, y: 2*cur.y - $0.y)
                } ?? cur
                p.addQuadCurve(to: q, control: c)
                cur = q; lastCtrl = c
            case "a":
                // arc → 折线近似（图标里仅小圆点用得到）
                guard let _ = next(), let _ = next(), let _ = next(),
                      let laf = next(), let _ = next(), let q = pt(rel)
                else { return }
                // 简单近似：large-arc 时画半圆，否则直线
                if laf == 1 {
                    let mid = CGPoint(x: (cur.x+q.x)/2, y: (cur.y+q.y)/2)
                    p.addArc(center: mid,
                             radius: hypot(q.x-cur.x, q.y-cur.y)/2,
                             startAngle: 0, endAngle: .pi*2, clockwise: false)
                } else { p.addLine(to: q) }
                cur = q
            case "z":
                p.closeSubpath(); cur = start
            default:
                _ = next() // 未知指令丢一个数避免死循环
                if i < d.endIndex && !d[i].isLetter { i = d.index(after: i) }
            }
        }
    }
}
