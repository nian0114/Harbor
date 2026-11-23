import AppKit
import HarborKit
import CHarbor

/// 连接模式（对应 linux HB_CONN_*）。
enum ConnMode { case shell, sftp, forward }

/// 对应 linux SIG_*。
protocol ConnDelegate: AnyObject {
    func conn(_ c: ConnCtl, stage idx: Int, line: String, mark: Character)
    func connSession(_ c: ConnCtl, _ s: Session)
    func connConnected(_ c: ConnCtl)
    func connFailed(_ c: ConnCtl, code: Int, title: String, details: String?)
    func connDisconnected(_ c: ConnCtl)
    func connExited(_ c: ConnCtl, code: Int, reason: String)
    func connOSInfo(_ c: ConnCtl, id: String, pretty: String)
}

/// host_id 连接或 quick-connect（target 字符串）皆可。
final class ConnCtl {
    let mode: ConnMode
    private(set) var hostId: String?
    private(set) var quickTarget: String?     // "user@host:port"
    private var hname = "", huser = ""
    private(set) var sess: Session?
    weak var delegate: ConnDelegate?
    private weak var window: NSWindow?

    private var wasConnected = false
    private(set) var reconnectPending = false
    private var awaitingReply = false
    private var reconnectTimer: Timer?
    private var retryCount = 0
    private var stopped = false
    private var reuseVt: OpaquePointer?
    private var logId: Int64 = 0
    private var t0 = Date()

    // 待“记住”的凭据
    private var pendKeyId: String?
    private var pendIsPass = false
    private var hopIds: [String?] = []
    private var promptField: NSSecureTextField?
    private var rememberBox: NSButton?

    // 借给 hb_ssh_opts 的分配：secrets 用完 wipe+free；allocs 仅 free
    private var secrets: [UnsafeMutablePointer<CChar>] = []
    private var allocs: [UnsafeMutableRawPointer] = []
    private var keepObjs: [OpaquePointer] = []
    private var missingJump: String?

    init(mode: ConnMode, hostId: String? = nil,
         quickTarget: String? = nil, window: NSWindow?) {
        self.mode = mode; self.hostId = hostId
        self.quickTarget = quickTarget; self.window = window
        hname = quickTarget ?? ""
    }
    private var v: Vault { Vault.shared }

    // ---------- build_ctx / host_to_hop ----------
    private func sec(_ p: UnsafeMutablePointer<CChar>?) -> UnsafePointer<CChar>? {
        if let p { secrets.append(p) }
        return UnsafePointer(p)
    }

    private func hostToHop(_ h: OpaquePointer, _ hop: inout hb_ssh_hop) {
        let hid = hb_obj_id(h).str
        hop.host = hb_obj_get_str(h, "address")
        hop.port = Int32(hb_obj_get_int(h, "port", 22))
        if hop.port <= 0 { hop.port = 22 }
        hop.user = hb_obj_get_str(h, "user")
        if hop.user == nil || hop.user!.str.isEmpty {
            let u = NSUserName().withCString { strdup($0) }!
            allocs.append(u); hop.user = UnsafePointer(u)
        }
        hop.label = hb_obj_get_str(h, "name")
        let auth = hb_obj_get_str(h, "auth").str
        let kid = hb_obj_get_str(h, "key_id").str
        hop.auth = HB_SSH_AUTH_AUTO
        switch auth {
        case "password":
            hop.auth = HB_SSH_AUTH_PASSWORD
            hop.password = sec(v.secretC(hid, "password"))
        case "key":
            hop.auth = HB_SSH_AUTH_KEY
            if kid.isEmpty {
                hop.identity_file = hb_obj_get_str(h, "identity_file")
                hop.passphrase = sec(v.secretC(hid, "passphrase"))
            } else {
                hop.privkey = sec(v.secretC(kid, "private"))
                hop.passphrase = sec(v.secretC(kid, "passphrase"))
            }
        case "agent": hop.auth = HB_SSH_AUTH_AGENT
        case "none":  hop.auth = HB_SSH_AUTH_NONE
        default:
            hop.auth = HB_SSH_AUTH_AUTO
            hop.identity_file = hb_obj_get_str(h, "identity_file")
            hop.passphrase = sec(v.secretC(hid, "passphrase"))
            hop.password = sec(v.secretC(hid, "password"))
        }
    }

    private func collectKnown() -> (UnsafeMutablePointer<UnsafePointer<CChar>?>?, Int) {
        let objs = v.list("known")
        defer { objs.forEach { hb_obj_free($0) } }
        let lines = objs.compactMap { hb_obj_get_str($0, "line") }
            .filter { $0.pointee != 0 }.map { strdup($0)! }
        let arr = UnsafeMutablePointer<UnsafePointer<CChar>?>.allocate(
            capacity: lines.count + 1)
        for (i, l) in lines.enumerated() { arr[i] = UnsafePointer(l) }
        arr[lines.count] = nil
        for l in lines { allocs.append(l) }
        allocs.append(arr)
        return (arr, lines.count)
    }

    /// 无需 host 对象时用 quickTarget 建临时对象。
    private func targetHostObj() -> OpaquePointer? {
        if let hid = hostId { return v.get(hid) }
        guard let qt = quickTarget else { return nil }
        var user: UnsafeMutablePointer<CChar>?
        var host: UnsafeMutablePointer<CChar>?
        var port: Int32 = 0
        guard hb_ssh_parse_target(qt, &user, &host, &port) == HB_OK,
              let host else { return nil }
        defer { free(user); free(host) }
        let h = hb_obj_new("host")!
        hb_obj_set_str(h, "address", host)
        if port > 0 { hb_obj_set_int(h, "port", Int64(port)) }
        if let user { hb_obj_set_str(h, "user", user) }
        hb_obj_set_str(h, "name", qt)
        hb_obj_set_str(h, "proto", "ssh")
        hb_obj_set_str(h, "auth", "auto")
        huser = user.map { String(cString: $0) } ?? ""
        return h
    }

    /// 连接视图头部图标用的 host os 字段（quickTarget → "" → 通用瓦片）。
    var hostOS: String {
        guard let hid = hostId, let h = v.get(hid) else { return "" }
        defer { hb_obj_free(h) }
        return hb_obj_get_str(h, "os").str
    }

    /// 连接视图头部副标题 "host:port"。
    var targetDesc: String {
        if let h = targetHostObj() {
            defer { hb_obj_free(h) }
            let addr = hb_obj_get_str(h, "address").str
            let port = hb_obj_get_int(h, "port", 22)
            return "\(addr):\(port > 0 ? port : 22)"
        }
        return quickTarget ?? ""
    }

    // ---------- 入口 ----------
    func start(rows: Int32, cols: Int32) {
        missingJump = nil
        hname = quickTarget ?? ""
        if let hid = hostId, let h = v.get(hid) {
            hname = hb_obj_get_str(h, "name").str
            huser = hb_obj_get_str(h, "user").str
            hb_obj_free(h)
        }
        /* 重连/重试替换旧 session：被 free 的会话其 EXIT 事件随队列
         * 排空而丢失 —— 先收尾旧日志行（linux 靠 orphan sweep 兜
         * 底，mac 在替换点直接收尾）。 */
        logFinish(wasConnected ? "ok" : "cancelled", nil)
        logStart()
        let confirm = collectConfirmKeys()
        if confirm.isEmpty { continueConnect(rows: rows, cols: cols) }
        else { confirmKeys(confirm, rows: rows, cols: cols) }
    }

    private func collectConfirmKeys() -> [String] {
        guard let hid = hostId, let h = v.get(hid) else { return [] }
        defer { hb_obj_free(h) }
        var jv: UnsafeMutablePointer<UnsafePointer<CChar>?>?
        let nj = hb_obj_get_strv(h, "jump", &jv)
        defer { free(jv) }
        var out: [String] = []
        for i in 0...nj {
            var hh: OpaquePointer?
            if i < nj {
                if let j = jv?[i] { hh = v.get(j.str) }
            } else { hh = h }
            guard let hh else { continue }
            let auth = hb_obj_get_str(hh, "auth").str
            let kid = hb_obj_get_str(hh, "key_id").str
            if auth == "key", !kid.isEmpty, let k = v.get(kid) {
                if hb_obj_get_bool(k, "confirm", false) {
                    out.append("\(hb_obj_get_str(k, "name").str) → "
                        + "\(hb_obj_get_str(hh, "user").str)@\(hb_obj_get_str(hh, "name").str)")
                }
                hb_obj_free(k)
            }
            if i < nj { hb_obj_free(hh) }
        }
        return out
    }

    private func confirmKeys(_ lines: [String], rows: Int32, cols: Int32) {
        let a = NSAlert()
        a.messageText = L("确认使用密钥")
        a.informativeText = lines.joined(separator: "\n")
        a.addButton(withTitle: L("继续连接"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: "key-confirm",
                                      in: window) { [weak self] ok in
            guard let self else { return }
            if ok { self.continueConnect(rows: rows, cols: cols) }
            else {
                self.logFinish("cancelled", "key confirm denied")
                self.delegate?.connFailed(self, code: 0,
                    title: L("已拒绝使用密钥，连接已取消"), details: nil)
            }
        }
    }

    private func continueConnect(rows: Int32, cols: Int32) {
        if let old = sess { old.free_(); sess = nil }
        cleanupCtx()
        var o = hb_ssh_opts()
        var target = hb_ssh_hop()
        var jumps: [hb_ssh_hop] = []
        hopIds = []
        guard let h = targetHostObj() else {
            logFinish("error", "no target")
            delegate?.connFailed(self, code: 0, title: L("无法解析目标主机"),
                                 details: quickTarget)
            return
        }
        keepObjs.append(h)
        hostToHop(h, &target)
        var jv: UnsafeMutablePointer<UnsafePointer<CChar>?>?
        let nj = hb_obj_get_strv(h, "jump", &jv)
        for i in 0..<nj {
            let jid = jv?[i].str ?? ""
            guard let jh = v.get(jid) else { missingJump = jid; break }
            var jhop = hb_ssh_hop()
            hostToHop(jh, &jhop)
            jumps.append(jhop)
            keepObjs.append(jh)
            hopIds.append(jid)
        }
        free(jv)
        if missingJump != nil { jumps.removeAll() }
        hopIds.append(hostId)
        if let mj = missingJump {
            logFinish("error", "missing jump host")
            delegate?.connFailed(self, code: 0,
                title: String(format: L("跳板主机不存在：%@"), mj), details: nil)
            cleanupCtx(); return
        }
        o.target = target
        let jp = UnsafeMutablePointer<hb_ssh_hop>.allocate(
            capacity: max(1, jumps.count))
        for (i, j) in jumps.enumerated() { jp[i] = j }
        defer { jp.deallocate() }
        o.jumps = jumps.isEmpty ? nil : UnsafePointer(jp)
        o.njumps = jumps.count
        let (kl, nk) = collectKnown()
        o.known_lines = UnsafePointer(kl)
        o.nknown = nk
        o.connect_timeout_s = 15
        let ka = hb_obj_get_int(h, "keepalive", -1)
        o.keepalive_s = ka < 0 ? 30 : Int32(ka)
        let termC = strdup("xterm-256color")!
        allocs.append(termC)
        o.term = UnsafePointer(termC)
        let os = hb_obj_get_str(h, "os").str
        o.detect_os = os.isEmpty || os == "auto"
        o.reuse_vt = reuseVt
        o.mode = mode == .sftp ? HB_SSH_MODE_SFTP :
                 mode == .forward ? HB_SSH_MODE_FORWARD : HB_SSH_MODE_SHELL
        let s = Session(sshOpts: &o, rows: rows, cols: cols,
                        scrollback: Int32(Vault.shared.setting("term_scrollback")
                            ?? "10000") ?? 10000)
        sess = s
        // onEvent 由 delegate（SSHTab）设置：先 handleEvent，再终端事件
        delegate?.connSession(self, s)
        cleanupCtx()
    }

    private func cleanupCtx() {
        for p in secrets { memset(p, 0, strlen(p)); hb_secret_free(p) }
        secrets.removeAll()
        for p in allocs { free(p) }
        allocs.removeAll()
        keepObjs.forEach { hb_obj_free($0) }
        keepObjs.removeAll()
    }

    // ---------- 事件（镜像 ctl_handle_event；返回是否已消费） ----------
    @discardableResult
    func handleEvent(_ ev: UnsafePointer<hb_sess_event>) -> Bool {
        switch ev.pointee.type {
        case HB_SESS_EV_STAGE:
            delegate?.conn(self, stage: Int(ev.pointee.code),
                           line: ev.pointee.text.str, mark: " ")
            return true
        case HB_SESS_EV_HOSTKEY: onHostKey(ev); return true
        case HB_SESS_EV_PROMPT:  onPrompt(ev);  return true
        case HB_SESS_EV_CONNECTED: onConnected(); return true
        case HB_SESS_EV_OSINFO:
            let id = ev.pointee.text.str
            delegate?.connOSInfo(self, id: id, pretty: ev.pointee.text2.str)
            if let hid = hostId, let h = v.get(hid), !id.isEmpty {
                hb_obj_set_str(h, "os", id); v.put(h); hb_obj_free(h)
            }
            return true
        case HB_SESS_EV_ERROR: onError(ev); return true
        case HB_SESS_EV_EXIT:  onExit(ev);  return true
        default: return false
        }
    }

    // ---------- HOSTKEY ----------
    private func onHostKey(_ ev: UnsafePointer<hb_sess_event>) {
        awaitingReply = true
        let changed = ev.pointee.code == 1
        let hostPort = ev.pointee.text.str
        let fp = ev.pointee.text2.str
        let parts = ev.pointee.extra.str.split(separator: "\n",
                                               maxSplits: 1,
                                               omittingEmptySubsequences: false)
        let line = parts.first.map(String.init) ?? ""
        let art = parts.count > 1 ? String(parts[1]) : ""
        let a = NSAlert()
        if changed {
            a.alertStyle = .critical
            a.messageText = L("主机密钥已变更")
            a.informativeText = String(format:
                L("主机 %@ 的密钥与记录不符，可能遭遇中间人攻击！\n\n%@\n\n%@"),
                hostPort, fp, art)
            a.addButton(withTitle: L("断开"))         // 默认 = Return
            a.addButton(withTitle: L("仍然连接"))
        } else {
            a.messageText = String(format: L("首次连接 %@"), hostPort)
            a.informativeText = "\(fp)\n\n\(art)"
            a.addButton(withTitle: L("信任并连接"))
            a.addButton(withTitle: L("断开"))
        }
        DialogRegistry.shared.present(alert: a,
                                      kind: changed ? "hk-changed" : "tofu",
                                      in: window) { [weak self] ok in
            guard let self, let s = self.sess?.sess else { return }
            self.awaitingReply = false
            let accepted = changed ? !ok : ok
            hb_session_hostkey_reply(s, accepted)
            if accepted { self.saveKnown(hostPort: hostPort, fp: fp, line: line) }
            else {
                self.logFinish(changed ? "hostkey_changed" : "hostkey_rejected",
                               "user rejected")
            }
        }
    }

    private func saveKnown(hostPort: String, fp: String, line: String) {
        guard !line.isEmpty else { return }
        let sp = line.split(separator: " ")
        guard sp.count >= 3 else { return }
        var host = String(sp[0])
        let keytype = String(sp[1]), b64 = String(sp[2])
        var port: Int64 = 22
        if host.hasPrefix("["), let rb = host.firstIndex(of: "]") {
            let after = host.index(after: rb)
            if after < host.endIndex, host[after] == ":" {
                port = Int64(host[host.index(after: after)...]) ?? 22 }
            host = String(host[host.index(after: host.startIndex)..<rb])
        } else if let cc = host.lastIndex(of: ":") {
            port = Int64(host[host.index(after: cc)...]) ?? 22
            host = String(host[..<cc])
        }
        let fpOnly = fp.split(separator: " ").last.map(String.init) ?? fp
        let objs = v.list("known")
        var o = objs.first(where: {
            hb_obj_get_str($0, "host").str == host &&
            hb_obj_get_int($0, "port", 22) == port })
        objs.filter { $0 != o }.forEach { hb_obj_free($0) }
        if o == nil { o = v.newObj("known") }
        guard let o else { return }
        hb_obj_set_str(o, "host", host); hb_obj_set_int(o, "port", port)
        hb_obj_set_str(o, "keytype", keytype); hb_obj_set_str(o, "key", b64)
        hb_obj_set_str(o, "fingerprint", fpOnly)
        hb_obj_set_int(o, "added", Int64(Date().timeIntervalSince1970 * 1000))
        hb_obj_set_str(o, "line", line)
        v.put(o); hb_obj_free(o)
        _ = hostPort
    }

    // ---------- PROMPT ----------
    private func onPrompt(_ ev: UnsafePointer<hb_sess_event>) {
        FileHandle.standardError.write("[conn] PROMPT code=\(ev.pointee.code)\n".data(using: .utf8)!)
        awaitingReply = true
        let isPass = ev.pointee.code == 1
        let idx = Int(ev.pointee.text2.str) ?? -1
        let hopHost = (idx >= 0 && idx < hopIds.count
                       ? hopIds[idx] : nil) ?? hostId
        var targetId = hopHost
        if let hh = hopHost, let h = v.get(hh) {
            let kid = hb_obj_get_str(h, "key_id").str
            if isPass && !kid.isEmpty { targetId = kid }
            hb_obj_free(h)
        }
        pendKeyId = targetId; pendIsPass = isPass
        let a = NSAlert()
        a.messageText = ev.pointee.text.str.isEmpty
            ? "Password:" : ev.pointee.text.str
        a.informativeText = isPass ? L("私钥口令") : L("密码")
        a.addButton(withTitle: L("确定"))
        a.addButton(withTitle: L("取消"))
        let f = NSSecureTextField(frame: NSRect(x: 0, y: 24, width: 240, height: 24))
        f.setAccessibilityIdentifier("prompt-secret")
        let rem = NSButton(checkboxWithTitle: L("记住（加密）"),
                           target: nil, action: nil)
        rem.frame = NSRect(x: 0, y: 0, width: 240, height: 20)
        rem.setAccessibilityIdentifier("prompt-remember")
        let stack = NSView(frame: NSRect(x: 0, y: 0, width: 240, height: 52))
        stack.addSubview(f); stack.addSubview(rem)
        a.accessoryView = stack
        promptField = f; rememberBox = rem
        DialogRegistry.shared.present(alert: a, kind: "prompt",
                                      in: window) { [weak self] ok in
            guard let self, let s = self.sess?.sess else { return }
            self.awaitingReply = false
            let secStr = self.promptField?.stringValue ?? ""
            self.promptField?.stringValue = ""     // 清空对话框中的明文
            let remember = self.rememberBox?.state == .on
            self.promptField = nil; self.rememberBox = nil
            if ok {
                let buf = secStr.withCString { strdup($0) }
                hb_session_prompt_reply(s, buf)
                if let buf { memset(buf, 0, strlen(buf)); free(buf) }
                if remember, let pid = self.pendKeyId {
                    self.v.setSecret(pid,
                                     self.pendIsPass ? "passphrase" : "password",
                                     secStr)
                }
            } else {
                hb_session_prompt_reply(s, nil)
                self.logFinish("cancelled", "prompt cancelled")
            }
            self.pendKeyId = nil
        }
    }

    // ---------- CONNECTED / ERROR / EXIT ----------
    private func onConnected() {
        wasConnected = true; retryCount = 0
        if logId > 0 {
            guard let vv = v.vault else { return }
            _ = hb_vault_log_finish(vv, logId, "ok", nil, -1)
        }
        if let hid = hostId, let h = v.get(hid) {
            hb_obj_set_int(h, "last_connected",
                           Int64(Date().timeIntervalSince1970 * 1000))
            v.put(h); hb_obj_free(h)
        }
        delegate?.connConnected(self)
    }

    private func onError(_ ev: UnsafePointer<hb_sess_event>) {
        let msg = ev.pointee.text.str
        delegate?.conn(self, stage: -1,
                       line: msg.isEmpty ? L("连接失败") : msg, mark: "e")
        pendKeyId = nil
        let code = ev.pointee.code
        let credErr = code == Int32(HB_SSH_ERR_CANCELLED.rawValue)
            || code == Int32(HB_SSH_ERR_AUTH.rawValue)
            || code == Int32(HB_SSH_ERR_HOSTKEY_CHANGED.rawValue)
            || code == Int32(HB_SSH_ERR_HOSTKEY_REJECTED.rawValue)
        if wasConnected && !credErr {
            delegate?.connDisconnected(self)
            scheduleReconnect()
            return
        }
        logFinish(hb_ssh_err_name(code).str, msg)
        delegate?.connFailed(self, code: Int(code),
                             title: errTitle(code, msg),
                             details: ev.pointee.text2.str)
    }

    private func onExit(_ ev: UnsafePointer<hb_sess_event>) {
        // 对齐 linux：未连接即退出 → cancelled；已连接 → ok。
        if !wasConnected { pendKeyId = nil }
        logFinish(wasConnected ? "ok" : "cancelled",
                  ev.pointee.text.str)
        delegate?.connExited(self, code: Int(ev.pointee.code),
                             reason: ev.pointee.text.str)
    }

    // ---------- 重连 ----------
    private func scheduleReconnect() {
        guard !stopped else { return }
        retryCount += 1
        let delay = min(pow(2.0, Double(retryCount - 1)), 30.0)
        delegate?.conn(self, stage: -1,
            line: String(format: L("连接已断开，%.0f 秒后重连…"), delay), mark: "e")
        reconnectPending = true
        reconnectTimer = Timer.scheduledTimer(withTimeInterval: delay,
                                              repeats: false) { [weak self] _ in
            self?.reconnectNow()
        }
    }
    func reconnectNow() {
        reconnectPending = false
        reconnectTimer?.invalidate()
        guard !stopped else { return }
        if let s = sess, let vt = s.stealVt() { reuseVt = vt }
        if let s = sess { s.free_(); sess = nil }
        start(rows: 24, cols: 80)
    }

    func cancel() {
        stopped = true
        reconnectPending = false
        reconnectTimer?.invalidate()
        if awaitingReply { awaitingReply = false }
        sess?.close()
    }
    func join() {
        reconnectTimer?.invalidate()
        // dispose 对齐：销毁时仍开着的日志行收尾（tab 关闭/退出）
        logFinish(wasConnected ? "ok" : "cancelled", nil)
        sess?.free_(); sess = nil
    }

    deinit { join() }

    // ---------- vault 日志 ----------
    private func logStart() {
        var l = hb_log()
        var id: Int64 = 0
        let u = strdup(huser)
        let hn = strdup(hname)
        let hid = strdup(hostId ?? "")
        let pr = strdup(mode == .sftp ? "SFTP" :
                        mode == .forward ? "SSH/Forward" : "SSH")
        let res = strdup("connecting")
        l.user = u.map { UnsafePointer($0) }
        l.host = hn.map { UnsafePointer($0) }
        l.host_id = hid.map { UnsafePointer($0) }
        l.proto = pr.map { UnsafePointer($0) }
        l.result = res.map { UnsafePointer($0) }
        l.duration_s = -1; l.saved = false
        if let vv = v.vault { _ = hb_vault_log_add(vv, &l, &id) }
        for p in [u, hn, hid, pr, res] { free(p) }
        logId = id; t0 = Date()
    }
    private func logFinish(_ result: String, _ reason: String?) {
        guard logId > 0, let vv = v.vault else { logId = 0; return }
        let d = Int64(Date().timeIntervalSince(t0))
        _ = hb_vault_log_finish(vv, logId, result, reason, d)
        logId = 0
    }

    private func errTitle(_ c: Int32, _ fallback: String) -> String {
        switch UInt32(bitPattern: c) {
        case HB_SSH_ERR_RESOLVE.rawValue: return L("无法解析主机名")
        case HB_SSH_ERR_REFUSED.rawValue: return L("连接被拒绝")
        case HB_SSH_ERR_UNREACHABLE.rawValue: return L("主机不可达")
        case HB_SSH_ERR_TIMEOUT.rawValue: return L("连接超时")
        case HB_SSH_ERR_HANDSHAKE.rawValue: return L("SSH 握手失败")
        case HB_SSH_ERR_HOSTKEY_REJECTED.rawValue: return L("已拒绝未知主机密钥")
        case HB_SSH_ERR_HOSTKEY_CHANGED.rawValue: return L("主机密钥已变更")
        case HB_SSH_ERR_AUTH.rawValue: return L("认证失败")
        case HB_SSH_ERR_CHANNEL.rawValue: return L("无法建立通道")
        case HB_SSH_ERR_DISCONNECTED.rawValue: return L("连接已断开")
        case HB_SSH_ERR_CANCELLED.rawValue: return L("连接已取消")
        case HB_SSH_ERR_FORWARD.rawValue: return L("端口转发建立失败")
        default: return fallback.isEmpty ? L("连接失败") : fallback
        }
    }
}
