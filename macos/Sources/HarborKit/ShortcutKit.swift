import Foundation

/// 快捷键表（对齐 linux hb-shortcuts.c 的语义；按键为 macOS 默认）。
/// 单一表：菜单 keyEquivalent 与设置页都从这里取；
/// 覆盖存 settings.shortcuts_macos strv "id@idx=accel"（与 linux 的
/// GTK accel 串不同源，故用独立键名；随 E2EE 同步到其它 Mac）。
public struct ShortcutRow {
    public let id: String
    public var key: String      // 规范化键名："k","tab","left","f5","]"
    public var mods: UInt       // SC_M_*
    public let label: String
    public init(_ id: String, _ key: String, _ mods: UInt,
                _ label: String) {
        self.id = id; self.key = key; self.mods = mods
        self.label = label
    }
}

public let SC_M_CMD: UInt = 1
public let SC_M_SHIFT: UInt = 2
public let SC_M_OPT: UInt = 4
public let SC_M_CTRL: UInt = 8

public enum ShortcutTable {
    /// 默认表。tab-n（⌘1…9）家族固定不可改（同 linux）。每 id 的首行
    /// 是菜单展示绑定；其余行走隐藏菜单项，同样可改。
    public static let rows: [ShortcutRow] = [
        ShortcutRow("palette",    "k",     SC_M_CMD, L("命令面板")),
        ShortcutRow("find",       "f",     SC_M_CMD, L("在终端中查找…")),
        ShortcutRow("new-local",  "t",     SC_M_CMD, L("新建本地标签页")),
        ShortcutRow("close-tab",  "w",     SC_M_CMD, L("关闭标签页")),
        ShortcutRow("close-pane", "w",     SC_M_CMD|SC_M_SHIFT, L("关闭当前 Pane")),
        ShortcutRow("new-workspace", "w",  SC_M_CMD|SC_M_OPT, L("新建 Workspace…")),
        ShortcutRow("tab-next",   "]",     SC_M_CMD|SC_M_SHIFT, L("下一个标签页")),
        ShortcutRow("tab-prev",   "[",     SC_M_CMD|SC_M_SHIFT, L("上一个标签页")),
        ShortcutRow("tab-next",   "tab",   SC_M_CTRL, L("下一个标签页")),
        ShortcutRow("tab-prev",   "tab",   SC_M_CTRL|SC_M_SHIFT, L("上一个标签页")),
        ShortcutRow("split-right","d",     SC_M_CMD|SC_M_SHIFT, L("向右分屏")),
        ShortcutRow("split-down", "e",     SC_M_CMD|SC_M_SHIFT, L("向下分屏")),
        ShortcutRow("broadcast",  "b",     SC_M_CMD|SC_M_SHIFT, L("输入广播")),
        ShortcutRow("focus-mode", "m",     SC_M_CMD|SC_M_SHIFT, L("Focus 模式")),
        ShortcutRow("pane-left",  "left",  SC_M_CMD|SC_M_OPT, L("焦点移到左侧 Pane")),
        ShortcutRow("pane-right", "right", SC_M_CMD|SC_M_OPT, L("焦点移到右侧 Pane")),
        ShortcutRow("pane-up",    "up",    SC_M_CMD|SC_M_OPT, L("焦点移到上方 Pane")),
        ShortcutRow("pane-down",  "down",  SC_M_CMD|SC_M_OPT, L("焦点移到下方 Pane")),
        ShortcutRow("font-plus",  "=",     SC_M_CMD, L("放大字体")),
        ShortcutRow("font-minus", "-",     SC_M_CMD, L("缩小字体")),
        ShortcutRow("font-reset", "0",     SC_M_CMD, L("实际大小")),
        ShortcutRow("tab-n",      "1",     SC_M_CMD, L("跳到标签 N（1=Vault）")),
    ]

    public static func editable(_ idx: Int) -> Bool {
        idx >= 0 && idx < rows.count && rows[idx].id != "tab-n"
    }

    // ---- accel 串 "cmd+shift+k"；"" = 禁用 ----

    static let modNames: [(String, UInt)] =
        [("cmd", SC_M_CMD), ("shift", SC_M_SHIFT),
         ("opt", SC_M_OPT), ("ctrl", SC_M_CTRL)]

    /// 解析失败 → nil；"" → (key:"", mods:0) 合法禁用。
    public static func parse(_ accel: String) -> (key: String, mods: UInt)? {
        if accel.isEmpty { return ("", 0) }
        var mods: UInt = 0
        var key = ""
        for part in accel.split(separator: "+").map(String.init) {
            if let m = modNames.first(where: { $0.0 == part })?.1 {
                mods |= m
            } else if key.isEmpty, keyName(part) != nil {
                key = keyName(part)!
            } else {
                return nil
            }
        }
        return key.isEmpty ? nil : (key, mods)
    }

    public static func accel(key: String, mods: UInt) -> String {
        var s = ""
        for (n, m) in modNames where mods & m != 0 { s += n + "+" }
        return s + key
    }

    /// 规范化键名（小写）。keyCode → 名 的映射在捕获侧做。
    public static func keyName(_ k: String) -> String? {
        let n = k.lowercased()
        if n.count == 1, let c = n.first,
           c.isLetter || c.isNumber ||
           "`-=[]\\;,.'/".contains(c) { return n }
        let named: Set<String> = [
            "tab","space","return","delete","forwarddelete","escape",
            "left","right","up","down","home","end","pageup","pagedown",
        ]
        if named.contains(n) { return n }
        if n.hasPrefix("f"), let v = Int(n.dropFirst()),
           v >= 1 && v <= 20 { return n }
        return nil
    }

    /// 展示标签 "⌘⇧K"（⌃⌥⇧⌘ 顺序按 Apple 惯例）。
    public static func label(key: String, mods: UInt) -> String {
        if key.isEmpty { return L("已禁用") }
        var s = ""
        if mods & SC_M_CTRL != 0 { s += "⌃" }
        if mods & SC_M_OPT != 0  { s += "⌥" }
        if mods & SC_M_SHIFT != 0 { s += "⇧" }
        if mods & SC_M_CMD != 0  { s += "⌘" }
        let glyphs: [String: String] = [
            "tab": "⇥", "space": "␣", "return": "↩", "delete": "⌫",
            "forwarddelete": "⌦", "escape": "⎋", "left": "←",
            "right": "→", "up": "↑", "down": "↓", "home": "↖",
            "end": "↘", "pageup": "⇞", "pagedown": "⇟",
            "-": "-", "=": "=", "[": "[", "]": "]",
        ]
        s += glyphs[key] ?? key.uppercased()
        return s
    }

    // ---- 覆盖表（"id@idx=accel" strv）----

    /// 默认表 + 覆盖 → 有效表。非法/脏覆盖忽略回落默认。
    public static func effective(_ overrides: [String]) -> [ShortcutRow] {
        var t = rows
        for (i, r) in rows.enumerated() {
            let p = "\(r.id)@\(i)="
            guard let o = overrides.first(where: { $0.hasPrefix(p) })
            else { continue }
            if let (k, m) = parse(String(o.dropFirst(p.count))) {
                t[i].key = k; t[i].mods = m
            }
        }
        return t
    }

    /// 同 (key,mods) 的另一行号（跳过 skip）；无 → nil；禁用行不冲突。
    public static func conflict(_ eff: [ShortcutRow], skip: Int,
                                key: String, mods: UInt) -> Int? {
        guard !key.isEmpty else { return nil }
        return eff.indices.first {
            $0 != skip && eff[$0].key == key && eff[$0].mods == mods
        }
    }

    /// 写入/删除覆盖：accel==nil → 恢复默认（删条目）。
    public static func setOverride(_ overrides: inout [String],
                                   idx: Int, accel: String?) {
        guard idx >= 0 && idx < rows.count else { return }
        let p = "\(rows[idx].id)@\(idx)="
        overrides.removeAll { $0.hasPrefix(p) }
        if let accel { overrides.append(p + accel) }
    }

    /// 终端风险提示：无修饰键（吞普通输入）或纯 ⌃+字母（shell 控制键）。
    public static func terminalRisk(key: String, mods: UInt) -> Bool {
        if mods == 0 { return true }
        if mods == SC_M_CTRL, key.count == 1,
           key.first!.isLetter { return true }
        return false
    }
}
