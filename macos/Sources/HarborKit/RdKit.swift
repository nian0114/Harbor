import CoreGraphics
import Foundation

/// 远程桌面纯逻辑（对齐 linux hb-desktop-page.c 的 helper）——
/// 无 UI 依赖，供页面与测试复用。
public enum RdKit {

    // ---------- 帧缓冲像素格式 ----------
    /// core fb 是 32 位 BGRX（内存字节 B,G,R,X = LE 字 0xXXRRGGBB，
    /// core/include/harbor/rd.h）：X 在最高字节，跳过第一个分量。
    /// 注意 noneSkipLast 会把字节3（X=0）读作 R——红通道全丢、白窗变青。
    public static let fbBitmapInfo: UInt32 =
        CGImageAlphaInfo.noneSkipFirst.rawValue
        | CGBitmapInfo.byteOrder32Little.rawValue

    // ---------- 画质档位（hb_rd_quality：0 fast 1 balanced 2 high） ----------
    public static func qualityOf(_ s: String) -> Int {
        s == "fast" ? 0 : s == "high" ? 2 : 1
    }
    public static func qualityStr(_ q: Int) -> String {
        q == 0 ? "fast" : q == 2 ? "high" : "balanced"
    }
    public static func qualityLabel(_ q: Int) -> String {
        q == 0 ? L("流畅") : q == 2 ? L("高清") : L("均衡")
    }

    // ---------- 画布几何（linux canvas_draw_rect / rd_canvas_map） ----------
    /// fit 模式下图像在控件内的目标矩形；orig=原始大小 → 1:1 居中
    ///（macOS 画布尺寸 = max(fb, 可视区)，linux 里由 CenterBox 居中）。
    public static func drawRect(availW: Double, availH: Double,
                                fbW: Int, fbH: Int,
                                orig: Bool) -> (x: Double, y: Double,
                                                w: Double, h: Double) {
        if orig || fbW <= 0 || fbH <= 0 {
            return (max(0, (availW - Double(fbW)) / 2),
                    max(0, (availH - Double(fbH)) / 2),
                    Double(fbW), Double(fbH))
        }
        let sx = availW / Double(fbW), sy = availH / Double(fbH)
        var s = sx < sy ? sx : sy
        if s <= 0 { s = 1 }
        let w = Double(fbW) * s, h = Double(fbH) * s
        return ((availW - w) / 2, (availH - h) / 2, w, h)
    }
    /// 控件坐标 → 帧缓冲坐标；图外（letterbox）返回 nil。
    public static func mapPoint(x: Double, y: Double,
                                rect r: (x: Double, y: Double,
                                         w: Double, h: Double),
                                fbW: Int, fbH: Int) -> (fx: Double, fy: Double)? {
        if r.w <= 0 || r.h <= 0 ||
           x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h {
            return nil
        }
        return ((x - r.x) / r.w * Double(fbW),
                (y - r.y) / r.h * Double(fbH))
    }

    /// 回环地址（linux loopback_addr）——经主机与目标同址时 dest=127.0.0.1。
    public static func isLoopback(_ a: String) -> Bool {
        a.isEmpty || a.caseInsensitiveCompare("localhost") == .orderedSame
            || a == "127.0.0.1" || a == "::1"
    }

    // ---------- 鼠标（hb_rd_pointer 按钮位：0左 1中 2右 3/4滚轮 5/6横滚） ----------
    public static func buttonBit(_ buttonNumber: Int) -> UInt32 {
        switch buttonNumber {
        case 0: return 1     // left
        case 1: return 4     // right
        case 2: return 2     // middle
        default: return 0
        }
    }

    // ---------- 键盘：macVK → (X11/evdev keycode, keysym) ----------
    /// X11 keycode = evdev code + 8；core 的 RDP 路径按 x11kc 查表→scancode，
    /// VNC 路径发 keysym。字符键 keysym 为 0 → 调用方用产生字符的码点。
    public struct KeyInfo: Equatable {
        public let x11kc: UInt32
        public let keysym: UInt32
        public init(_ kc: UInt32, _ sym: UInt32) {
            x11kc = kc; keysym = sym
        }
    }

    // X11 keysyms
    public static let SYM_BACKSPACE: UInt32 = 0xFF08
    public static let SYM_TAB: UInt32 = 0xFF09
    public static let SYM_RETURN: UInt32 = 0xFF0D
    public static let SYM_ESCAPE: UInt32 = 0xFF1B
    public static let SYM_DELETE: UInt32 = 0xFFFF
    public static let SYM_HOME: UInt32 = 0xFF50
    public static let SYM_LEFT: UInt32 = 0xFF51
    public static let SYM_UP: UInt32 = 0xFF52
    public static let SYM_RIGHT: UInt32 = 0xFF53
    public static let SYM_DOWN: UInt32 = 0xFF54
    public static let SYM_PAGEUP: UInt32 = 0xFF55
    public static let SYM_PAGEDOWN: UInt32 = 0xFF56
    public static let SYM_END: UInt32 = 0xFF57
    public static let SYM_INSERT: UInt32 = 0xFF63
    public static let SYM_NUMLOCK: UInt32 = 0xFF7F
    public static let SYM_KP_ENTER: UInt32 = 0xFF8D
    public static let SYM_KP_MULT: UInt32 = 0xFFAA
    public static let SYM_KP_ADD: UInt32 = 0xFFAB
    public static let SYM_KP_SUB: UInt32 = 0xFFAD
    public static let SYM_KP_DEC: UInt32 = 0xFFAE
    public static let SYM_KP_DIV: UInt32 = 0xFFAF
    public static let SYM_KP_EQUAL: UInt32 = 0xFFBD
    public static let SYM_SHIFT_L: UInt32 = 0xFFE1
    public static let SYM_SHIFT_R: UInt32 = 0xFFE2
    public static let SYM_CTRL_L: UInt32 = 0xFFE3
    public static let SYM_CTRL_R: UInt32 = 0xFFE4
    public static let SYM_CAPSLOCK: UInt32 = 0xFFE5
    public static let SYM_ALT_L: UInt32 = 0xFFE9
    public static let SYM_ALT_R: UInt32 = 0xFFEA
    public static let SYM_SUPER_L: UInt32 = 0xFFEB
    public static let SYM_SUPER_R: UInt32 = 0xFFEC
    public static func symF(_ n: UInt32) -> UInt32 { 0xFFBE + n - 1 }   // F1..F12
    public static func symKP(_ d: UInt32) -> UInt32 { 0xFFB0 + d }      // KP 0..9

    /// macVK → (x11kc, keysym)。x11kc=0 表示不认识的键。
    /// keysym=0 → 字符键，用 NSEvent 产生字符的码点作 keysym。
    public static func keyInfo(_ vk: UInt16) -> KeyInfo? {
        if let k = vkTab[vk] { return k }
        return nil
    }

    /// Ctrl+Alt+Del 序列（linux send_cad：x11kc 37/64/119）。
    public static let cad: [(keysym: UInt32, kc: UInt32)] = [
        (0xFFE3, 37), (0xFFE9, 64), (0xFFFF, 119),
    ]
    /// Win/Super 键（linux send_win：x11kc 133）。
    public static let winKey: (keysym: UInt32, kc: UInt32) = (0xFFEB, 133)

    // swiftlint:disable:next large_tuple — 静态查找表
    private static let vkTab: [UInt16: KeyInfo] = {
        // macVK → evdev code（x11kc = evdev+8），keysym 0 = 取字符码点
        let ev: [UInt16: (evdev: UInt32, sym: UInt32)] = [
            // 字母
            0x00: (30, 0), 0x01: (31, 0), 0x02: (32, 0), 0x03: (33, 0),
            0x04: (35, 0), 0x05: (34, 0), 0x06: (44, 0), 0x07: (45, 0),
            0x08: (46, 0), 0x09: (47, 0), 0x0B: (48, 0), 0x0C: (16, 0),
            0x0D: (17, 0), 0x0E: (18, 0), 0x0F: (19, 0), 0x10: (21, 0),
            0x11: (20, 0), 0x1F: (24, 0), 0x20: (22, 0), 0x22: (23, 0),
            0x23: (25, 0), 0x25: (38, 0), 0x26: (36, 0), 0x28: (37, 0),
            0x2D: (49, 0), 0x2E: (50, 0),
            // 数字/符号行（字符键 → keysym 取字符）
            0x12: (2, 0), 0x13: (3, 0), 0x14: (4, 0), 0x15: (5, 0),
            0x17: (6, 0), 0x16: (7, 0), 0x1A: (8, 0), 0x1C: (9, 0),
            0x19: (10, 0), 0x1D: (11, 0), 0x1B: (12, 0), 0x18: (13, 0),
            0x27: (40, 0), 0x29: (39, 0), 0x2B: (51, 0), 0x2F: (52, 0),
            0x2C: (53, 0), 0x2A: (43, 0), 0x32: (41, 0), 0x21: (26, 0),
            0x1E: (27, 0), 0x31: (57, 0x20),
            // 功能键
            0x33: (14, 0xFF08),   // Backspace
            0x30: (15, 0xFF09),   // Tab
            0x24: (28, 0xFF0D),   // Return
            0x35: (1, 0xFF1B),    // Escape
            0x75: (111, 0xFFFF),  // Forward Delete
            0x73: (102, 0xFF50),  // Home
            0x77: (107, 0xFF57),  // End
            0x74: (104, 0xFF55),  // PageUp
            0x79: (109, 0xFF56),  // PageDown
            0x72: (110, 0xFF63),  // Insert (Help)
            0x7B: (105, 0xFF51),  // Left
            0x7E: (103, 0xFF52),  // Up
            0x7C: (106, 0xFF53),  // Right
            0x7D: (108, 0xFF54),  // Down
            0x7A: (59, 0xFFBE),   // F1
            0x78: (60, 0xFFBF), 0x63: (61, 0xFFC0), 0x76: (62, 0xFFC1),
            0x60: (63, 0xFFC2), 0x61: (64, 0xFFC3), 0x62: (65, 0xFFC4),
            0x64: (66, 0xFFC5), 0x65: (67, 0xFFC6), 0x6D: (68, 0xFFC7),
            0x67: (87, 0xFFC8), 0x6F: (88, 0xFFC9),   // F11, F12
            // 修饰键
            0x38: (42, 0xFFE1),   // LShift
            0x3C: (54, 0xFFE2),   // RShift
            0x3B: (29, 0xFFE3),   // LControl
            0x3E: (97, 0xFFE4),   // RControl
            0x3A: (56, 0xFFE9),   // LOption (Alt_L)
            0x3D: (100, 0xFFEA),  // ROption (Alt_R)
            0x37: (125, 0xFFEB),  // LCommand → Super_L
            0x36: (126, 0xFFEC),  // RCommand → Super_R
            0x39: (58, 0xFFE5),   // CapsLock
            // 小键盘
            0x52: (82, 0xFFB0), 0x53: (79, 0xFFB1), 0x54: (80, 0xFFB2),
            0x55: (81, 0xFFB3), 0x56: (75, 0xFFB4), 0x57: (76, 0xFFB5),
            0x58: (77, 0xFFB6), 0x59: (71, 0xFFB7), 0x5B: (72, 0xFFB8),
            0x5C: (73, 0xFFB9), 0x41: (83, 0xFFAE),   // KP .
            0x43: (55, 0xFFAA), 0x45: (78, 0xFFAB),   // KP * +
            0x4E: (74, 0xFFAD), 0x4B: (98, 0xFFAF),   // KP - /
            0x4C: (96, 0xFF8D), 0x51: (117, 0xFFBD),  // KP Enter, KP =
            0x47: (69, 0xFF7F),                       // Clear → NumLock
        ]
        var t: [UInt16: KeyInfo] = [:]
        for (vk, e) in ev { t[vk] = KeyInfo(e.evdev + 8, e.sym) }
        return t
    }()

    /// 字符 → X11 keysym：Latin-1（≤0xFF）恒等，其余 BMP 加
    /// 0x01000000 偏移（XConvertCase/keysymdef 规则）；控制字符不发。
    public static func charKeysym(_ uni: UInt32) -> UInt32 {
        if uni < 0x20 || uni > 0xFFFF { return 0 }
        return uni <= 0xFF ? uni : 0x01000000 | uni
    }

    /// 平滑滚轮累积 → 本次应发的整格数与剩余（linux in_scroll）。
    /// 返回 (steps, rest)；dy<0 为向下滚动。
    public static func scrollSteps(acc: Double, delta: Double,
                                   unit: Double) -> (steps: Int,
                                                     rest: Double) {
        var a = acc + delta
        let n = Int(a / unit)
        a -= Double(n) * unit
        return (n, a)
    }
    /// 滚轮按钮位：bit3 上 bit4 下 bit5 左 bit6 右（hb_rd_pointer）。
    public static func wheelMask(dy: Int) -> UInt32 {
        dy < 0 ? 1 << 4 : 1 << 3
    }
    public static func hwheelMask(dx: Int) -> UInt32 {
        dx < 0 ? 1 << 6 : 1 << 5
    }
}
