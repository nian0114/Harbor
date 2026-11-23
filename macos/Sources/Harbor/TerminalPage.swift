import AppKit
import CHarbor
import HarborKit

// ---------- Tab ----------
class TermTab: NSObject {
    let id = UUID()
    /// 工作区叶 id（对齐 Linux WsNode.page）——会话页在 Workspace 里的身份。
    let wsLeafId = UUID()
    /// 所属工作区；nil = pinned 页或尚未入区。
    weak var wsLeaf: Workspace?
    /// pinned 页（Vault/SFTP）：不可关、strip 画图标不画 ×。
    var closable = true
    var iconName: String?
    var title: String { didSet { onChange?() } }
    var statusDot: Theme.RGBA = Theme.dotPend { didSet { onChange?() } }
    var view: NSView
    var session: Session?
    var term: TerminalView?
    var onChange: (() -> Void)?
    var onExit: ((Int32) -> Void)?
    private var exitBar: NSView?

    // ---- Workspace 叶钩子（对齐 Linux HbSessionPage 的面） ----
    /// pane-head 图标；Local=terminal，SSH=os tile。
    var wsIconName: String { "terminal" }
    /// pane-head 标题。
    var wsTitle: String { title }
    /// snippet scope / watch 主机规则用的 host id；Local → nil。
    var wsHostId: String? { nil }
    /// SSH 叶（watch 默认开、SSH 图标）；SSHTab 覆盖为 true。
    var isSSHLeaf: Bool { false }
    /// 会话已死（pane-head 出重连钮）。
    var wsDead: Bool { session.map { !$0.alive } ?? false }
    /// 全部终端（WsTab 覆盖 → 各 pane）。
    var allTerms: [TerminalView] { term.map { [$0] } ?? [] }
    /// pane-head 重连钮；Local = 重启 shell，SSH = 重连。
    func wsReconnect() { restart() }
    /// 分屏克隆：Local → 新本地终端；SSHTab 覆盖 → 同 host 新连接。
    func wsClone() -> TermTab? {
        let t = TermTab.local(rows: 24, cols: 80)
        t.start()
        return t
    }

    /// WsTab 覆盖：焦点叶变化 → tab 外观同步。普通叶为空实现。
    func syncFromWorkspace() {}

    init(title: String, view: NSView) { self.title = title; self.view = view }

    static func local(rows: Int32, cols: Int32) -> TermTab {
        let t = TermTab(title: L("本地终端"), view: NSView())
        let term = TerminalView()
        term.translatesAutoresizingMaskIntoConstraints = false
        t.view.addSubview(term)
        NSLayoutConstraint.activate([
            term.leadingAnchor.constraint(equalTo: t.view.leadingAnchor),
            term.trailingAnchor.constraint(equalTo: t.view.trailingAnchor),
            term.topAnchor.constraint(equalTo: t.view.topAnchor),
            term.bottomAnchor.constraint(equalTo: t.view.bottomAnchor),
        ])
        t.term = term
        t.statusDot = Theme.dotOk
        return t
    }

    func start(reuse: OpaquePointer? = nil) {
        guard let term else { return }
        let sb = Int32(Vault.shared.setting("term_scrollback") ?? "10000")
            ?? 10000
        let s = Session(rows: Int32(term.rows()), cols: Int32(term.cols()),
                        scrollback: sb, reuse: reuse)
        s.onEvent = { [weak self, weak term] ev in
            guard let self else { return }
            switch ev.type {
            case HB_SESS_EV_OUTPUT: term?.poke()
            case HB_SESS_EV_TITLE:
                if let c = ev.text {
                    let t = String(cString: c)
                    if !t.isEmpty { self.title = t }
                }
            case HB_SESS_EV_BELL: term?.bell()
            case HB_SESS_EV_EXIT:
                let code = ev.code
                self.statusDot = Theme.dotErr
                self.showExit(code: Int32(code))
            default: break
            }
        }
        term.session = s
        session = s
        s.drain()
    }

    func showExit(code: Int32) {
        guard let term, exitBar == nil else { return }
        let bar = NSView()
        bar.wantsLayer = true
        bar.layer?.backgroundColor = Theme.subtle.color.cgColor
        let lbl = NSTextField(labelWithString:
            L("已结束") + " (\(code))")
        lbl.font = .systemFont(ofSize: 12)
        let btn = NSButton(title: L("重新启动"), target: self,
                           action: #selector(restart))
        btn.bezelStyle = .rounded
        bar.translatesAutoresizingMaskIntoConstraints = false
        lbl.translatesAutoresizingMaskIntoConstraints = false
        btn.translatesAutoresizingMaskIntoConstraints = false
        bar.addSubview(lbl); bar.addSubview(btn)
        term.superview?.addSubview(bar)
        NSLayoutConstraint.activate([
            bar.leadingAnchor.constraint(equalTo: term.superview!.leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: term.superview!.trailingAnchor),
            bar.bottomAnchor.constraint(equalTo: term.superview!.bottomAnchor),
            bar.heightAnchor.constraint(equalToConstant: 32),
            lbl.leadingAnchor.constraint(equalTo: bar.leadingAnchor,
                                         constant: 12),
            lbl.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            btn.trailingAnchor.constraint(equalTo: bar.trailingAnchor,
                                          constant: -8),
            btn.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
        ])
        exitBar = bar
        onExit?(code)
    }
    @objc func restart() {
        let v = session?.stealVt()
        session?.free_()
        exitBar?.removeFromSuperview(); exitBar = nil
        statusDot = Theme.dotOk
        title = L("本地终端")
        start(reuse: v)
    }
    var running: Bool { session?.alive ?? false }
    /// 终止会话并 join 线程（关 tab / 退出时）。
    func kill() { session?.free_() }
}

/// 工作区 tab：tabs[] 里的会话页包装（对齐 Linux tab→workspace 壳）。
/// term/session/title/statusDot 同步自焦点叶；running 为任一叶存活。
final class WsTab: TermTab {
    let ws: Workspace

    init(leaf: TermTab) {
        ws = Workspace(seed: leaf)
        super.init(title: leaf.title, view: ws.view)
        ws.tab = self
        syncFromWorkspace()
    }
    /// Workspace 选择器：多目标一次性铺一行（对齐 hb_window_open_workspace）。
    init(leaves: [TermTab]) {
        ws = Workspace(seed: leaves[0])
        for (i, l) in leaves.enumerated() where i > 0 {
            ws.addPane(l, horizontal: true,
                       frac: 1.0 / Float(leaves.count - i + 1))
        }
        super.init(title: "Workspace", view: ws.view)
        ws.tab = self
        iconName = leaves.count > 1 ? "cols" : nil
        syncFromWorkspace()
    }

    override var running: Bool { ws.anyRunning }
    override var allTerms: [TerminalView] { ws.allTerms }
    override func kill() { ws.killAll() }

    /// 焦点叶外观 → tab 外观（strip 重绘由 MainWindow 触发）。
    override func syncFromWorkspace() {
        let f = ws.focusedLeaf
        term = f?.term
        session = f?.session
        statusDot = f?.statusDot ?? Theme.dotPend
        if ws.paneCount > 1 {
            title = "Workspace"
            iconName = "cols"
        } else {
            title = f?.title ?? title
            iconName = nil
        }
    }
}
