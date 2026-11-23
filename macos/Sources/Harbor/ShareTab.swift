import AppKit
import CHarbor
import HarborKit

/// FR-12 viewer tab（对齐 linux/src/hb-share-page.c）：
/// 渲染 hb_share_peer 的 vt（TerminalView ext 模式）+ banner（观看 X ·
/// 权限 pill · 端到端加密 · 离开）+ 只读提示（rate-limit 3s）+ ended
/// overlay（保留最后一帧 + 原因 + 关闭）。
final class ShareTab: TermTab {
    /// 引擎 ctx（service peers 拥有；kill 时归还）。
    private(set) var ctx: ShareService.PeerCtx?
    private(set) var ended = false
    private(set) var endText = ""

    private var permLbl: NSTextField!
    private var noticeLbl: NSTextField!
    private var endBox: NSView!
    private var endReasonLbl: NSTextField!
    private var termW: NSLayoutConstraint?
    private var termH: NSLayoutConstraint?
    private var noticeTimer: Timer?
    private var lastNotice = Date.distantPast
    private var obs: [NSObjectProtocol] = []

    init(ctx: ShareService.PeerCtx, title: String) {
        self.ctx = ctx
        super.init(title: title, view: NSView())
        iconName = "broadcast"
        buildView(ctx)
        subscribe()
        updatePerm()
        fitTerm()
        term?.poke()
    }

    // ---------------- view ----------------

    private func buildView(_ ctx: ShareService.PeerCtx) {
        // banner：broadcast + 正在观看 <title> + pill + 端到端加密 + 离开
        let banner = NSView()
        banner.wantsLayer = true
        banner.layer?.backgroundColor = Theme.subtle.color.cgColor
        let ic = NSImageView(image: hbIcon("broadcast", size: 14,
                                           tint: Theme.termDim.color))
        let watch = NSTextField(labelWithString:
            L("正在观看 ") +
            (ctx.peerTitle?.isEmpty == false
             ? ctx.peerTitle! : L("共享会话")))
        watch.font = .systemFont(ofSize: 12, weight: .semibold)
        watch.textColor = Theme.text.color
        permLbl = NSTextField(labelWithString: L("只读"))
        permLbl.font = .systemFont(ofSize: 11)
        permLbl.textColor = Theme.termDim.color
        let e2e = NSTextField(labelWithString: L("端到端加密"))
        e2e.font = .systemFont(ofSize: 11)
        e2e.textColor = Theme.termDim.color
        let leave = NSButton(title: L("离开"), target: self,
                             action: #selector(onLeave))
        leave.bezelStyle = .texturedRounded
        leave.font = .systemFont(ofSize: 11.5)
        leave.setAccessibilityIdentifier("share-leave")
        [banner, ic, watch, permLbl, e2e, leave].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false }
        banner.addSubview(ic); banner.addSubview(watch)
        banner.addSubview(permLbl); banner.addSubview(e2e)
        banner.addSubview(leave)
        view.addSubview(banner)

        // 只读提示（banner 下小条，rate-limited）
        noticeLbl = NSTextField(labelWithString: "")
        noticeLbl.font = .systemFont(ofSize: 11)
        noticeLbl.textColor = Theme.amber.color
        noticeLbl.isHidden = true
        noticeLbl.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(noticeLbl)

        // 终端 letterbox + ended overlay
        let body = NSView()
        body.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(body)
        let term = TerminalView()
        term.translatesAutoresizingMaskIntoConstraints = false
        term.extVt = ctx.vt
        term.onDeadInput = { [weak self] _ in
            self?.showNotice(L("只读模式，输入未生效"))
        }
        body.addSubview(term)
        self.term = term
        let tw = term.widthAnchor.constraint(equalToConstant: 640)
        let th = term.heightAnchor.constraint(equalToConstant: 400)
        termW = tw; termH = th
        NSLayoutConstraint.activate([
            banner.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            banner.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            banner.topAnchor.constraint(equalTo: view.topAnchor),
            banner.heightAnchor.constraint(equalToConstant: 30),
            ic.leadingAnchor.constraint(equalTo: banner.leadingAnchor,
                                        constant: 12),
            ic.centerYAnchor.constraint(equalTo: banner.centerYAnchor),
            watch.leadingAnchor.constraint(
                equalTo: ic.trailingAnchor, constant: 8),
            watch.centerYAnchor.constraint(equalTo: banner.centerYAnchor),
            permLbl.leadingAnchor.constraint(
                equalTo: watch.trailingAnchor, constant: 8),
            permLbl.centerYAnchor.constraint(
                equalTo: banner.centerYAnchor),
            leave.trailingAnchor.constraint(
                equalTo: banner.trailingAnchor, constant: -10),
            leave.centerYAnchor.constraint(equalTo: banner.centerYAnchor),
            e2e.trailingAnchor.constraint(
                equalTo: leave.leadingAnchor, constant: -10),
            e2e.centerYAnchor.constraint(equalTo: banner.centerYAnchor),
            noticeLbl.leadingAnchor.constraint(
                equalTo: view.leadingAnchor, constant: 14),
            noticeLbl.topAnchor.constraint(
                equalTo: banner.bottomAnchor, constant: 2),
            body.topAnchor.constraint(
                equalTo: noticeLbl.bottomAnchor, constant: 4),
            body.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            body.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            body.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            term.centerXAnchor.constraint(equalTo: body.centerXAnchor),
            term.centerYAnchor.constraint(equalTo: body.centerYAnchor),
            tw, th,
        ])

        // ended overlay：保留最后一帧 + 原因 + 关闭
        endBox = NSView()
        endBox.wantsLayer = true
        endBox.layer?.backgroundColor =
            Theme.card.color.withAlphaComponent(0.92).cgColor
        endBox.layer?.cornerRadius = 8
        endBox.isHidden = true
        endBox.translatesAutoresizingMaskIntoConstraints = false
        let eic = NSImageView(image: hbIcon("broadcast", size: 28,
                                            tint: Theme.termDim.color))
        endReasonLbl = NSTextField(labelWithString: L("共享已结束"))
        endReasonLbl.font = .systemFont(ofSize: 13)
        endReasonLbl.textColor = Theme.text.color
        let closeB = NSButton(title: L("关闭"), target: self,
                              action: #selector(onLeave))
        closeB.bezelStyle = .rounded
        closeB.keyEquivalent = "\r"
        closeB.setAccessibilityIdentifier("share-end-close")
        [eic, endReasonLbl, closeB].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            endBox.addSubview($0)
        }
        body.addSubview(endBox)
        NSLayoutConstraint.activate([
            endBox.centerXAnchor.constraint(equalTo: body.centerXAnchor),
            endBox.centerYAnchor.constraint(equalTo: body.centerYAnchor),
            endBox.widthAnchor.constraint(equalToConstant: 240),
            eic.topAnchor.constraint(equalTo: endBox.topAnchor,
                                     constant: 18),
            eic.centerXAnchor.constraint(
                equalTo: endBox.centerXAnchor),
            endReasonLbl.topAnchor.constraint(
                equalTo: eic.bottomAnchor, constant: 10),
            endReasonLbl.centerXAnchor.constraint(
                equalTo: endBox.centerXAnchor),
            closeB.topAnchor.constraint(
                equalTo: endReasonLbl.bottomAnchor, constant: 14),
            closeB.centerXAnchor.constraint(
                equalTo: endBox.centerXAnchor),
            closeB.bottomAnchor.constraint(
                equalTo: endBox.bottomAnchor, constant: -16),
        ])
    }

    // ---------------- 事件 ----------------

    private func subscribe() {
        let nc = NotificationCenter.default
        obs.append(nc.addObserver(
            forName: ShareService.nPeerEvent, object: ctx,
            queue: .main) { [weak self] n in
                guard let ev = n.userInfo?["ev"]
                        as? ShareService.ShareEv else { return }
                self?.onPeerEv(ev)
            })
        obs.append(nc.addObserver(
            forName: ShareService.nPeerEnded, object: ctx,
            queue: .main) { [weak self] n in
                self?.showEnded(n.userInfo?["reason"] as? String)
            })
    }

    private func onPeerEv(_ ev: ShareService.ShareEv) {
        switch ev.type {
        case HB_SH_EV_SNAPSHOT, HB_SH_EV_RESIZE:
            fitTerm(); term?.poke()
        case HB_SH_EV_PERM:
            updatePerm()
        case HB_SH_EV_NOTICE:
            if !ev.text.isEmpty { showNotice(ev.text) }
        case HB_SH_EV_ENDED:
            showEnded(ev.text)
        case HB_SH_EV_PAKE_FAIL:
            showNotice(L("配对码错误"))
        case HB_SH_EV_LOCKED:
            showEnded("locked")
        case HB_SH_EV_ERROR:
            showNotice(ev.text.isEmpty ? L("共享出错") : ev.text)
        default: break
        }
        // JOINED/PRESENCE 等也可能改了 vt
        term?.poke()
    }

    // ---------------- 状态 ----------------

    private func updatePerm() {
        guard let ctx else { return }
        let collab = ctx.perm == HB_SHARE_COLLAB
        permLbl.stringValue = ShareKit.permName(collab)
        permLbl.textColor = collab ? Theme.green.color
                                   : Theme.termDim.color
        term?.extWritable = collab
    }

    /// vt rows×cols → letterbox 尺寸约束（对齐 fit_term）。
    private func fitTerm() {
        guard let ctx, let v = ctx.vt, let term else { return }
        var r: Int32 = 24, c: Int32 = 80
        hb_vt_size(v, &r, &c)
        let cs = term.cellSizePx()
        if cs.width > 0 && cs.height > 0 {
            termW?.constant = CGFloat(c) * cs.width + 32
            termH?.constant = CGFloat(r) * cs.height + 28
        }
    }

    private func showNotice(_ txt: String) {
        let now = Date()
        if now.timeIntervalSince(lastNotice) < 3 { return }  // 3s rate-limit
        lastNotice = now
        noticeLbl.stringValue = txt
        noticeLbl.isHidden = false
        noticeTimer?.invalidate()
        noticeTimer = Timer.scheduledTimer(withTimeInterval: 3,
            repeats: false) { [weak self] _ in
            self?.noticeLbl.isHidden = true
        }
    }

    func showEnded(_ reason: String?) {
        if ended { return }
        ended = true
        endText = ShareKit.endReason(reason)
        endReasonLbl.stringValue = endText
        endBox.isHidden = false
        statusDot = Theme.dotErr
    }

    @objc private func onLeave() {
        (view.window as? MainWindow)?.closeShareTab(self)
    }

    /// dev：viewer vt 可见屏文本（对齐 hb_share_page_screen）。
    func screenText() -> String {
        guard let v = ctx?.vt else { return "" }
        var rows: Int32 = 0, cols: Int32 = 0
        var out = ""
        hb_vt_lock(v)
        hb_vt_size(v, &rows, &cols)
        for r in 0..<rows {
            if let s = hb_vt_get_text(v, r, 0, r, cols - 1, false) {
                out += String(cString: s); free(s)
            }
            out += "\n"
        }
        hb_vt_unlock(v)
        return out
    }

    // ---------------- 生命周期 ----------------

    override func kill() {
        // 对齐 hb_share_page_dispose：先摘外部 vt（滞留的渲染/计时器
        // 不得再碰已释放的 vt），再 release peer。
        noticeTimer?.invalidate(); noticeTimer = nil
        for o in obs { NotificationCenter.default.removeObserver(o) }
        obs = []
        term?.extVt = nil
        if let c = ctx {
            ctx = nil
            ShareService.shared.releasePeer(c)
        }
        super.kill()
    }
}
