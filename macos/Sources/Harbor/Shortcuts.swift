import AppKit
import HarborKit

/// 快捷键单例：ShortcutTable 默认 + settings.shortcuts_macos 覆盖
/// → 有效表；改后即时刷菜单 keyEquivalent（linux 同：dispatcher
/// 每次按键现算）。
final class ShortcutKit {
    static let shared = ShortcutKit()
    private(set) var rows: [ShortcutRow] = ShortcutTable.rows
    /// 设置页捕获回调（按下/取消/禁用后的提示文案）。
    var onToast: ((String) -> Void)?

    func reload() {
        rows = ShortcutTable.effective(
            WatchSettings.strv("shortcuts_macos"))
        applyToMenus()
    }

    // ---- 覆盖读写 ----

    private func store(_ strv: [String]) {
        WatchSettings.save("shortcuts_macos", strv: strv)
        reload()
    }
    /// accel==nil → 恢复默认；"" → 禁用。冲突行自动解绑（同 linux）。
    func apply(_ idx: Int, accel: String?) {
        var ov = WatchSettings.strv("shortcuts_macos")
        ShortcutTable.setOverride(&ov, idx: idx, accel: accel)
        store(ov)
    }
    func resetAll() {
        WatchSettings.save("shortcuts_macos", strv: [])
        reload()
    }
    /// 提交新绑定：冲突 → 解绑对方；终端风险键 → 警告但仍应用。
    func commit(_ idx: Int, accel: String) {
        guard let p = ShortcutTable.parse(accel) else {
            onToast?(L("无效的快捷键")); return
        }
        if let conf = ShortcutTable.conflict(rows, skip: idx,
                                             key: p.key, mods: p.mods) {
            apply(conf, accel: "")
            onToast?(String(format: L("与「%@」冲突，已解绑对方"),
                            rows[conf].label))
        }
        if !p.key.isEmpty,
           ShortcutTable.terminalRisk(key: p.key, mods: p.mods) {
            onToast?(L("警告：该键在终端里可能是 shell 控制键或普通输入"))
        }
        apply(idx, accel: accel)
    }

    // ---- 菜单绑定 ----

    /// 菜单项 identifier → 行 id（首行展示绑定）。非菜单行（如
    /// ⌃Tab 次绑定）用隐藏菜单项承接。
    static let menuIds: [String: String] = [
        "palette": "palette", "find": "find", "new-local-tab": "new-local",
        "close-tab": "close-tab", "close-pane": "close-pane",
        "new-workspace": "new-workspace", "next-tab": "tab-next",
        "prev-tab": "tab-prev", "split-right": "split-right",
        "split-down": "split-down", "broadcast": "broadcast",
        "focus-mode": "focus-mode", "pane-left": "pane-left",
        "pane-right": "pane-right", "pane-up": "pane-up",
        "pane-down": "pane-down", "font-plus": "font-plus",
        "font-minus": "font-minus", "font-reset": "font-reset",
    ]

    /// key/mods → NSMenuItem keyEquivalent（unicode 功能键映射）。
    static func keyEquiv(key: String, mods: UInt)
        -> (String, NSEvent.ModifierFlags)? {
        guard !key.isEmpty else { return nil }
        let special: [String: String] = [
            "tab": "\t", "space": " ", "return": "\r",
            "delete": "\u{8}", "forwarddelete": "\u{f728}",
            "escape": "\u{1b}", "left": "\u{f702}", "right": "\u{f703}",
            "up": "\u{f700}", "down": "\u{f701}", "home": "\u{f729}",
            "end": "\u{f72b}", "pageup": "\u{f72c}", "pagedown": "\u{f72d}",
        ]
        var ke = special[key]
        if ke == nil, key.hasPrefix("f"), let v = Int(key.dropFirst()),
           (1...20).contains(v) {
            ke = String(UnicodeScalar(0xF703 + v)!)  // NSF1FunctionKey=F704
        }
        let eq = ke ?? key
        var m: NSEvent.ModifierFlags = []
        if mods & SC_M_CMD != 0 { m.insert(.command) }
        if mods & SC_M_SHIFT != 0 { m.insert(.shift) }
        if mods & SC_M_OPT != 0 { m.insert(.option) }
        if mods & SC_M_CTRL != 0 { m.insert(.control) }
        return (eq, m)
    }

    /// 把有效表刷进主菜单：identifier 命中的项取该 id 首行绑定；
    /// 非首行（次绑定）同步到隐藏菜单项（"id@idx" identifier）。
    func applyToMenus() {
        guard let mm = NSApp.mainMenu else { return }
        let all = (mm.items).flatMap { $0.submenu?.items ?? [] }
        var firstIdx: [String: Int] = [:]
        for (i, r) in rows.enumerated() where firstIdx[r.id] == nil {
            firstIdx[r.id] = i
        }
        for it in all {
            guard let id = it.identifier?.rawValue,
                  let sid = Self.menuIds[id],
                  let fi = firstIdx[sid] else { continue }
            let r = rows[fi]
            if let (eq, m) = Self.keyEquiv(key: r.key, mods: r.mods) {
                it.keyEquivalent = eq
                it.keyEquivalentModifierMask = m
            } else {
                it.keyEquivalent = ""
            }
        }
        for (i, r) in rows.enumerated() where i != firstIdx[r.id] {
            let hid = "\(r.id)@\(i)"
            if let (eq, m) = Self.keyEquiv(key: r.key, mods: r.mods) {
                ensureHidden(mm, id: hid, eq: eq, mods: m)
            } else {
                removeHidden(mm, id: hid)
            }
        }
        // tab-n 固定行（⌘1…9）：不受覆盖，仍同步一次键位
        if let tabRows = firstIdx["tab-n"], rows.count > tabRows {
            for it in all where it.identifier?.rawValue
                .hasPrefix("tab-") == true {
                if let n = Int(it.identifier!.rawValue.dropFirst(4)),
                   n >= 1, n <= 9 {
                    it.keyEquivalent = "\(n)"
                    it.keyEquivalentModifierMask = .command
                }
            }
        }
    }

    /// 行号 → 菜单动作（非首行绑定的派发目标）。
    private func action(for id: String) -> Selector? {
        switch id {
        case "tab-next": return #selector(App.nextTab)
        case "tab-prev": return #selector(App.prevTab)
        case "palette": return #selector(App.openPalette)
        case "find":    return #selector(App.openFind)
        case "new-local": return #selector(App.newTab)
        case "close-tab": return #selector(App.closeTab)
        case "close-pane": return #selector(App.closePane)
        case "new-workspace": return #selector(App.openWorkspacePicker)
        case "split-right": return #selector(App.splitRight)
        case "split-down": return #selector(App.splitDown)
        case "broadcast": return #selector(App.toggleBroadcast)
        case "focus-mode": return #selector(App.toggleFocusMode)
        case "pane-left": return #selector(App.moveLeft)
        case "pane-right": return #selector(App.moveRight)
        case "pane-up": return #selector(App.moveUp)
        case "pane-down": return #selector(App.moveDown)
        case "font-plus": return #selector(App.fontPlus)
        case "font-minus": return #selector(App.fontMinus)
        case "font-reset": return #selector(App.fontReset)
        default: return nil
        }
    }
    private func ensureHidden(_ mm: NSMenu, id: String,
                              eq: String, mods: NSEvent.ModifierFlags) {
        let view = mm.items.first {
            $0.submenu?.items.contains {
                $0.identifier?.rawValue == "next-tab" } ?? false }
        guard let sub = view?.submenu, let act = action(for:
            String(id.split(separator: "@").first ?? "")) else { return }
        let it = sub.items.first { $0.identifier?.rawValue == id }
            ?? { let i = NSMenuItem(title: "", action: act,
                                    keyEquivalent: "")
                 i.isHidden = true
                 i.identifier = NSUserInterfaceItemIdentifier(id)
                 sub.addItem(i); return i }()
        it.keyEquivalent = eq
        it.keyEquivalentModifierMask = mods
    }
    private func removeHidden(_ mm: NSMenu, id: String) {
        for top in mm.items {
            top.submenu?.items
                .filter { $0.identifier?.rawValue == id }
                .forEach { top.submenu?.removeItem($0) }
        }
    }
}
