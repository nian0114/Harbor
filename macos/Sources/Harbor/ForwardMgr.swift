import AppKit
import CHarbor
import HarborKit

/// 端口转发运行管理器（FR-06）——对齐 linux/src/hb-forward-mgr.c：
/// 按 vault "forward" 对象 id 管理运行会话；每条规则一个
/// HB_SSH_MODE_FORWARD 的 ConnCtl（完整 TOFU/密钥/密码/跳板/日志）。
/// 状态机 STOPPED → CONNECTING → LISTENING ⇄ RECONNECTING → FAILED；
/// 重连退避 1/2/5/10/30s，LISTENING 复位。
enum FwdState { case stopped, connecting, listening, reconnecting,
                failed }

struct FwdStatus {
    var state: FwdState = .stopped
    var boundPort = 0
    var up: UInt64 = 0
    var down: UInt64 = 0
    var active: UInt32 = 0
    var total: UInt32 = 0
    var message: String?
    var retryInS = 0
    var running: Bool {
        state == .connecting || state == .listening ||
        state == .reconnecting
    }
}

final class ForwardMgr {
    static let shared = ForwardMgr()
    /// 对话框（key-confirm/密码）挂载窗口；未就绪拒绝启动（同 linux）。
    weak var window: NSWindow?
    /// 单行/全量变更回调（linux mgr "changed" 信号；id=nil → 全刷）。
    var onChange: ((String?) -> Void)?
    private var runs: [String: FwdRun] = [:]
    private var bootDone = false
    private var vaultObs: NSObjectProtocol?

    private init() {
        vaultObs = NotificationCenter.default.addObserver(
            forName: .hbVaultChanged, object: nil, queue: .main)
        { [weak self] n in
            guard (n.userInfo?["type"] as? String) == "forward"
            else { return }
            self?.onStoreChanged(
                id: n.userInfo?["id"] as? String)
        }
    }
    deinit {
        if let o = vaultObs {
            NotificationCenter.default.removeObserver(o)
        }
    }

    private func emit(_ id: String?) { onChange?(id) }

    // ---------- 对外 ----------
    func start(_ id: String) { runStart(id, persist: true) }
    func stop(_ id: String) {
        if let r = runs[id] {
            runStop(r, persist: true)
        } else {
            /* 未运行也要把意图写回（开关来回拨） */
            persistEnabled(id, false)
        }
    }
    /// 启动恢复：窗口就绪后一次性拉起全部 enabled 规则（不改写意图）。
    func startAll() {
        bootDone = true
        for o in Vault.shared.list("forward") {
            if hb_obj_get_bool(o, "enabled", false) {
                runStart(hb_obj_id(o).str, persist: false)
            }
            hb_obj_free(o)
        }
    }
    /// 应用退出兜底：摘下全部会话；enabled 不动，下次启动恢复。
    func stopAll() {
        for r in runs.values { runStop(r, persist: false) }
    }
    /// SHELL/SFTP 连上主机 X → 拉起 via==X 且 autostart 的规则。
    func autostart(_ hostId: String) {
        for o in Vault.shared.list("forward") {
            let via = hb_obj_get_str(o, "via").str
            if via == hostId,
               hb_obj_get_bool(o, "autostart", false) {
                let id = hb_obj_id(o).str
                if let r = runs[id],
                   r.wantRun || r.state == .connecting ||
                   r.state == .reconnecting {
                } else {
                    runStart(id, persist: false)
                }
            }
            hb_obj_free(o)
        }
    }
    func status(_ id: String) -> FwdStatus {
        guard let r = runs[id] else { return FwdStatus() }
        var st = FwdStatus(state: r.state, boundPort: r.boundPort,
                           up: r.up, down: r.down, active: r.active,
                           total: r.total, message: r.message)
        if r.state == .reconnecting, let at = r.retryAt {
            let left = at.timeIntervalSinceNow
            st.retryInS = left > 0 ? Int(left) + 1 : 0
        }
        return st
    }
    func runningCount() -> Int {
        runs.values.filter { $0.state == .connecting ||
            $0.state == .listening ||
            $0.state == .reconnecting }.count
    }
    var devRuns: [String: FwdState] {
        runs.mapValues { $0.state }
    }

    // ---------- 启动 / 停止 ----------
    private func runStart(_ id: String, persist: Bool) {
        let r = run(id, create: true)
        if r.wantRun { return }
        guard let o = Vault.shared.get(id) else { return }
        if hb_obj_tombstone(o) { hb_obj_free(o); return }
        guard window != nil else {
            failPermanent(r, L("窗口未就绪"), details: nil)
            hb_obj_free(o); return
        }
        let via = hb_obj_get_str(o, "via").str
        guard hostExists(via) else {
            failPermanent(r, L("经由主机不存在或已删除"), details: nil)
            hb_obj_free(o); return
        }
        r.ensureCtl(via: via, window: window)
        r.via = via
        r.cfg = FwdKit.cfgKey(kind: hb_obj_get_str(o, "kind").str,
            bindAddr: hb_obj_get_str(o, "bind_addr").str,
            bindPort: Int(hb_obj_get_int(o, "bind_port", 0)),
            destAddr: hb_obj_get_str(o, "dest_addr").str,
            destPort: Int(hb_obj_get_int(o, "dest_port", 0)),
            via: via)
        r.applyRule(o)
        r.wantRun = true
        if persist { persistEnabled(id, true) }
        r.resetSnap()
        r.state = .connecting
        r.startConn()
        emit(id)
        hb_obj_free(o)
    }
    private func runStop(_ r: FwdRun, persist: Bool) {
        r.wantRun = false
        r.retryTimer?.invalidate(); r.retryTimer = nil; r.retryAt = nil
        r.sess = nil
        r.ctl?.cancel()
        r.state = .stopped
        if persist { persistEnabled(r.id, false) }
        emit(r.id)
    }
    /// 运行中的规则被编辑 → 新配置重启（linux run_restart）。
    private func runRestart(_ r: FwdRun, _ o: OpaquePointer) {
        let via = hb_obj_get_str(o, "via").str
        guard hostExists(via) else {
            failPermanent(r, L("经由主机不存在或已删除"), details: nil)
            return
        }
        r.via = via
        r.ensureCtl(via: via, window: window)
        r.applyRule(o)
        r.retryTimer?.invalidate(); r.retryTimer = nil; r.retryAt = nil
        r.sess = nil
        r.ctl?.cancel()
        r.resetSnap()
        r.state = .connecting
        r.startConn()
        emit(r.id)
    }
    /// 不可重试失败：FAILED + enabled=false + 一条通知。
    func failPermanent(_ r: FwdRun, _ title: String, details: String?) {
        r.retryTimer?.invalidate(); r.retryTimer = nil; r.retryAt = nil
        r.sess = nil
        r.wantRun = false
        r.state = .failed
        /* message 取面向用户的中文说明（ev->text=title）；details 为
         * libssh2 原始诊断时附括号 —— 远程拒绝因此带
         * AllowTcpForwarding/GatewayPorts 说明（FR-06 验收点）。 */
        var msg = (details?.isEmpty == false) ? details! : title
        if let d = details, !d.isEmpty, !title.isEmpty,
           d.hasPrefix("libssh2"), !msg.contains(title) {
            msg = "\(title) (\(d))"
        }
        r.message = msg
        persistEnabled(r.id, false)
        var desc: String?
        if let o = Vault.shared.get(r.id), !hb_obj_tombstone(o) {
            desc = FwdKit.ruleDesc(
                kind: FwdKit.Kind(rawValue:
                    hb_obj_get_str(o, "kind").str) ?? .local,
                bindAddr: hb_obj_get_str(o, "bind_addr").str,
                bindPort: Int(hb_obj_get_int(o, "bind_port", 0)),
                destAddr: hb_obj_get_str(o, "dest_addr").str,
                destPort: Int(hb_obj_get_int(o, "dest_port", 0)))
            hb_obj_free(o)
        }
        let t = L("转发规则启动失败 · %@").fmt(desc ?? r.id)
        let b = msg
        NotifyCenter.shared.post(kind: "err", title: t, body: b,
                                 tabIndex: nil)
        NotifyCenter.shared.system.post(title: t, body: b,
                                        tabIndex: nil,
                                        priorityHigh: true)
        emit(r.id)
    }

    // ---------- 工具 ----------
    private func run(_ id: String, create: Bool) -> FwdRun {
        if let r = runs[id] { return r }
        let r = FwdRun(mgr: self, id: id)
        runs[id] = r
        return r
    }
    func removeRun(_ id: String) {
        if let r = runs.removeValue(forKey: id) {
            r.shutdown()
        }
    }
    private func hostExists(_ id: String) -> Bool {
        guard !id.isEmpty, let h = Vault.shared.get(id),
              !hb_obj_tombstone(h) else { return false }
        hb_obj_free(h)
        return true
    }
    private func persistEnabled(_ id: String, _ on: Bool) {
        guard let o = Vault.shared.get(id) else { return }
        if hb_obj_tombstone(o) { hb_obj_free(o); return }
        hb_obj_set_bool(o, "enabled", on)
        Vault.shared.put(o)
        hb_obj_free(o)
    }

    // ---------- vault 变更（linux on_store_changed）----------
    private func onStoreChanged(id: String?) {
        guard let id else { return }
        let o = Vault.shared.get(id)
        if let o, !hb_obj_tombstone(o) {
            if let r = runs[id], r.wantRun {
                let cfg = FwdKit.cfgKey(
                    kind: hb_obj_get_str(o, "kind").str,
                    bindAddr: hb_obj_get_str(o, "bind_addr").str,
                    bindPort: Int(hb_obj_get_int(o, "bind_port", 0)),
                    destAddr: hb_obj_get_str(o, "dest_addr").str,
                    destPort: Int(hb_obj_get_int(o, "dest_port", 0)),
                    via: hb_obj_get_str(o, "via").str)
                if cfg != r.cfg {
                    r.cfg = cfg
                    runRestart(r, o)
                }
            }
            hb_obj_free(o)
        } else {
            if let o = o { hb_obj_free(o) }
            /* 删除 → 停止并摘掉运行时 */
            if runs[id] != nil {
                removeRun(id)
                emit(id)
            }
        }
    }
}

// ============================ 单条规则运行 ============================

final class FwdRun: ConnDelegate {
    let id: String
    private weak var mgr: ForwardMgr?
    var ctl: ConnCtl?
    var sess: Session?
    var state: FwdState = .stopped
    var wantRun = false
    var via = ""
    var cfg = ""
    var boundPort = 0
    var up: UInt64 = 0
    var down: UInt64 = 0
    var active: UInt32 = 0
    var total: UInt32 = 0
    var message: String?
    var boIdx = 0
    var retryAt: Date?
    var retryTimer: Timer?

    init(mgr: ForwardMgr, id: String) {
        self.mgr = mgr; self.id = id
    }
    private func emit() { mgr?.onChange?(id) }
    func resetSnap() {
        boundPort = 0; up = 0; down = 0; active = 0; total = 0
        message = nil; boIdx = 0
    }
    func ensureCtl(via: String, window: NSWindow?) {
        if ctl == nil {
            let c = ConnCtl(mode: .forward, hostId: via,
                            window: window)
            c.delegate = self
            ctl = c
        } else {
            ctl?.hostId = via
            ctl?.window = window
        }
    }
    /// vault 对象 → ConnCtl 规则（linux rule_from_obj + set_forward）。
    func applyRule(_ o: OpaquePointer) {
        let k = hb_obj_get_str(o, "kind").str
        let kind: hb_pf_kind = k == "remote" ? HB_PF_REMOTE :
                               k == "dynamic" ? HB_PF_DYNAMIC : HB_PF_LOCAL
        ctl?.setForward(kind,
            bindAddr: hb_obj_get_str(o, "bind_addr").str,
            bindPort: Int32(hb_obj_get_int(o, "bind_port", 0)),
            destAddr: hb_obj_get_str(o, "dest_addr").str,
            destPort: Int32(hb_obj_get_int(o, "dest_port", 0)))
    }
    func startConn() {
        ctl?.start(rows: 24, cols: 80)
    }
    /// 彻底摘除（规则被删）：定时器 + 会话一并停。
    func shutdown() {
        retryTimer?.invalidate(); retryTimer = nil
        sess = nil
        ctl?.cancel()
        ctl?.join()
    }

    // ---------- 重连退避 ----------
    private func scheduleRetry() {
        let secs = FwdKit.backoffSecs(boIdx)
        if boIdx < FwdKit.backoff.count { boIdx += 1 }
        retryAt = Date().addingTimeInterval(TimeInterval(secs))
        retryTimer?.invalidate()
        retryTimer = Timer.scheduledTimer(
            withTimeInterval: TimeInterval(secs), repeats: false)
        { [weak self] _ in self?.retryFire() }
    }
    private func enterReconnecting() {
        guard wantRun else { return }
        sess = nil
        /* 幂等：failed + exited 成对到来时同一次失败只推进一次退避 */
        if state == .reconnecting && retryTimer != nil { return }
        state = .reconnecting
        scheduleRetry()
        emit()
    }
    private func retryFire() {
        retryTimer = nil
        guard wantRun else { return }
        /* 重试瞬间 via 主机可能已被删除 → 判失败（不可重试） */
        guard let mgr else { return }
        let o = Vault.shared.get(via)
        if let o, !hb_obj_tombstone(o) {
            hb_obj_free(o)
        } else {
            if let o { hb_obj_free(o) }
            mgr.failPermanent(self, L("经由主机不存在或已删除"),
                              details: nil)
            return
        }
        state = .connecting
        startConn()
        emit()
    }

    // ---------- ConnDelegate ----------
    func conn(_ c: ConnCtl, stage idx: Int, line: String,
              mark: Character) {}
    func connOSInfo(_ c: ConnCtl, id: String, pretty: String) {}
    func connSession(_ c: ConnCtl, _ s: Session) {
        sess = s
        s.onEvent = { [weak self, weak ctl = self.ctl] ev in
            guard let self else { return }
            var e = ev
            if ctl?.handleEvent(&e) == true { return }
            if ev.type == HB_SESS_EV_FORWARD, let pf = ev.pf {
                self.onPf(pf.pointee)
            }
        }
    }
    private func onPf(_ st: hb_pf_status) {
        switch st.kind {
        case HB_PF_EV_LISTENING:
            state = .listening
            boundPort = Int(st.bound_port)
            boIdx = 0                  /* 成功 → 退避复位 */
            message = nil
        case HB_PF_EV_STATS:
            up = st.bytes_up; down = st.bytes_down
            active = st.active; total = st.total
        case HB_PF_EV_WARN:
            message = st.message.map { String(cString: $0) }
        default: break
        }
        emit()
    }
    func connConnected(_ c: ConnCtl) {}
    func connFailed(_ c: ConnCtl, code: Int, title: String,
                    details: String?) {
        guard wantRun, let mgr else { return }
        if Self.retryable(code) {
            enterReconnecting()
            return
        }
        /* AUTH / HOSTKEY_* / CANCELLED / FORWARD / code 0（绑定冲突、
         * 密钥被拒、跳板缺失…） → 永久失败 */
        mgr.failPermanent(self,
                          title.isEmpty ? L("启动失败") : title,
                          details: details)
    }
    func connDisconnected(_ c: ConnCtl) { enterReconnecting() }
    func connExited(_ c: ConnCtl, code: Int, reason: String) {
        /* 会话线程结束：已判 FAILED / 用户停止 → 忽略；否则视作断开 */
        if state == .failed || !wantRun { return }
        enterReconnecting()
    }
    /// linux retryable_code（hb-forward-mgr.c 同名单）。
    static func retryable(_ code: Int) -> Bool {
        let c = Int32(code)
        for e in [HB_SSH_ERR_RESOLVE, HB_SSH_ERR_REFUSED,
                  HB_SSH_ERR_UNREACHABLE, HB_SSH_ERR_TIMEOUT,
                  HB_SSH_ERR_HANDSHAKE, HB_SSH_ERR_CHANNEL,
                  HB_SSH_ERR_DISCONNECTED] where
            c == Int32(e.rawValue) {
            return true
        }
        return false
    }
}
