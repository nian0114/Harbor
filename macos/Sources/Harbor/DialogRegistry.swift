import AppKit
import HarborKit

/// 追踪打开的 sheet/alert，供 dev hook `dialogs`/`dialog-fill`/`dialog-press`。
/// 每个被 present 的 NSAlert 的 window 注册进此表直到 dismiss。
final class DialogRegistry {
    static let shared = DialogRegistry()
    private var tracked: [(win: NSWindow, kind: String)] = []
    private var fields: [String: NSControl] = [:]

    func present(alert: NSAlert, kind: String,
                 in window: NSWindow?,
                 done: @escaping (Bool) -> Void) {
        if let av = alert.accessoryView {
            registerFields(av)
        }
        guard let w = window ?? NSApp.keyWindow ?? NSApp.mainWindow else {
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
        if let c = v as? NSControl {
            let id = c.accessibilityIdentifier()
            if !id.isEmpty { fields[id] = c }
        }
    }
    private func untrack(_ w: NSWindow) {
        tracked.removeAll { $0.win === w }
        fields.removeAll()
    }

    /// dev `dialogs`：JSON
    var json: String {
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
            findFields(s, &out)
        }
    }
    /// dev `dialog-fill <field> <base64>`
    func fill(field id: String, text: String) -> Bool {
        guard let c = fields[id] else { return false }
        if let b = c as? NSButton {
            b.state = (text == "1" || text == "true") ? .on : .off
        } else {
            c.stringValue = text
        }
        return true
    }
    /// dev `dialog-press <button-title-or-id>`
    func press(_ label: String) -> Bool {
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
            if pressIn(s, label) { return true }
        }
        return false
    }
}
