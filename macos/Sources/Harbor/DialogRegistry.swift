import AppKit
import HarborKit

/// 追踪打开的 sheet/alert，供 dev hook `dialogs`/`dialog-fill`/`dialog-press`。
/// 每个被 present 的 NSAlert 的 window 注册进此表直到 dismiss。
final class DialogRegistry {
    static let shared = DialogRegistry()
    private var tracked: [(win: NSWindow, kind: String)] = []
    private var fields: [String: NSControl] = [:]
    private var textViews: [String: NSTextView] = [:]
    /// 待决的 .terminateLater；sheet 完成回调/手动应答后清位，防二次 reply。
    var pendingTerminate = false

    func present(alert: NSAlert, kind: String,
                 in window: NSWindow?,
                 done: @escaping (Bool) -> Void) {
        if let av = alert.accessoryView {
            registerFields(av)
        }
        /* SSH 启动的实例常无 key/main window —— 兜底任一顶窗，
         * 避免 runModal 脱离 dev 追踪（TOFU 卡死即此因）。 */
        guard let w = window ?? NSApp.keyWindow ?? NSApp.mainWindow
                ?? NSApp.windows.first else {
            let r = alert.runModal()
            done(r == .alertFirstButtonReturn); return
        }
        FileHandle.standardError.write("[dlg] present kind=\(kind) win=\(w)\n".data(using: .utf8)!)
        alert.beginSheetModal(for: w) { [weak self] r in
            self?.untrack(alert.window)
            done(r == .alertFirstButtonReturn)
        }
        tracked.append((alert.window, kind))
    }

    private func registerFields(_ v: NSView) {
        for s in v.subviews { registerFields(s) }
        if let sv = v as? NSScrollView, let d = sv.documentView {
            registerFields(d)
        }
        if let c = v as? NSControl {
            let id = c.accessibilityIdentifier()
            if !id.isEmpty { fields[id] = c }
        }
        if let tv = v as? NSTextView {
            let id = tv.accessibilityIdentifier()
            if !id.isEmpty { textViews[id] = tv }
        }
    }
    /// 手动 beginSheetModal 的 alert（present(done:) 只能回 Bool，
    /// 多分支响应需 raw response）——仍登记进 dev dialogs。
    /// 必须在 beginSheetModal 之后调用（alert.window 才存在）。
    func track(alert: NSAlert, kind: String) {
        if let av = alert.accessoryView { registerFields(av) }
        tracked.append((alert.window, kind))
    }
    func track(window: NSWindow, kind: String) {
        if let cv = window.contentView { registerFields(cv) }
        tracked.append((window, kind))
    }
    func untrack(alert: NSAlert) { untrack(alert.window) }

    func untrack(_ w: NSWindow) {
        tracked.removeAll { $0.win === w }
        fields.removeAll()
        textViews.removeAll()
    }

    /// beginSheetModal 在已有 sheet 时静默失败，其完成回调永不触发 →
    /// 记录滞留。读取前清掉「未 attach 且不可见」的窗。
    private func pruneStale() {
        tracked.removeAll { !$0.win.isVisible && $0.win.sheetParent == nil }
    }

    /// dev `dialogs`：JSON
    var json: String {
        pruneStale()
        var arr: [[String: Any]] = []
        for t in tracked {
            let title = (t.win as? NSPanel)?.title ?? ""
            var btns: [String] = []
            var btitles: [String] = []
            var fids: [String] = []
            findButtons(t.win.contentView, &btns, &btitles)
            findFields(t.win.contentView, &fids)
            arr.append(["kind": t.kind, "title": title,
                        "buttons": btns, "buttonTitles": btitles,
                        "fields": fids])
        }
        let d = try? JSONSerialization.data(withJSONObject: arr)
        return d.flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
    }
    private func findButtons(_ v: NSView?, _ out: inout [String],
                             _ titles: inout [String]) {
        guard let v else { return }
        for s in v.subviews {
            if let b = s as? NSButton {
                let id = b.accessibilityIdentifier()
                out.append(id.isEmpty ? b.title : id)
                titles.append(b.title)
            }
            if let sv = s as? NSScrollView, let d = sv.documentView {
                findButtons(d, &out, &titles)
            }
            findButtons(s, &out, &titles)
        }
    }
    private func findFields(_ v: NSView?, _ out: inout [String]) {
        guard let v else { return }
        for s in v.subviews {
            if let t = s as? NSTextField, t.isEditable {
                let id = t.accessibilityIdentifier()
                if !id.isEmpty { out.append(id) }
            }
            if let tv = s as? NSTextView {
                let id = tv.accessibilityIdentifier()
                if !id.isEmpty { out.append(id) }
            }
            if let sv = s as? NSScrollView, let d = sv.documentView {
                findFields(d, &out)
            }
            findFields(s, &out)
        }
    }
    /// dev `dialog-fill <field> <base64>`
    func fill(field id: String, text: String) -> Bool {
        if let tv = textViews[id] {
            tv.string = text
            return true
        }
        guard let c = fields[id] else { return false }
        if let b = c as? NSButton {
            b.state = (text == "1" || text == "true") ? .on : .off
        } else {
            c.stringValue = text
            NotificationCenter.default.post(
                name: NSControl.textDidChangeNotification, object: c)
        }
        return true
    }
    /// dev `dialog-press <button-title-or-id>`
    func press(_ label: String) -> Bool {
        pruneStale()
        for t in tracked {
            if pressIn(t.win.contentView, label) { return true }
        }
        return false
    }
    private func pressIn(_ v: NSView?, _ label: String) -> Bool {
        guard let v else { return false }
        for s in v.subviews {
            if let b = s as? NSButton,
               (b.title == label || b.accessibilityIdentifier() == label) {
                b.performClick(nil); return true
            }
            if let sv = s as? NSScrollView, let d = sv.documentView,
               pressIn(d, label) { return true }
            if pressIn(s, label) { return true }
        }
        return false
    }
}
