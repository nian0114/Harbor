import Foundation
#if canImport(AppKit)
import AppKit
#else
import CoreGraphics
#endif

/// 设计 token —— 名称与 linux/data/style.css 的 @define-color hb_* 一一对应。
/// M1a 只有亮色；所有颜色必须经此表取用（M9 加 dark 表）。
public struct Theme {
    public struct RGBA: Equatable {
        public var r: Double, g: Double, b: Double, a: Double
        public init(r: Double, g: Double, b: Double, a: Double = 1) {
            self.r = r; self.g = g; self.b = b; self.a = a
        }
        public init(_ hex: UInt32, _ alpha: Double = 1) {
            r = Double((hex >> 16) & 0xff) / 255
            g = Double((hex >> 8) & 0xff) / 255
            b = Double(hex & 0xff) / 255
            a = alpha
        }
        #if canImport(AppKit)
        public var color: NSColor {
            NSColor(calibratedRed: r, green: g, blue: b, alpha: a)
        }
        #endif
    }

    // -- 表面 --
    public static let titlebar     = RGBA(0x222939)
    public static let titlebarTab  = RGBA(0x39415a)
    public static let page         = RGBA(0xe9edf1)
    public static let content      = RGBA(0xedf1f4)
    public static let sidebar      = RGBA(0xf7f9fb)
    public static let card         = RGBA(0xffffff)
    public static let line         = RGBA(0xdfe4ea)
    public static let text         = RGBA(0x1c2333)
    public static let btn          = RGBA(0xe4e8ed)
    public static let btnHover     = RGBA(0xd8dde4)
    public static let hover        = RGBA(0xeef1f5)
    public static let subtle       = RGBA(0xf4f6f8)
    public static let sel          = RGBA(0xeaf2fd)
    public static let chip         = RGBA(0xf0f3f7)
    // -- 语义 --
    public static let blue         = RGBA(0x2f7cf6)
    public static let green        = RGBA(0x22a06b)
    public static let red          = RGBA(0xe5484d)
    public static let amber        = RGBA(0xf0a13a)
    public static let primaryHover = RGBA(0x2468d8)
    public static let dangerBg     = RGBA(0xfbe9ea)
    public static let errBg        = RGBA(0xfdf0f0)
    public static let errLine      = RGBA(0xf3c6c8)
    public static let hintAmberBg  = RGBA(0xfdf6ea)
    public static let hintAmberLn  = RGBA(0xf3e2bb)
    public static let hintAmberFg  = RGBA(0x8a6a1f)
    public static let hintRedBg    = RGBA(0xfdefef)
    public static let hintRedFg    = RGBA(0xa03033)
    public static let hintBlueBg   = RGBA(0xf0f6ff)
    public static let bluePillBg   = RGBA(0xe8f0fe)
    public static let bluePillFg   = RGBA(0x2b6cd4)
    public static let greenPillBg  = RGBA(0xe6f6ee)
    public static let greenPillFg  = RGBA(0x1d7a4f)
    // -- tab strip / 状态点 --
    public static let dotOk        = RGBA(0x2ecc71)
    public static let dotErr       = RGBA(0xff5f57)
    public static let dotPend      = RGBA(0xf5a623)
    public static let dotShare     = RGBA(0x4aa3ff)
    // -- 终端默认（hb_term_bg / hb_term_text / hb_term_dim）--
    public static let termBg       = RGBA(0x0a0f1e)
    public static let termText     = RGBA(0x3fd97f)
    public static let termDim      = RGBA(0x8b96ad)
    // -- 头像 swatch（c-*，经 token 取用）--
    public static let avatarLocal  = RGBA(0x22a06b)
    public static let avatarRdp    = RGBA(0x2f7cf6)
    public static let avatarVnc    = RGBA(0xf0a13a)

    /// xterm 256 色板（0-15 ANSI + 216 cube + 24 gray）。
    public static let palette256: [RGBA] = {
        var t = [RGBA](repeating: RGBA(0), count: 256)
        let ansi: [UInt32] = [0x000000,0xcc0000,0x4e9a06,0xc4a000,0x3465a4,
                              0x75507b,0x06989a,0xd3d7cf,0x555753,0xef2929,
                              0x8ae234,0xfce94f,0x729fcf,0xad7fa8,0x34e2e2,
                              0xeeeeec]
        for i in 0..<16 { t[i] = RGBA(ansi[i]) }
        let cube: [Double] = [0, 95, 135, 175, 215, 255].map { $0/255 }
        var n = 16
        for r in 0..<6 { for g in 0..<6 { for b in 0..<6 {
            t[n] = RGBA(r: cube[r], g: cube[g], b: cube[b]); n += 1 } } }
        for i in 0..<24 {
            let v = Double(8 + 10*i)/255
            t[232+i] = RGBA(r: v, g: v, b: v)
        }
        return t
    }()
    public static let selection = RGBA(0x4a90e2, 0.35)
    public static let marked    = RGBA(0xf0a13a, 0.30)
}
private extension Theme.RGBA {
    init(r: Double, g: Double, b: Double) {
        self = Theme.RGBA(0)
        self.r = r; self.g = g; self.b = b; self.a = 1
    }
}
