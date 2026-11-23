import Foundation
#if canImport(AppKit)
import AppKit
#else
import CoreGraphics
#endif

/// 设计 token —— 名称与 linux/data/style.css 的 @define-color hb_* 一一对应
/// （hb_foo_bar → fooBar）。亮色表来自 style.css，暗色表来自
/// theme-dark.css，两侧 token 集合完全一致（有测试断言）。
/// 所有颜色必须经此表取用。
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

    /// ui_theme: light / dark / system（跟随系统时读 NSApp 的
    /// effectiveAppearance；测试或无 NSApp 环境按亮色处理）。
    public enum Mode: String {
        case light, dark, system
    }
    public static var mode: Mode = .light

    public static var isDark: Bool {
        switch mode {
        case .light: return false
        case .dark:  return true
        case .system:
            #if canImport(AppKit)
            guard Thread.isMainThread,
                  let app = NSApp?.effectiveAppearance
            else { return false }
            return app.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            #else
            return false
            #endif
        }
    }

    public static func c(_ name: String) -> RGBA {
        (isDark ? dark : light)[name] ?? light[name] ?? RGBA(0xff00ff)
    }
#if canImport(AppKit)
    /// 供 draw/绘制路径取色（等价 c(name).color）。
    public static func color(_ name: String) -> NSColor { c(name).color }
    /// 动态色：每次 set() 时按当前 mode 重新解析——hbIcon 这类
    /// 惰性绘制图片必须用它，静态 .color 会把创建时的浅/深结果固化。
    public static func dyn(_ name: String) -> NSColor {
        NSColor(name: nil, dynamicProvider: { _ in c(name).color })
    }
#endif

    // ---- token 表（style.css / theme-dark.css 逐 token 移植）----

public static let light: [String: RGBA] = [
        "accentBgColor": .init(0x2f7cf6),
        "accentColor": .init(0x2f7cf6),
        "accentFgColor": .init(0xffffff),
        "amber": .init(0xf0a13a),
        "avatarLocal": .init(0x3b4559),
        "avatarRdp": .init(0x0f6cbd),
        "avatarVnc": .init(0x3f8cff),
        "blue": .init(0x2f7cf6),
        "bluePillBg": .init(0xe8f0fe),
        "bluePillFg": .init(0x2b6cd4),
        "btn": .init(0xe4e8ed),
        "btnHover": .init(0xd8dde4),
        "cPink": .init(0xec4899),
        "cPurple": .init(0x8b5cf6),
        "card": .init(0xffffff),
        "cardHover": .init(0xc9d4e2),
        "chip": .init(0xf0f3f7),
        "content": .init(0xedf1f4),
        "dangerBg": .init(0xfbe9ea),
        "dotErr": .init(0xff5f57),
        "dotOk": .init(0x2ecc71),
        "dotPend": .init(0xf5a623),
        "dotShare": .init(0x4aa3ff),
        "endOvl": .init(0xffffff, 0.92),
        "errBg": .init(0xfdf0f0),
        "errLine": .init(0xf3c6c8),
        "focusGreen": .init(0x2ea043),
        "grad1": .init(0x4aa3ff),
        "grad2": .init(0x7b61ff),
        "green": .init(0x22a06b),
        "greenPillBg": .init(0xe6f6ee),
        "greenPillFg": .init(0x1d7a4f),
        "hintAmberBg": .init(0xfdf6ea),
        "hintAmberFg": .init(0x8a6a1f),
        "hintAmberLine": .init(0xf3e2bb),
        "hintBlueBg": .init(0xf0f6ff),
        "hintBlueFg": .init(0x2b5c9e),
        "hintBlueLine": .init(0xd6e6fb),
        "hintRedBg": .init(0xfdefef),
        "hintRedFg": .init(0xa03033),
        "hover": .init(0xeef1f5),
        "kbdBg": .init(0xffffff),
        "line": .init(0xdfe4ea),
        "notifHover": .init(0xf2f5f9),
        "notifUnread": .init(0xf0f6ff),
        "odActive": .init(0xffffff, 0.12),
        "odHover": .init(0xffffff, 0.06),
        "odHover2": .init(0xffffff, 0.08),
        "odLine": .init(0xffffff, 0.08),
        "odSelected": .init(0xffffff, 0.14),
        "odStrong": .init(0xffffff, 0.16),
        "onAccent": .init(0xffffff),
        "page": .init(0xe9edf1),
        "palScrim": .init(0x0f1626, 0.45),
        "palShadow": .init(0x0a1428, 0.4),
        "pending": .init(0xcfd6de),
        "pillErrBg": .init(0xfdebec),
        "pillErrFg": .init(0xb3373b),
        "pillRunBg": .init(0xe8f1fd),
        "pillRunFg": .init(0x2f66c4),
        "pillWaitBg": .init(0xfdf3e3),
        "pillWaitFg": .init(0xb4761a),
        "primaryHover": .init(0x2468d8),
        "rdBanner": .init(0x10233c),
        "rdBannerFg": .init(0x7db8f2),
        "rdBg": .init(0x10141f),
        "rdCardLine": .init(0x2a3454),
        "rdDimFg": .init(0x5d87b8),
        "rdFg": .init(0xcdd4e4),
        "rdHint": .init(0xf2c97d),
        "rdLine": .init(0x1c3a5e),
        "rdOvl": .init(0x10141f, 0.55),
        "rdOvl2": .init(0x10141f, 0.9),
        "rdTagFg": .init(0x9ecbf5),
        "red": .init(0xe5484d),
        "rowHover": .init(0xf6f9fc),
        "rowLine": .init(0xeef1f4),
        "scrim": .init(0x10141c, 0.45),
        "scroll1": .init(0xe7ebf0),
        "scroll2": .init(0xd9dee5),
        "sel": .init(0xeaf2fd),
        "shadow": .init(0x000000, 0.05),
        "shadowRaised": .init(0x000000, 0.09),
        "shareBannerBg": .init(0xf0fdf6),
        "shareBannerLine": .init(0xd7eedd),
        "sidebar": .init(0xf7f9fb),
        "sidebarAct": .init(0xe3e8ee),
        "subtle": .init(0xf4f6f8),
        "tabTermBg": .init(0x12351f),
        "tabTermFg": .init(0x7fe0a3),
        "tabTermLine": .init(0x1f5c33),
        "tableHead": .init(0xfbfcfd),
        "termBg": .init(0x0a0f1e),
        "termDim": .init(0x8b96ad),
        "termText": .init(0x3fd97f),
        "text": .init(0x1c2333),
        "text2": .init(0x5b6472),
        "text3": .init(0x9aa3b0),
        "titlebar": .init(0x222939),
        "titlebarTab": .init(0x39415a),
        "titlebarText": .init(0xcdd4e4),
        "ubuntu": .init(0xe95420),
        "warnBg": .init(0xfdf3da),
        "xferCancel": .init(0x9aa3ad),
        "xferDone": .init(0x2da44e),
        "xferFail": .init(0xd1242f),
        "xferFill": .init(0xe3e8ee),
    ]
    public static let dark: [String: RGBA] = [
        "accentBgColor": .init(0x5a93f7),
        "accentColor": .init(0x5a93f7),
        "accentFgColor": .init(0xffffff),
        "amber": .init(0xe5b562),
        "avatarLocal": .init(0x3b4559),
        "avatarRdp": .init(0x0f6cbd),
        "avatarVnc": .init(0x3f8cff),
        "blue": .init(0x5a93f7),
        "bluePillBg": .init(0x2b3a55),
        "bluePillFg": .init(0x8db4f0),
        "btn": .init(0x2a3242),
        "btnHover": .init(0x333d52),
        "cPink": .init(0xec4899),
        "cPurple": .init(0x8b5cf6),
        "card": .init(0x242b3a),
        "cardHover": .init(0x46536b),
        "chip": .init(0x28303f),
        "content": .init(0x1d2331),
        "dangerBg": .init(0x4a2b2d),
        "dotErr": .init(0xff5f57),
        "dotOk": .init(0x2ecc71),
        "dotPend": .init(0xf5a623),
        "dotShare": .init(0x4aa3ff),
        "endOvl": .init(0x141822, 0.92),
        "errBg": .init(0x3d2628),
        "errLine": .init(0x5a3a3d),
        "focusGreen": .init(0x3fb950),
        "grad1": .init(0x4aa3ff),
        "grad2": .init(0x7b61ff),
        "green": .init(0x3fbf8f),
        "greenPillBg": .init(0x234034),
        "greenPillFg": .init(0x63cf9e),
        "hintAmberBg": .init(0x3d3524),
        "hintAmberFg": .init(0xe0b45c),
        "hintAmberLine": .init(0x57492a),
        "hintBlueBg": .init(0x26344f),
        "hintBlueFg": .init(0x9cc0f2),
        "hintBlueLine": .init(0x3a4f75),
        "hintRedBg": .init(0x3d2628),
        "hintRedFg": .init(0xe08084),
        "hover": .init(0x2a3242),
        "kbdBg": .init(0x2a3242),
        "line": .init(0x333c4f),
        "notifHover": .init(0x252c3c),
        "notifUnread": .init(0x26344f),
        "odActive": .init(0xffffff, 0.12),
        "odHover": .init(0xffffff, 0.06),
        "odHover2": .init(0xffffff, 0.08),
        "odLine": .init(0xffffff, 0.08),
        "odSelected": .init(0xffffff, 0.14),
        "odStrong": .init(0xffffff, 0.16),
        "onAccent": .init(0xffffff),
        "page": .init(0x171c28),
        "palScrim": .init(0x000000, 0.5),
        "palShadow": .init(0x000000, 0.6),
        "pending": .init(0x46536b),
        "pillErrBg": .init(0x3d2628),
        "pillErrFg": .init(0xe08084),
        "pillRunBg": .init(0x26344f),
        "pillRunFg": .init(0x8db4f0),
        "pillWaitBg": .init(0x3d3524),
        "pillWaitFg": .init(0xe0b45c),
        "primaryHover": .init(0x3a7bd9),
        "rdBanner": .init(0x10233c),
        "rdBannerFg": .init(0x7db8f2),
        "rdBg": .init(0x10141f),
        "rdCardLine": .init(0x2a3454),
        "rdDimFg": .init(0x5d87b8),
        "rdFg": .init(0xcdd4e4),
        "rdHint": .init(0xf2c97d),
        "rdLine": .init(0x1c3a5e),
        "rdOvl": .init(0x10141f, 0.55),
        "rdOvl2": .init(0x10141f, 0.9),
        "rdTagFg": .init(0x9ecbf5),
        "red": .init(0xf26f74),
        "rowHover": .init(0x242b3a),
        "rowLine": .init(0x2a3242),
        "scrim": .init(0x000000, 0.5),
        "scroll1": .init(0x3a4250),
        "scroll2": .init(0x2a3242),
        "sel": .init(0x2b3a55),
        "shadow": .init(0x000000, 0.4),
        "shadowRaised": .init(0x000000, 0.5),
        "shareBannerBg": .init(0x1f3a2c),
        "shareBannerLine": .init(0x2d5540),
        "sidebar": .init(0x171c28),
        "sidebarAct": .init(0x28303f),
        "subtle": .init(0x242b3a),
        "tabTermBg": .init(0x12351f),
        "tabTermFg": .init(0x7fe0a3),
        "tabTermLine": .init(0x1f5c33),
        "tableHead": .init(0x202634),
        "termBg": .init(0x0a0f1e),
        "termDim": .init(0x8b96ad),
        "termText": .init(0x3fd97f),
        "text": .init(0xe4e9f2),
        "text2": .init(0x9aa5b5),
        "text3": .init(0x6b7684),
        "titlebar": .init(0x1b2130),
        "titlebarTab": .init(0x2a3145),
        "titlebarText": .init(0xcdd4e4),
        "ubuntu": .init(0xe95420),
        "warnBg": .init(0x3d3524),
        "xferCancel": .init(0x6b7684),
        "xferDone": .init(0x3fb950),
        "xferFail": .init(0xf06a70),
        "xferFill": .init(0x3a4250),
    ]

    // -- token 访问器：调用点一律经 Theme.<name>，值随 mode 即时解析 --
    public static var accentBgColor: RGBA { c("accentBgColor") }
    public static var accentColor: RGBA { c("accentColor") }
    public static var accentFgColor: RGBA { c("accentFgColor") }
    public static var amber: RGBA { c("amber") }
    public static var avatarLocal: RGBA { c("avatarLocal") }
    public static var avatarRdp: RGBA { c("avatarRdp") }
    public static var avatarVnc: RGBA { c("avatarVnc") }
    public static var blue: RGBA { c("blue") }
    public static var bluePillBg: RGBA { c("bluePillBg") }
    public static var bluePillFg: RGBA { c("bluePillFg") }
    public static var btn: RGBA { c("btn") }
    public static var btnHover: RGBA { c("btnHover") }
    public static var cPink: RGBA { c("cPink") }
    public static var cPurple: RGBA { c("cPurple") }
    public static var card: RGBA { c("card") }
    public static var cardHover: RGBA { c("cardHover") }
    public static var chip: RGBA { c("chip") }
    public static var content: RGBA { c("content") }
    public static var dangerBg: RGBA { c("dangerBg") }
    public static var dotErr: RGBA { c("dotErr") }
    public static var dotOk: RGBA { c("dotOk") }
    public static var dotPend: RGBA { c("dotPend") }
    public static var dotShare: RGBA { c("dotShare") }
    public static var endOvl: RGBA { c("endOvl") }
    public static var errBg: RGBA { c("errBg") }
    public static var errLine: RGBA { c("errLine") }
    public static var focusGreen: RGBA { c("focusGreen") }
    public static var grad1: RGBA { c("grad1") }
    public static var grad2: RGBA { c("grad2") }
    public static var green: RGBA { c("green") }
    public static var greenPillBg: RGBA { c("greenPillBg") }
    public static var greenPillFg: RGBA { c("greenPillFg") }
    public static var hintAmberBg: RGBA { c("hintAmberBg") }
    public static var hintAmberFg: RGBA { c("hintAmberFg") }
    public static var hintAmberLine: RGBA { c("hintAmberLine") }
    public static var hintBlueBg: RGBA { c("hintBlueBg") }
    public static var hintBlueFg: RGBA { c("hintBlueFg") }
    public static var hintBlueLine: RGBA { c("hintBlueLine") }
    public static var hintRedBg: RGBA { c("hintRedBg") }
    public static var hintRedFg: RGBA { c("hintRedFg") }
    public static var hover: RGBA { c("hover") }
    public static var kbdBg: RGBA { c("kbdBg") }
    public static var line: RGBA { c("line") }
    public static var notifHover: RGBA { c("notifHover") }
    public static var notifUnread: RGBA { c("notifUnread") }
    public static var odActive: RGBA { c("odActive") }
    public static var odHover: RGBA { c("odHover") }
    public static var odHover2: RGBA { c("odHover2") }
    public static var odLine: RGBA { c("odLine") }
    public static var odSelected: RGBA { c("odSelected") }
    public static var odStrong: RGBA { c("odStrong") }
    public static var onAccent: RGBA { c("onAccent") }
    public static var page: RGBA { c("page") }
    public static var palScrim: RGBA { c("palScrim") }
    public static var palShadow: RGBA { c("palShadow") }
    public static var pending: RGBA { c("pending") }
    public static var pillErrBg: RGBA { c("pillErrBg") }
    public static var pillErrFg: RGBA { c("pillErrFg") }
    public static var pillRunBg: RGBA { c("pillRunBg") }
    public static var pillRunFg: RGBA { c("pillRunFg") }
    public static var pillWaitBg: RGBA { c("pillWaitBg") }
    public static var pillWaitFg: RGBA { c("pillWaitFg") }
    public static var primaryHover: RGBA { c("primaryHover") }
    public static var rdBanner: RGBA { c("rdBanner") }
    public static var rdBannerFg: RGBA { c("rdBannerFg") }
    public static var rdBg: RGBA { c("rdBg") }
    public static var rdCardLine: RGBA { c("rdCardLine") }
    public static var rdDimFg: RGBA { c("rdDimFg") }
    public static var rdFg: RGBA { c("rdFg") }
    public static var rdHint: RGBA { c("rdHint") }
    public static var rdLine: RGBA { c("rdLine") }
    public static var rdOvl: RGBA { c("rdOvl") }
    public static var rdOvl2: RGBA { c("rdOvl2") }
    public static var rdTagFg: RGBA { c("rdTagFg") }
    public static var red: RGBA { c("red") }
    public static var rowHover: RGBA { c("rowHover") }
    public static var rowLine: RGBA { c("rowLine") }
    public static var scrim: RGBA { c("scrim") }
    public static var scroll1: RGBA { c("scroll1") }
    public static var scroll2: RGBA { c("scroll2") }
    public static var sel: RGBA { c("sel") }
    public static var shadow: RGBA { c("shadow") }
    public static var shadowRaised: RGBA { c("shadowRaised") }
    public static var shareBannerBg: RGBA { c("shareBannerBg") }
    public static var shareBannerLine: RGBA { c("shareBannerLine") }
    public static var sidebar: RGBA { c("sidebar") }
    public static var sidebarAct: RGBA { c("sidebarAct") }
    public static var subtle: RGBA { c("subtle") }
    public static var tabTermBg: RGBA { c("tabTermBg") }
    public static var tabTermFg: RGBA { c("tabTermFg") }
    public static var tabTermLine: RGBA { c("tabTermLine") }
    public static var tableHead: RGBA { c("tableHead") }
    public static var termBg: RGBA { c("termBg") }
    public static var termDim: RGBA { c("termDim") }
    public static var termText: RGBA { c("termText") }
    public static var text: RGBA { c("text") }
    public static var text2: RGBA { c("text2") }
    public static var text3: RGBA { c("text3") }
    public static var titlebar: RGBA { c("titlebar") }
    public static var titlebarTab: RGBA { c("titlebarTab") }
    public static var titlebarText: RGBA { c("titlebarText") }
    public static var ubuntu: RGBA { c("ubuntu") }
    public static var warnBg: RGBA { c("warnBg") }
    public static var xferCancel: RGBA { c("xferCancel") }
    public static var xferDone: RGBA { c("xferDone") }
    public static var xferFail: RGBA { c("xferFail") }
    public static var xferFill: RGBA { c("xferFill") }

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

#if canImport(AppKit)
// ---- 主题热切换：layer/text 颜色 token 注册，Theme.reapply() 重算 ----
extension Theme {
    /// key: "bg"|"border"|"text"；layer/textfield → token 名。
    static let reg =
        NSMapTable<AnyObject, NSMutableDictionary>.weakToStrongObjects()
    private static var resolvers:
        NSMapTable<AnyObject, NSMapTable<NSString, AnyObject>>
        = .weakToStrongObjects()

    static func bindLayer(_ l: AnyObject, _ key: String, _ token: String?) {
        let d = reg.object(forKey: l)
            ?? NSMutableDictionary(); reg.setObject(d, forKey: l)
        if let token { d[key] = token } else { d.removeObject(forKey: key) }
        applyOne(l, key)
    }
    static func bindResolver(_ o: AnyObject, _ key: String,
                             _ f: (() -> CGColor?)?) {
        let m = resolvers.object(forKey: o)
            ?? NSMapTable<NSString, AnyObject>(keyOptions: .strongMemory,
                                               valueOptions: .strongMemory)
        resolvers.setObject(m, forKey: o)
        if let f { m.setObject(f as AnyObject, forKey: key as NSString) }
        else { m.removeObject(forKey: key as NSString) }
        applyOne(o, key)
    }
    private static func applyOne(_ o: AnyObject, _ key: String) {
        let tok = reg.object(forKey: o)?[key] as? String
        if let f = o as? NSTextField, key == "text" {
            if let tok { f.textColor = c(tok).color }
            return
        }
        let dyn = resolvers.object(forKey: o)?
            .object(forKey: key as NSString) as? () -> CGColor?
        guard let l = o as? CALayer else { return }
        let c = dyn?() ?? tok.map { Theme.c($0).color.cgColor }
        switch key {
        case "bg":     l.backgroundColor = c
        case "border": l.borderColor = c
        default:       break
        }
    }
    /// 主题切换后重算所有注册项；由 MainWindow 在 mode 变更后调用。
    public static func reapply() {
        let e = reg.keyEnumerator().allObjects as [AnyObject]
        for o in e {
            guard let d = reg.object(forKey: o) else { continue }
            for k in d.allKeys.compactMap({ $0 as? String }) {
                applyOne(o, k)
            }
        }
        let e2 = resolvers.keyEnumerator().allObjects as [AnyObject]
        for o in e2 {
            guard let m = resolvers.object(forKey: o) else { continue }
            for k in m.keyEnumerator().allObjects
                .compactMap({ $0 as? NSString }) {
                applyOne(o, k as String)
            }
        }
    }
}

extension CALayer {
    /// 绑定 token 名的背景色；主题切换自动重算。赋值即生效。
    public var hbBg: String? {
        get { Theme.reg.object(forKey: self)?["bg"] as? String }
        set { Theme.bindLayer(self, "bg", newValue) }
    }
    public var hbBorder: String? {
        get { Theme.reg.object(forKey: self)?["border"] as? String }
        set { Theme.bindLayer(self, "border", newValue) }
    }
    /// 动态背景（表达式含状态）：resolver 立即求值并在切换时重算。
    public var hbBgResolver: (() -> CGColor?)? {
        get { nil }
        set { Theme.bindResolver(self, "bg", newValue) }
    }
    public var hbBorderResolver: (() -> CGColor?)? {
        get { nil }
        set { Theme.bindResolver(self, "border", newValue) }
    }
}
extension NSTextField {
    /// 绑定 token 名的文字色；主题切换自动重算。
    public var hbText: String? {
        get { Theme.reg.object(forKey: self)?["text"] as? String }
        set { Theme.bindLayer(self, "text", newValue) }
    }
}
#endif
