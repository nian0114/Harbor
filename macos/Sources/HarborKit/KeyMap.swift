import Foundation

/// NSEvent → hb_key/hb_mod 映射（纯逻辑，可测试）。
/// HB_KEY_* 与 HB_MOD_* 的值与 core vt.h 一致。
public enum KeyMap {
    public static let HB_KEY_NONE: Int32 = 0
    public static let HB_KEY_ENTER: Int32 = 1
    public static let HB_KEY_TAB: Int32 = 2
    public static let HB_KEY_BACKSPACE: Int32 = 3
    public static let HB_KEY_ESCAPE: Int32 = 4
    public static let HB_KEY_UP: Int32 = 5
    public static let HB_KEY_DOWN: Int32 = 6
    public static let HB_KEY_LEFT: Int32 = 7
    public static let HB_KEY_RIGHT: Int32 = 8
    public static let HB_KEY_INS: Int32 = 9
    public static let HB_KEY_DEL: Int32 = 10
    public static let HB_KEY_HOME: Int32 = 11
    public static let HB_KEY_END: Int32 = 12
    public static let HB_KEY_PAGEUP: Int32 = 13
    public static let HB_KEY_PAGEDOWN: Int32 = 14
    public static func fkey(_ n: Int32) -> Int32 { 100 + n - 1 }
    public static let HB_MOD_SHIFT: Int32 = 1
    public static let HB_MOD_ALT: Int32 = 2
    public static let HB_MOD_CTRL: Int32 = 4

    /// AppKit keyCode → hb_key。返回 nil = 走 unichar 路径。
    public static func specialKey(_ keyCode: UInt16) -> Int32? {
        switch keyCode {
        case 36, 76: return HB_KEY_ENTER          // Return / keypad Enter
        case 48:     return HB_KEY_TAB
        case 51:     return HB_KEY_BACKSPACE
        case 53:     return HB_KEY_ESCAPE
        case 126:    return HB_KEY_UP
        case 125:    return HB_KEY_DOWN
        case 123:    return HB_KEY_LEFT
        case 124:    return HB_KEY_RIGHT
        case 114:    return HB_KEY_INS            // Help
        case 117:    return HB_KEY_DEL            // forward delete
        case 115:    return HB_KEY_HOME
        case 119:    return HB_KEY_END
        case 116:    return HB_KEY_PAGEUP
        case 121:    return HB_KEY_PAGEDOWN
        case 122:    return fkey(1)
        case 120:    return fkey(2)
        case 99:     return fkey(3)
        case 118:    return fkey(4)
        case 96:     return fkey(5)
        case 97:     return fkey(6)
        case 98:     return fkey(7)
        case 100:    return fkey(8)
        case 101:    return fkey(9)
        case 109:    return fkey(10)
        case 103:    return fkey(11)
        case 111:    return fkey(12)
        case 105:    return fkey(13)              // F13 → 忽略（hb 只到 F12）
        default:     return nil
        }
    }

    /// NSEvent.ModifierFlags → HB_MOD_*。⌥asMeta=false 时 ⌥ 不算 alt
    /// （让系统继续组字），由调用方决定。
    public static func mods(_ flags: UInt, optionIsMeta: Bool) -> Int32 {
        var m: Int32 = 0
        if flags & (1 << 17) != 0 { m |= HB_MOD_SHIFT }   // .shift
        if flags & (1 << 18) != 0 { m |= HB_MOD_CTRL }    // .control
        if flags & (1 << 19) != 0 && optionIsMeta { m |= HB_MOD_ALT } // .option
        return m
    }
}
