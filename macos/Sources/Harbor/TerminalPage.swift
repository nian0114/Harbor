import AppKit
import CHarbor
import HarborKit

// ---------- Tab ----------
final class TermTab: NSObject {
    let id = UUID()
    var title: String { didSet { onChange?() } }
    var statusDot: Theme.RGBA = Theme.dotPend { didSet { onChange?() } }
    var view: NSView
    var session: Session?
    var term: TerminalView?
    var onChange: (() -> Void)?
    var onExit: ((Int32) -> Void)?
    private var exitBar: NSView?

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

    private func showExit(code: Int32) {
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
}
