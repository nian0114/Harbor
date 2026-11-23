import AppKit
import CHarbor
import HarborKit

/// Workspace 目标选择器（对齐 linux hb_workspace_pick_targets）：
/// 勾选 2–4 个分屏目标（Local + 各 host），创建后并排铺一行。
/// 用 NSAlert + accessoryView 承载 → DialogRegistry 自动可驱动。
final class WsPickSheet: NSObject {
    /// 自持有直到 sheet 关闭（NSButton.target 不 retain）。
    private static var live: [WsPickSheet] = []

    private weak var window: MainWindow?
    private weak var alertRef: NSAlert?
    private var checks: [(btn: NSButton, kind: String)] = []

    init(window: MainWindow) { self.window = window }

    func present() {
        WsPickSheet.live.append(self)
        let a = NSAlert()
        a.messageText = L("新建 Workspace")
        a.informativeText = L("选择 2–4 个分屏目标（并排一行）")
        a.addButton(withTitle: L("创建 Workspace"))
        a.addButton(withTitle: L("取消"))
        let acc = NSView()
        acc.translatesAutoresizingMaskIntoConstraints = false
        addRow(acc, L("Local · 本地终端"), "local", "local")
        let v = Vault.shared
        for h in v.list("host") {
            defer { hb_obj_free(h) }
            let id = hb_obj_id(h).str
            let name = hb_obj_get_str(h, "name").str
            let user = hb_obj_get_str(h, "user").str
            addRow(acc,
                   "\(name.isEmpty ? "host" : name) · ssh, " +
                   "\(user.isEmpty ? "?" : user)",
                   "pick-\(id)", id)
        }
        var prev: NSView = acc
        for (b, _) in checks {
            NSLayoutConstraint.activate([
                b.leadingAnchor.constraint(equalTo: acc.leadingAnchor,
                                           constant: 4),
                prev === acc
                    ? b.topAnchor.constraint(equalTo: acc.topAnchor,
                                             constant: 6)
                    : b.topAnchor.constraint(equalTo: prev.bottomAnchor,
                                             constant: 4),
            ])
            prev = b
        }
        NSLayoutConstraint.activate([
            acc.widthAnchor.constraint(equalToConstant: 300),
            prev.bottomAnchor.constraint(equalTo: acc.bottomAnchor,
                                         constant: -4),
        ])
        a.accessoryView = acc
        alertRef = a
        updateCreate()
        DialogRegistry.shared.present(alert: a, kind: "ws-pick",
                                      in: window) { [weak self] ok in
            defer {
                if let s = self {
                    WsPickSheet.live.removeAll { $0 === s }
                }
            }
            guard ok, let self, let w = self.window else { return }
            self.finish(w)
        }
    }

    private func addRow(_ acc: NSView, _ t: String, _ aid: String,
                        _ kind: String) {
        let b = NSButton(checkboxWithTitle: t, target: self,
                         action: #selector(toggle(_:)))
        b.translatesAutoresizingMaskIntoConstraints = false
        b.setAccessibilityIdentifier(aid)
        b.font = .systemFont(ofSize: 12)
        acc.addSubview(b)
        checks.append((b, kind))
    }
    @objc private func toggle(_ b: NSButton) { updateCreate() }
    private func updateCreate() {
        let n = checks.filter { $0.btn.state == .on }.count
        alertRef?.buttons.first?.isEnabled = (n >= 2 && n <= 4)
    }
    private func finish(_ w: MainWindow) {
        var leaves: [TermTab] = []
        let v = Vault.shared
        for (b, kind) in checks where b.state == .on {
            if kind == "local" {
                leaves.append(TermTab.local(rows: 24, cols: 80))
            } else if let h = v.get(kind) {
                let name = hb_obj_get_str(h, "name").str
                hb_obj_free(h)
                leaves.append(SSHTab(hostId: kind, quickTarget: nil,
                                     window: w,
                                     title: name.isEmpty ? kind : name))
            }
        }
        w.openWorkspace(leaves: leaves)
    }
}
