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
    /// 返回未启动的叶 —— session 由 Workspace 在 pane 布局
    /// 定稿后经 startSession() 按真实 frame 启动。
    func wsClone() -> TermTab? {
        return TermTab.local(rows: 24, cols: 80)
    }
    /// pane/首启布局后按真实 frame 启 session（对齐 linux
    /// realize-then-spawn；SSHTab 覆盖为 connect()）。
    /// term 此时已有真实 bounds → rows()/cols() 有效。
    func startSession() { start() }
    /// 监控设置对话框（对齐 hb_watch_ui_open）。
    func openWatch() { WatchDialog.present(leaf: self) }

    // ---- 终端内查找 ⌘F（单 pane tab；WsTab 覆盖 → Workspace.findHost） ----
    /// 浮在终端顶部的查找条（对齐 linux find bar 覆盖层）。
    private var singleFindBar: FindBar?
    private var singleFindTerm: TerminalView?

    func openFind() {
        guard let t = term else { return }
        let b: FindBar
        if let old = singleFindBar { b = old }
        else {
            b = FindBar()
            b.onClose = { [weak self] in self?.closeFind() }
            b.translatesAutoresizingMaskIntoConstraints = false
            singleFindBar = b
        }
        if b.superview == nil {
            view.addSubview(b)
            NSLayoutConstraint.activate([
                b.leadingAnchor.constraint(
                    equalTo: view.leadingAnchor),
                b.trailingAnchor.constraint(
                    equalTo: view.trailingAnchor),
                b.topAnchor.constraint(equalTo: view.topAnchor),
                b.heightAnchor.constraint(equalToConstant: 32),
            ])
        }
        singleFindTerm?.findClear()
        singleFindTerm?.onFindClose = nil
        singleFindTerm = t
        b.bind(t)
        b.open()
        t.onFindClose = { [weak self] in self?.closeFind() }
    }
    func closeFind() {
        singleFindTerm?.findClear()
        singleFindTerm?.onFindClose = nil
        singleFindTerm = nil
        singleFindBar?.removeFromSuperview()
        if let t = term { view.window?.makeFirstResponder(t) }
    }
    /// dev hook：设 find needle
    func devFindSet(_ s: String) {
        if singleFindTerm == nil { openFind() }
        singleFindBar?.devSet(s)
    }
    /// dev hook：上/下一条
    func devFindStep(_ dir: Int) { singleFindBar?.devStep(dir) }
    /// dev hook：find 计数 n/总数（""=未开）
    var findCountText: String {
        singleFindTerm == nil ? "" : (singleFindBar?.countText ?? "")
    }

    // ---- 完成监控状态（对齐 hb-term-page/hb-ssh-page watch_*） ----
    /// 开关：SSH 默认 settings.watch_default_ssh(true)，
    /// Local → watch_default_local(false)。SSH leaf 由 SSHTab 覆盖。
    lazy var watchOn: Bool = WatchCtl.defaultOn(isSSH: isSSHLeaf)
    lazy var watchSignals: UInt32 = WatchCtl.leafSignals()
    /// 角标："ok"✓绿 / "err"×红 / "pending"●琥珀（对齐 watch dot）。
    var watchBadge: String? { didSet { onChange?() } }
    /// 挂/摘引擎（session 建立后调用）。
    func applyWatch() {
        WatchCtl.apply(session?.sess, on: watchOn,
                       hostId: wsHostId, signals: watchSignals)
    }
    /// HB_SESS_EV_WATCH → 路由（通知中心 + 角标 + 系统通知接缝）。
    func handleWatchHit(_ hit: hb_watch_hit) {
        WatchRoute.route(leaf: self, hit: hit)
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
            case HB_SESS_EV_WATCH:
                if let h = ev.watch { self.handleWatchHit(h.pointee) }
            case HB_SESS_EV_EXIT:
                let code = ev.code
                self.statusDot = Theme.dotErr
                self.showExit(code: Int32(code))
            default: break
            }
        }
        term.session = s
        session = s
        applyWatch()
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

    // ---- ⌘F 转发到 Workspace（焦点 pane 的 find bar） ----
    override func openFind() { ws.openFind() }
    override func closeFind() { ws.closeFind() }
    override func devFindSet(_ s: String) { ws.devFindSet(s) }
    override func devFindStep(_ dir: Int) { ws.devFindStep(dir) }
    override var findCountText: String { ws.findCountText }

    /// tab 角标：任一叶有 badge → 最严重者优先（err>pending>ok）。
    var badgeGlyph: (String, NSColor)? {
        var best: String?
        for l in ws.leavesList {
            guard let b = l.watchBadge else { continue }
            if b == "err" { best = "err"; break }
            if best != "pending" { best = b }
        }
        switch best {
        case "err":     return ("×", Theme.red.color)
        case "pending": return ("●", Theme.amber.color)
        case "ok":      return ("✓", Theme.green.color)
        default:        return nil
        }
    }

    /// 焦点叶外观 → tab 外观（strip 重绘由 MainWindow 触发）。
    override func syncFromWorkspace() {
        let f = ws.focusedLeaf
        term = f?.term
        session = f?.session
        statusDot = f?.statusDot ?? Theme.dotPend
        if ws.paneCount > 1 {
            // tab 题 = 焦点 pane 题 + pane 数（tooltip 仍有 "Workspace · N 分屏"）
            let base = f?.wsTitle ?? L("Workspace")
            title = "\(base) · \(ws.paneCount)\(L("分屏"))"
            iconName = "cols"
        } else {
            title = f?.title ?? title
            iconName = nil
        }
    }
}
