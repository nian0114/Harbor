import Foundation

/// 终端配色 —— 与 linux/src/hb-term-theme.c THEMES[] 同源。
public struct TermTheme {
    public let id: String
    public let name: String
    public let bg: Theme.RGBA
    public let fg: Theme.RGBA
    public let cursor: Theme.RGBA
    public let dim: Theme.RGBA
    public let selection: Theme.RGBA
    public let find: Theme.RGBA     // 全部命中（琥珀半透明）
    public let findCur: Theme.RGBA  // 当前命中（更深）
    // 终端表面 chrome（find bar 等，对齐 .find-bar 的 --t-* 变量）
    public let bar: Theme.RGBA
    public let line: Theme.RGBA
    public let border: Theme.RGBA
    public let title: Theme.RGBA
    public let muted: Theme.RGBA
    public let ansi: [Theme.RGBA]   // 16 色；≥16 走 256 板

    public func color(_ type: UInt8, idx: UInt8,
                      r: UInt8, g: UInt8, b: UInt8,
                      def: Theme.RGBA) -> Theme.RGBA {
        switch type {
        case 0, 1: return def                    // DEFAULT_FG/BG
        case 2:                                    // INDEXED
            let i = Int(idx)
            if i < 16 { return ansi[i] }
            return Theme.palette256[i]
        default: return Theme.RGBA(r: Double(r)/255,
                                   g: Double(g)/255, b: Double(b)/255)
        }
    }
    public static func `for`(_ id: String?) -> TermTheme {
        all.first { $0.id == id } ?? all[0]
    }

    public static let all: [TermTheme] = [
        TermTheme(id: "harbor", name: "Harbor Dark",
            bg: .init(0x0a0f1e), fg: .init(0xdbe4f5),
            cursor: .init(0x3fd97f), dim: .init(0x8b96ad),
            selection: .init(0x3fd97f, 0.30),
            find: .init(0xe8c66a, 0.35), findCur: .init(0xe8c66a, 0.75),
            bar: .init(0x0d1426), line: .init(0x1a2338),
            border: .init(0x242f4a), title: .init(0x9fb0cc),
            muted: .init(0x64718c),
            ansi: [0x1a2338,0xff7b72,0x3fd97f,0xe8c66a,0x6cb8ff,0xc792ea,
                   0x56c8e8,0xdbe4f5,0x64718c,0xff9b94,0x6ee6a0,0xf2d88c,
                   0x93ccff,0xd6a8f0,0x84d8f0,0xffffff].map { .init($0) }),
        TermTheme(id: "ubuntu", name: "Ubuntu",
            bg: .init(0x300a24), fg: .init(0xeeeeec),
            cursor: .init(0x8ae234), dim: .init(0xc0a3b8),
            selection: .init(0x8ae234, 0.30),
            find: .init(0xfce94f, 0.30), findCur: .init(0xfce94f, 0.70),
            bar: .init(0x3d1030), line: .init(0x4d1a3f),
            border: .init(0x632653), title: .init(0xd8c2d2),
            muted: .init(0xa98ba0),
            ansi: [0x2e3436,0xcc0000,0x4e9a06,0xc4a000,0x3465a4,0x75507b,
                   0x06989a,0xd3d7cf,0x555753,0xef2929,0x8ae234,0xfce94f,
                   0x729fcf,0xad7fa8,0x34e2e2,0xeeeeec].map { .init($0) }),
        TermTheme(id: "solarized", name: "Solarized Dark",
            bg: .init(0x002b36), fg: .init(0x93a1a1),
            cursor: .init(0x859900), dim: .init(0x657b83),
            selection: .init(0x859900, 0.30),
            find: .init(0xb58900, 0.45), findCur: .init(0xb58900, 0.85),
            bar: .init(0x073642), line: .init(0x0a4552),
            border: .init(0x11525f), title: .init(0x839496),
            muted: .init(0x586e75),
            ansi: [0x073642,0xdc322f,0x859900,0xb58900,0x268bd2,0xd33682,
                   0x2aa198,0xeee8d5,0x002b36,0xcb4b16,0x586e75,0x657b83,
                   0x839496,0x6c71c4,0x93a1a1,0xfdf6e3].map { .init($0) }),
        TermTheme(id: "light", name: "Harbor Light",
            bg: .init(0xf7f9fb), fg: .init(0x1c2333),
            cursor: .init(0x1a7f4b), dim: .init(0x8a93a3),
            selection: .init(0x2f7cf6, 0.25),
            find: .init(0xe8a33d, 0.40), findCur: .init(0xd97706, 0.80),
            bar: .init(0xeef1f5), line: .init(0xe2e7ee),
            border: .init(0xd4dae3), title: .init(0x5b6472),
            muted: .init(0x9aa3b0),
            ansi: [0x1c2333,0xd1242f,0x1a7f4b,0x9a6700,0x0969da,0x8250df,
                   0x1b7c83,0x6e7781,0x57606a,0xe5484d,0x22a06b,0xbf8700,
                   0x2f7cf6,0xa475f9,0x3192aa,0x8c959f].map { .init($0) }),
    ]
}
