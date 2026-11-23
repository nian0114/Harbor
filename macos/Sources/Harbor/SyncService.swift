import AppKit
import CHarbor
import HarborKit

/// M6 账号同步服务（FR-11）——逐行对齐 linux/src/hb-sync.c。
/// 平台层只负责传输（URLSession/URLSessionWebSocketTask）、凭据持久化
/// （Keychain，按数据目录作用域，同 DEK）、状态机与定时器；
/// 所有密码学/合并/JSON body/游标/脏队列都在 core（harbor/sync.h）。
final class SyncService {
    static let shared = SyncService()

    enum State: Int {
        case signedOut = 0, syncing = 1, idle = 2, offline = 3, error = 4
    }

    // ---- 常量（对齐 Linux）----
    private static let pollSec: TimeInterval = 30
    private static let pushDebounce: TimeInterval = 1.0
    private static let pushRetryMax = 3
    private static let watchdogSec: TimeInterval = 60
    private static let wsCheckSec: TimeInterval = 15
    private static let wsPingSec: TimeInterval = 15
    private static let wsSilenceSec: TimeInterval = 75
    private static let reqTimeout: TimeInterval = 30

    // ---- 状态 ----
    private(set) var state: State = .signedOut
    private var statusTxt: String?
    private var lastErrorStr: String?
    private(set) var lastSyncTime: Int64 = 0
    private(set) var totpEnabled = false
    private(set) var mergePending = false
    private var busy = false          // 一轮同步进行中
    private var kickPending = false
    private var pushRoundN = 0
    private var opCount = 0
    private var gen = 0               // 登出即 +1；各 op/回调先比对
    private(set) var sync: OpaquePointer?   // hb_sync*

    // 秘密值：全部放堆上 C 缓冲，可 memset_s 擦除（String 无法擦除）。
    private var vk: UnsafeMutablePointer<UInt8>?    // 32B
    private var token: UnsafeMutablePointer<CChar>?
    private var serverUrl: String?
    private var hasVk = false

    // ---- 传输 ----
    private let session: URLSession
    private var roundTasks: [URLSessionDataTask] = []
    private var wsSession: URLSession!
    private var wsTask: URLSessionWebSocketTask?
    private var wsOpen = false
    private var wsConnecting = false
    private var wsBackoff = 0
    private var wsLastRx = Date.distantPast

    // ---- 定时器 ----
    private var pollTimer: Timer?
    private var watchdog: DispatchWorkItem?
    private var wsCheckTimer: Timer?
    private var wsPingTimer: Timer?
    private var wsRetryWork: DispatchWorkItem?
    private var debounceWork: DispatchWorkItem?

    var isBusy: Bool { opCount > 0 || busy }

    private init() {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = Self.reqTimeout
        session = URLSession(configuration: cfg)
        let wcfg = URLSessionConfiguration.ephemeral
        wcfg.timeoutIntervalForRequest = Self.reqTimeout
        wsSession = URLSession(configuration: wcfg)
        if let v = Vault.shared.vault {
            sync = hb_sync_new(v)
        }
        // 对象写入 → 1s 去抖 → kick（对齐 store "changed" → debounce）
        NotificationCenter.default.addObserver(
            forName: .hbVaultChanged, object: nil, queue: .main) {
            [weak self] _ in
            self?.onStoreChanged()
        }
        tryResume()
    }

    // ================= 状态发射 =================

    private func setState(_ st: State) {
        state = st
        emitChanged()
    }
    private func emitChanged() {
        // 异步投递：init→tryResume→setState 路径中观察者若再访问
        // .shared 会在 dispatch_once 内自递归（实测 EXC_BAD_INSTRUCTION）。
        DispatchQueue.main.async {
            NotificationCenter.default.post(
                name: .hbSyncChanged, object: nil)
        }
    }

    var statusText: String {
        if let t = statusTxt { return t }
        switch state {
        case .signedOut: return L("未登录")
        case .syncing:   return L("同步中…")
        case .offline:   return L("离线 · 重试中")
        case .error:     return lastErrorStr ?? L("错误")
        case .idle:      return L("已同步")
        }
    }

    var email: String? {
        guard let s = sync else { return nil }
        var aid: UnsafeMutablePointer<CChar>?
        var em: UnsafeMutablePointer<CChar>?
        var srv: UnsafeMutablePointer<CChar>?
        var dev: UnsafeMutablePointer<CChar>?
        _ = hb_sync_get_account(s, &aid, &em, &srv, &dev)
        defer { free(aid); free(em); free(srv); free(dev) }
        return em.map { String(cString: $0) }
    }
    var server: String? { serverUrl }
    var lastError: String? { lastErrorStr }
    var lastSync: Int64 { lastSyncTime }
    var conflictCount: Int {
        sync.map { hb_sync_conflict_count($0) } ?? 0
    }
    var deviceName: String { SyncKit.deviceName() }
    var deviceId: String? {
        guard let s = sync, state != .signedOut else { return nil }
        var id: UnsafeMutablePointer<CChar>?
        _ = hb_sync_get_account(s, nil, nil, nil, &id)
        defer { free(id) }
        return id.map { String(cString: $0) }
    }
    func vkCopy(_ out: UnsafeMutablePointer<UInt8>) -> Bool {
        guard hasVk, let vk, state != .signedOut else { return false }
        out.update(from: vk, count: 32)
        return true
    }

    // ================= 秘密值工具 =================

    private func wipeToken() {
        if let t = token {
            let n = strlen(t)
            _ = memset_s(t, n, 0, n)
            free(t)
            token = nil
        }
    }
    /// 供 ShareService 复用（FR-12：同一账号/服务器/认证）。
    func tokenString() -> String? {
        token.map { String(cString: $0) }
    }

    // ================= 凭据持久化（Keychain，按数据目录作用域） =================
    // 对齐 Linux creds_save/load/wipe：payload = "b64(vk).<token>"，整块
    // 擦除后释放（修复 Linux 曾先 split 再擦导致 token 残片存活的 bug）。

    private var credsAccount: String {
        "sync-creds:" + Vault.shared.dataDir.path
    }
    private var credsFile: URL {
        Vault.shared.dataDir.appendingPathComponent("sync.key")
    }

    private func credsSave() {
        guard let vk, let tk = tokenString() else { return }
        let b64 = Data(UnsafeBufferPointer(start: vk, count: 32))
            .base64EncodedString()
        let payload = b64 + "." + tk
        let d = payload.data(using: .utf8)!
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "dev.harbor.Harbor.sync",
            kSecAttrAccount as String: credsAccount,
        ]
        SecItemDelete(q as CFDictionary)
        var a = q
        a[kSecAttrAccessible as String] =
            kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        a[kSecValueData as String] = d
        let st = SecItemAdd(a as CFDictionary, nil)
        if st != errSecSuccess {
            // Keychain 不可用 → 0600 文件回退（与 DEK 回退同策略）
            try? d.write(to: credsFile, options: .atomic)
            try? FileManager.default.setAttributes(
                [.posixPermissions: 0o600], ofItemAtPath: credsFile.path)
        }
    }

    /// 返回堆上 payload C 串（调用方负责整块擦除+free）；失败 nil。
    private func credsLoad() -> UnsafeMutablePointer<CChar>? {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "dev.harbor.Harbor.sync",
            kSecAttrAccount as String: credsAccount,
            kSecReturnData as String: true,
        ]
        var item: CFTypeRef?
        let st = SecItemCopyMatching(q as CFDictionary, &item)
        if st == errSecSuccess, let d = item as? Data,
           let s = String(data: d, encoding: .utf8) {
            return strdup(s)
        }
        if let d = try? Data(contentsOf: credsFile),
           let s = String(data: d, encoding: .utf8) {
            return strdup(s)
        }
        return nil
    }

    private func credsWipe() {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "dev.harbor.Harbor.sync",
            kSecAttrAccount as String: credsAccount,
        ]
        SecItemDelete(q as CFDictionary)
        try? FileManager.default.removeItem(at: credsFile)
    }

    // ================= HTTP =================

    /// 对齐 Linux http()：Bearer + JSON body + authed 错误检查
    /// （device_revoked/unauthorized → sign_out_local + 通知）。
    @discardableResult
    private func http(_ server: String?, _ method: String, _ path: String,
                      _ body: String?, authed: Bool, round: Bool = false,
                      cb: @escaping (Int, String?, [String: Any]?) -> Void)
        -> URLSessionDataTask? {
        guard let server, !server.isEmpty,
              let url = URL(string: server + path)
        else { cb(0, nil, nil); return nil }
        var req = URLRequest(url: url)
        req.httpMethod = method
        req.timeoutInterval = Self.reqTimeout
        if let t = tokenString() {
            req.setValue("Bearer \(t)", forHTTPHeaderField: "Authorization")
        }
        if let b = body {
            req.httpBody = b.data(using: .utf8)
            req.setValue("application/json",
                         forHTTPHeaderField: "Content-Type")
        }
        var task: URLSessionDataTask!
        task = session.dataTask(with: req) { [weak self] data, resp, err in
            DispatchQueue.main.async {
                guard let self else { return }
                if round, let i = self.roundTasks.firstIndex(
                    of: task) { self.roundTasks.remove(at: i) }
                guard err == nil, let data,
                      let r = resp as? HTTPURLResponse
                else { cb(0, nil, nil); return }
                let txt = String(data: data, encoding: .utf8) ?? ""
                var obj: [String: Any]?
                if let o = try? JSONSerialization.jsonObject(with: data),
                   let d = o as? [String: Any] { obj = d }
                if authed, let code = obj?["error"] as? String {
                    if code == "device_revoked" {
                        let wipe = (obj?["wipe"] as? Bool) ?? false
                        self.signOutLocal(wipe, nil)
                        NotifyCenter.shared.post(
                            kind: "err", title: L("此设备已被吊销"),
                            body: wipe ? L("账号已退出，本地数据已擦除")
                                       : L("账号已退出"),
                            tabIndex: nil)
                        return
                    }
                    if code == "unauthorized" {
                        self.signOutLocal(false, L("登录已失效，请重新登录"))
                        return
                    }
                }
                cb(r.statusCode, txt, obj)
            }
        }
        if round { roundTasks.append(task) }
        task.resume()
        return task
    }

    private func jstr(_ o: [String: Any]?, _ k: String) -> String? {
        o?[k] as? String
    }

    // ================= 操作框架 =================

    struct OpResult {
        var ok = false
        var code: String?
        var message: String?
        var retryAfter = 0
        var words: String?
        var needMerge = false
        var devices: [SyncKit.DeviceInfo]?
        var totpSecret: String?
        var totpUri: String?
    }

    /// 对齐 OpCtx/SimpOp：所有秘密输入/输出为 C 串副本，释放时擦除；
    /// 只触碰副本，从不引用服务侧 live 秘密。
    private final class SyncOp {
        enum Kind { case login, registerOp, recover, rotate,
                    password, simple }
        weak var svc: SyncService?
        let kind: Kind
        var what = 0                 // simple: 0 setup 1 enable 2 disable
                                     //        3 devices 4 revoke
        var server: UnsafeMutablePointer<CChar>?
        var email: UnsafeMutablePointer<CChar>?
        var password: UnsafeMutablePointer<CChar>?
        var totp: UnsafeMutablePointer<CChar>?
        var newPassword: UnsafeMutablePointer<CChar>?
        var wordsIn: UnsafeMutablePointer<CChar>?
        var saltBuf = [UInt8](repeating: 0, count: 16)
        var ops: UInt64 = 0, mem: UInt64 = 0
        var wrapKey = [UInt8](repeating: 0, count: 32)
        var body: UnsafeMutablePointer<CChar>?
        var accountId: UnsafeMutablePointer<CChar>?
        var devId: UnsafeMutablePointer<CChar>?
        var token: UnsafeMutablePointer<CChar>?
        var totpEnabled = false
        var recoveryToken: UnsafeMutablePointer<CChar>?
        var outWords: UnsafeMutablePointer<CChar>?
        var vkIn: UnsafeMutablePointer<UInt8>?    // 32B 主线程快照
        var vkOut: UnsafeMutablePointer<UInt8>?   // 32B worker 产出
        var gen = 0
        var err: hb_err = HB_OK
        var code: UnsafeMutablePointer<CChar>?
        var deviceId: UnsafeMutablePointer<CChar>?
        var wipe = false
        var totpSecret: UnsafeMutablePointer<CChar>?
        var totpUri: UnsafeMutablePointer<CChar>?
        var devices: [SyncKit.DeviceInfo]?
        var cb: ((OpResult) -> Void)?

        init(_ s: SyncService, _ k: Kind,
             _ cb: ((OpResult) -> Void)?) {
            svc = s; kind = k; self.cb = cb; gen = s.gen
        }

        private func wf(_ p: inout UnsafeMutablePointer<CChar>?) {
            if let q = p {
                _ = memset_s(q, strlen(q), 0, strlen(q))
                free(q); p = nil
            }
        }
        private func gf(_ p: inout UnsafeMutablePointer<CChar>?) {
            free(p); p = nil
        }
        deinit {
            gf(&server); gf(&email); gf(&devId); gf(&deviceId); gf(&totpUri)
            wf(&password); wf(&totp); wf(&newPassword); wf(&wordsIn)
            wf(&body); wf(&accountId); wf(&token); wf(&recoveryToken)
            wf(&outWords); wf(&code); wf(&totpSecret)
            if let v = vkIn { _ = memset_s(v, 32, 0, 32); free(v) }
            if let v = vkOut { _ = memset_s(v, 32, 0, 32); free(v) }
            _ = memset_s(&wrapKey, 32, 0, 32)
            _ = memset_s(&saltBuf, 16, 0, 16)
        }

        func result(_ ok: Bool, _ code: String?, _ msg: String?, _ ra: Int) {
            var r = OpResult(ok: ok, code: code, message: msg,
                             retryAfter: ra)
            if let w = outWords {
                r.words = String(cString: w)
                _ = memset_s(w, strlen(w), 0, strlen(w)); free(w)
                outWords = nil
            }
            r.devices = devices; devices = nil
            if let t = totpSecret {
                r.totpSecret = String(cString: t)
                _ = memset_s(t, strlen(t), 0, strlen(t)); free(t)
                totpSecret = nil
            }
            if let u = totpUri {
                r.totpUri = String(cString: u); free(u); totpUri = nil
            }
            cb?(r)
        }

        func end() {
            guard let s = svc else { return }
            s.opCount -= 1
            s.liveOps.removeValue(forKey: ObjectIdentifier(self))
            s.emitChanged()
            svc = nil          // 最后一个引用释放 → deinit 擦除
        }
    }

    /// 持有进行中的 op（对齐 Linux 请求异步回调 ud 的所有权）。
    private var liveOps: [ObjectIdentifier: SyncOp] = [:]

    private func newOp(_ k: SyncOp.Kind,
                       _ cb: ((OpResult) -> Void)?) -> SyncOp {
        let o = SyncOp(self, k, cb)
        liveOps[ObjectIdentifier(o)] = o
        opCount += 1
        emitChanged()
        return o
    }

    private func opFail(_ o: SyncOp, _ st: Int, _ obj: [String: Any]?) {
        if st == 0 {
            o.result(false, "network", L("无法连接服务器"), 0)
            return
        }
        let code = jstr(obj, "error")
        var msg = jstr(obj, "message")
        if st == 429 {
            let ra = (obj?["retry_after"] as? Int)
                ?? Int((obj?["retry_after"] as? String) ?? "") ?? 0
            msg = msg ?? L("请求过于频繁，请稍后再试")
            o.result(false, "rate_limited", msg, ra)
            return
        }
        if code == "totp_required" {
            o.result(false, "totp_required", L("需要两步验证码"), 0); return
        }
        if code == "totp_invalid" {
            o.result(false, "totp_invalid", L("两步验证码错误"), 0); return
        }
        if code == "invalid_credentials" {
            o.result(false, "invalid_credentials",
                     L("邮箱、密码或恢复码错误"), 0); return
        }
        if code == "email_taken" {
            o.result(false, "email_taken", L("该邮箱已注册"), 0); return
        }
        o.result(false, code ?? "server_error",
                 msg ?? L("服务器错误"), 0)
    }

    // ================= login =================

    func login(server: String, email: String, password: String,
               totp: String?, cb: @escaping (OpResult) -> Void) {
        let o = newOp(.login, cb)
        o.server = strdup(server)
        guard let em = SyncKit.normEmail(email) else {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        o.email = strdup(em)
        o.password = strdup(SyncKit.nfkc(password))
        if let tp = totp, !tp.isEmpty { o.totp = strdup(tp) }
        var body: UnsafeMutablePointer<CChar>?
        guard em.withCString({ hb_sync_prelogin_body($0, &body) }) == HB_OK,
              let b = body
        else {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        http(server, "POST", "/v1/prelogin",
             String(cString: b), authed: false) { [weak self, weak o] st, txt, obj in
            free(b)
            guard let self, let o else { return }
            if st == 200, let t = txt,
               o.saltBuf.withUnsafeMutableBufferPointer({ sp in
                   hb_sync_prelogin_parse(t, sp.baseAddress, &o.ops, &o.mem)
               }) == HB_OK {
                self.scheduleWork(o) { self.loginAfterWork(o) }
                return
            }
            self.opFail(o, st, obj)
            o.end()
        }
    }

    private func loginAfterWork(_ o: SyncOp) {
        if o.err != HB_OK || o.gen != gen {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        let body = String(cString: o.body!)
        _ = memset_s(o.body!, strlen(o.body!), 0, strlen(o.body!))
        free(o.body!); o.body = nil
        let srv = String(cString: o.server!)
        http(srv, "POST", "/v1/login", body, authed: false) {
            [weak self, weak o] st, txt, obj in
            guard let self, let o else { return }
            if st == 200, let t = txt {
                if o.vkOut == nil { o.vkOut = .allocate(capacity: 32) }
                var te = false
                let e = o.wrapKey.withUnsafeBufferPointer { wk in
                    hb_sync_login_parse(t, wk.baseAddress, o.vkOut,
                                        &o.accountId, &o.devId, &o.token,
                                        &te)
                }
                o.totpEnabled = te
                if e == HB_OK && o.gen == self.gen {
                    self.finishSignin(o); return
                }
            }
            if st == 200 {
                o.result(false, "server_error", L("服务器响应异常"), 0)
            } else {
                self.opFail(o, st, obj)
            }
            o.end()
        }
    }

    // ================= register =================

    func register(server: String, email: String, password: String,
                  cb: @escaping (OpResult) -> Void) {
        let o = newOp(.registerOp, cb)
        o.vkOut = .allocate(capacity: 32)
        o.server = strdup(server)
        guard let em = SyncKit.normEmail(email) else {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        o.email = strdup(em)
        o.password = strdup(SyncKit.nfkc(password))
        scheduleWork(o) { self.registerAfterWork(o) }
    }

    private func registerAfterWork(_ o: SyncOp) {
        if o.err != HB_OK || o.gen != gen {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        let body = String(cString: o.body!)
        _ = memset_s(o.body!, strlen(o.body!), 0, strlen(o.body!))
        free(o.body!); o.body = nil
        let srv = String(cString: o.server!)
        http(srv, "POST", "/v1/register", body, authed: false) {
            [weak self, weak o] st, txt, obj in
            guard let self, let o else { return }
            if st == 200, let t = txt,
               hb_sync_auth_resp_parse(t, &o.accountId, &o.devId,
                                       &o.token) == HB_OK {
                self.finishSignin(o); return
            }
            if st == 200 {
                o.result(false, "server_error", L("服务器响应异常"), 0)
            } else {
                self.opFail(o, st, obj)
            }
            o.end()
        }
    }

    // ================= recover =================

    func recover(server: String, email: String, words: String,
                 newPassword: String, cb: @escaping (OpResult) -> Void) {
        let o = newOp(.recover, cb)
        o.server = strdup(server)
        guard let em = SyncKit.normEmail(email) else {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        o.email = strdup(em)
        o.wordsIn = strdup(words)
        o.newPassword = strdup(SyncKit.nfkc(newPassword))
        var body: UnsafeMutablePointer<CChar>?
        let ok = em.withCString { ep in
            hb_sync_recover_start_body(ep, words, &body)
        }
        guard ok == HB_OK, let b = body else {
            o.result(false, "invalid_credentials", L("恢复码无效"), 0)
            o.end(); return
        }
        http(server, "POST", "/v1/recover/start",
             String(cString: b), authed: false) { [weak self, weak o] st, txt, obj in
            free(b)
            guard let self, let o else { return }
            if st == 200, let t = txt {
                if o.vkOut == nil { o.vkOut = .allocate(capacity: 32) }
                let e = hb_sync_recover_start_parse(
                    t, o.wordsIn, o.vkOut, &o.recoveryToken)
                if e == HB_OK {
                    self.scheduleWork(o) { self.recoverAfterWork(o) }
                    return
                }
            }
            if st == 200 {
                o.result(false, "server_error", L("服务器响应异常"), 0)
            } else {
                self.opFail(o, st, obj)
            }
            o.end()
        }
    }

    private func recoverAfterWork(_ o: SyncOp) {
        if o.err != HB_OK || o.gen != gen {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        let body = String(cString: o.body!)
        _ = memset_s(o.body!, strlen(o.body!), 0, strlen(o.body!))
        free(o.body!); o.body = nil
        http(serverUrl ?? "", "POST", "/v1/recover/finish",
             body, authed: false) { [weak self, weak o] st, txt, obj in
            guard let self, let o else { return }
            if st == 200, let t = txt,
               hb_sync_auth_resp_parse(t, &o.accountId, &o.devId,
                                       &o.token) == HB_OK {
                self.finishSignin(o); return
            }
            if st == 200 {
                o.result(false, "server_error", L("服务器响应异常"), 0)
            } else {
                self.opFail(o, st, obj)
            }
            o.end()
        }
    }

    // ================= rotate / password =================

    func rotateRecovery(password: String,
                        cb: @escaping (OpResult) -> Void) {
        let o = newOp(.rotate, cb)
        o.password = strdup(SyncKit.nfkc(password))
        if let vk {
            o.vkIn = .allocate(capacity: 32)
            o.vkIn!.update(from: vk, count: 32)
        }
        guard let em = accountEmail() else {
            o.result(false, "server_error", L("未登录"), 0); o.end(); return
        }
        o.email = strdup(em)
        authedPrelogin(o)
    }

    func changePassword(old: String, new: String,
                        cb: @escaping (OpResult) -> Void) {
        let o = newOp(.password, cb)
        o.password = strdup(SyncKit.nfkc(old))
        o.newPassword = strdup(SyncKit.nfkc(new))
        if let vk {
            o.vkIn = .allocate(capacity: 32)
            o.vkIn!.update(from: vk, count: 32)
        }
        guard let em = accountEmail() else {
            o.result(false, "server_error", L("未登录"), 0); o.end(); return
        }
        o.email = strdup(em)
        authedPrelogin(o)
    }

    private func accountEmail() -> String? {
        guard let s = sync else { return nil }
        var aid: UnsafeMutablePointer<CChar>?
        var em: UnsafeMutablePointer<CChar>?
        var srv: UnsafeMutablePointer<CChar>?
        var dev: UnsafeMutablePointer<CChar>?
        _ = hb_sync_get_account(s, &aid, &em, &srv, &dev)
        defer { free(aid); free(em); free(srv); free(dev) }
        return em.map { String(cString: $0) }
    }

    private func authedPrelogin(_ o: SyncOp) {
        var body: UnsafeMutablePointer<CChar>?
        guard o.email != nil,
              hb_sync_prelogin_body(o.email!, &body) == HB_OK,
              let b = body
        else {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        http(serverUrl, "POST", "/v1/prelogin",
             String(cString: b), authed: false) { [weak self, weak o] st, txt, obj in
            free(b)
            guard let self, let o else { return }
            if st == 200, let t = txt,
               o.saltBuf.withUnsafeMutableBufferPointer({ sp in
                   hb_sync_prelogin_parse(t, sp.baseAddress, &o.ops, &o.mem)
               }) == HB_OK {
                self.scheduleWork(o) {
                    if o.kind == .rotate { self.rotateAfterWork(o) }
                    else { self.passwordAfterWork(o) }
                }
                return
            }
            self.opFail(o, st, obj)
            o.end()
        }
    }

    private func rotateAfterWork(_ o: SyncOp) {
        if o.err != HB_OK || o.gen != gen {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        let body = String(cString: o.body!)
        _ = memset_s(o.body!, strlen(o.body!), 0, strlen(o.body!))
        free(o.body!); o.body = nil
        simplePost(o, "/v1/recovery", body)
    }

    private func passwordAfterWork(_ o: SyncOp) {
        if o.err != HB_OK || o.gen != gen {
            o.result(false, "server_error", L("内部错误"), 0); o.end(); return
        }
        let body = String(cString: o.body!)
        _ = memset_s(o.body!, strlen(o.body!), 0, strlen(o.body!))
        free(o.body!); o.body = nil
        simplePost(o, "/v1/password", body)
    }

    private func simplePost(_ o: SyncOp, _ path: String, _ body: String) {
        http(serverUrl, "POST", path, body, authed: true) {
            [weak self, weak o] st, _, obj in
            guard let self, let o else { return }
            if st == 200 { o.result(true, nil, nil, 0) }
            else { self.opFail(o, st, obj) }
            o.end()
        }
    }

    // ================= 简单 authed ops =================

    func totpSetup(cb: @escaping (OpResult) -> Void) {
        let o = newOp(.simple, cb); o.what = 0; simpGo(o)
    }
    func totpEnable(_ code: String, cb: @escaping (OpResult) -> Void) {
        let o = newOp(.simple, cb); o.what = 1; o.code = strdup(code)
        simpGo(o)
    }
    func totpDisable(_ code: String, cb: @escaping (OpResult) -> Void) {
        let o = newOp(.simple, cb); o.what = 2; o.code = strdup(code)
        simpGo(o)
    }
    func devices(cb: @escaping (OpResult) -> Void) {
        let o = newOp(.simple, cb); o.what = 3; simpGo(o)
    }
    func revoke(_ deviceId: String, wipe: Bool,
                cb: ((OpResult) -> Void)?) {
        let o = newOp(.simple, cb); o.what = 4
        o.deviceId = strdup(deviceId); o.wipe = wipe
        simpGo(o)
    }

    private func simpGo(_ o: SyncOp) {
        var method = "POST", path = "", body: String? = nil
        switch o.what {
        case 0: path = "/v1/totp/setup"; body = "{}"
        case 1: path = "/v1/totp/enable"
            body = "{\"code\":\"\(String(cString: o.code!))\"}"
        case 2: path = "/v1/totp/disable"
            body = "{\"code\":\"\(String(cString: o.code!))\"}"
        case 3: method = "GET"; path = "/v1/devices"
        case 4: path = "/v1/devices/\(String(cString: o.deviceId!))/revoke"
            body = "{\"wipe\":\(o.wipe ? "true" : "false")}"
        default: o.end(); return
        }
        http(serverUrl, method, path, body, authed: true) {
            [weak self, weak o] st, txt, obj in
            guard let self, let o else { return }
            if st == 0 {
                o.result(false, "network", L("无法连接服务器"), 0)
                o.end(); return
            }
            if o.what == 0 && st == 200 {
                if let s = self.jstr(obj, "secret") { o.totpSecret = strdup(s) }
                if let u = self.jstr(obj, "uri") { o.totpUri = strdup(u) }
                o.result(true, nil, nil, 0); o.end(); return
            }
            if o.what == 3 && st == 200 {
                o.devices = SyncKit.parseDevices(txt ?? "")
                o.result(true, nil, nil, 0); o.end(); return
            }
            if st == 200 {
                if o.what == 1 { self.totpEnabled = true }
                if o.what == 2 { self.totpEnabled = false }
                o.result(true, nil, nil, 0); o.end(); return
            }
            let code = self.jstr(obj, "error")
            var msg = self.jstr(obj, "message")
            var ra = 0
            if st == 429 {
                ra = (obj?["retry_after"] as? Int)
                    ?? Int((obj?["retry_after"] as? String) ?? "") ?? 0
            }
            msg = msg ?? L("操作失败")
            o.result(false, code ?? "server_error", msg, ra)
            o.end()
        }
    }

    // ================= worker =================

    /// 对齐 Linux op_worker：Argon2 重的 body 构建在后台队列（不碰 vault）。
    private func scheduleWork(_ o: SyncOp, _ then: @escaping () -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            self.opWorker(o)
            DispatchQueue.main.async { then() }
        }
    }

    private func opWorker(_ o: SyncOp) {
        let host = SyncKit.deviceName()
        host.withCString { hn in
            switch o.kind {
            case .login:
                o.err = hb_sync_login_body(
                    o.email, o.password, o.saltBuf, o.ops, o.mem,
                    o.totp, hn, SyncKit.platform, &o.body, &o.wrapKey)
            case .registerOp:
                o.err = hb_sync_register_body(
                    o.email, o.password, hn, SyncKit.platform, 0, 0,
                    &o.body, &o.outWords, o.vkOut)
            case .recover:
                o.err = hb_sync_recover_finish_body(
                    o.recoveryToken, o.newPassword, 0, 0, hn,
                    SyncKit.platform, o.vkOut, &o.body)
            case .rotate:
                o.err = o.saltBuf.withUnsafeBufferPointer { sp in
                    hb_sync_recovery_rotate_body_vk(
                        o.vkIn, o.password, sp.baseAddress, o.ops, o.mem,
                        &o.body, &o.outWords)
                }
            case .password:
                o.err = o.saltBuf.withUnsafeBufferPointer { sp in
                    hb_sync_change_password_body(
                        o.password, sp.baseAddress, o.ops, o.mem,
                        o.newPassword, 0, 0, o.vkIn, &o.body)
                }
            case .simple:
                o.err = HB_OK
            }
        }
    }

    // ================= 会话建立 =================

    private func ensureVk() -> UnsafeMutablePointer<UInt8> {
        if vk == nil { vk = .allocate(capacity: 32) }
        return vk!
    }

    private func hasLocalScopeData() -> Bool {
        for t in ["host", "group", "key", "snippet", "forward", "known",
                  "settings"] {
            if !Vault.shared.list(t).isEmpty { return true }
        }
        return false
    }

    /// 对齐 finish_signin：代校验 → 装 VK/token/账号 → 存凭据 →
    /// IDLE → （login/recover 且本机有数据）need_merge → 否则立即起同步。
    private func finishSignin(_ o: SyncOp) {
        if o.gen != gen || o.vkOut == nil { o.end(); return }
        let v = ensureVk()
        v.update(from: o.vkOut!, count: 32)
        hasVk = true
        if let s = sync { _ = hb_sync_set_vk(s, o.vkOut) }
        _ = memset_s(o.vkOut!, 32, 0, 32); free(o.vkOut!); o.vkOut = nil
        wipeToken()
        token = o.token; o.token = nil
        serverUrl = o.server.map { String(cString: $0) }
        if serverUrl == nil { serverUrl = self.server }
        if let s = sync {
            serverUrl?.withCString { sv in
                _ = hb_sync_set_account(s, o.accountId, o.email,
                                        sv, o.devId)
            }
        }
        totpEnabled = o.totpEnabled
        credsSave()
        statusTxt = nil
        setState(.idle)
        let needMerge = (o.kind == .login || o.kind == .recover)
            && hasLocalScopeData()
        if needMerge {
            mergePending = true
            var r = OpResult(ok: true); r.needMerge = true
            o.cb?(r)
        } else {
            startTriggers()
            wsConnect()
            syncKick()
            o.result(true, nil, nil, 0)
        }
        o.end()
    }

    // ================= 同步循环 =================

    private func watchdogDisarm() {
        watchdog?.cancel(); watchdog = nil
    }

    private func loopOffline(_ why: String) {
        watchdogDisarm()
        busy = false
        kickPending = true
        lastErrorStr = why
        statusTxt = L("离线 · 重试中")
        setState(.offline)
    }

    private func finishRound() {
        watchdogDisarm()
        busy = false
        lastSyncTime = Int64(Date().timeIntervalSince1970)
        lastErrorStr = nil
        statusTxt = nil
        setState(.idle)
        if kickPending {
            kickPending = false
            syncKick()
        }
    }

    private func syncKick() {
        if state == .signedOut || !hasVk || token == nil { return }
        if busy { kickPending = true; return }
        busy = true
        watchdogDisarm()
        let w = DispatchWorkItem { [weak self] in
            guard let self, self.busy else { return }
            // 看门狗：整轮超时 → 取消本轮请求 → 离线重试
            for t in self.roundTasks { t.cancel() }
            self.roundTasks.removeAll()
            self.loopOffline(L("同步超时"))
        }
        watchdog = w
        DispatchQueue.main.asyncAfter(
            deadline: .now() + Self.watchdogSec, execute: w)
        statusTxt = L("同步中…")
        setState(.syncing)
        pullStep()
    }

    private func pullStep() {
        guard let s = sync else { loopOffline(L("内部错误")); return }
        var path: UnsafeMutablePointer<CChar>?
        guard hb_sync_pull_path(s, &path) == HB_OK, let p = path else {
            loopOffline(L("内部错误")); return
        }
        let pth = String(cString: p)
        free(p)
        http(serverUrl, "GET", pth, nil, authed: true, round: true) {
            [weak self] st, txt, _ in
            guard let self else { return }
            if self.state == .signedOut { return }
            if st == 200, let body = txt {
                var ps = hb_sync_pull_stats()
                if hb_sync_apply_pull(self.sync!, body, &ps) == HB_OK {
                    if ps.more { self.pullStep() }
                    else { self.pushRound(0) }
                    return
                }
            }
            self.loopOffline(st == 0 ? L("网络错误") : L("服务器错误"))
        }
    }

    private func pushRound(_ round: Int) {
        guard let s = sync else { finishRound(); return }
        pushRoundN = round
        var p: OpaquePointer?
        guard hb_sync_build_push(s, &p) == HB_OK, let push = p else {
            if let p { hb_sync_push_free(p) }
            finishRound(); return
        }
        let body = String(cString: hb_sync_push_body(push))
        http(serverUrl, "POST", "/v1/objects", body, authed: true,
             round: true) { [weak self] st, txt, _ in
            guard let self else { return }
            if self.state == .signedOut {
                hb_sync_push_free(push); return
            }
            if st == 200, let t = txt {
                var retry = false
                if hb_sync_apply_push(self.sync!, push, t, &retry) == HB_OK {
                    hb_sync_push_free(push)
                    if retry && self.pushRoundN < Self.pushRetryMax {
                        self.pushRound(self.pushRoundN + 1)
                    } else {
                        self.finishRound()
                    }
                    return
                }
            }
            hb_sync_push_free(push)
            self.loopOffline(st == 0 ? L("网络错误") : L("服务器错误"))
        }
    }

    // ================= WebSocket =================

    private func wsUrl() -> String? {
        guard let u = serverUrl else { return nil }
        if u.hasPrefix("https://") {
            return "wss://" + u.dropFirst(8) + "/v1/ws"
        }
        if u.hasPrefix("http://") {
            return "ws://" + u.dropFirst(7) + "/v1/ws"
        }
        return nil
    }

    private func wsConnect() {
        if wsConnecting || wsOpen || token == nil
            || state == .signedOut { return }
        guard let us = wsUrl(), let url = URL(string: us) else { return }
        var req = URLRequest(url: url)
        req.timeoutInterval = Self.reqTimeout
        if let t = tokenString() {
            req.setValue("Bearer \(t)", forHTTPHeaderField: "Authorization")
        }
        wsConnecting = true
        let task = wsSession.webSocketTask(with: req)
        wsTask = task
        task.resume()
        wsConnecting = false
        wsOpen = true
        wsBackoff = 0
        wsLastRx = Date()
        wsReceive()
        // 对齐 libsoup keepalive + ws_check：15s ping，75s 静默判死
        if wsPingTimer == nil {
            wsPingTimer = Timer.scheduledTimer(
                withTimeInterval: Self.wsPingSec, repeats: true) {
                [weak self] _ in self?.wsSendPing()
            }
        }
        if wsCheckTimer == nil {
            wsCheckTimer = Timer.scheduledTimer(
                withTimeInterval: Self.wsCheckSec, repeats: true) {
                [weak self] _ in self?.wsCheck()
            }
        }
        emitChanged()
        syncKick()   // 重连后主动对齐
    }

    private func wsReceive() {
        wsTask?.receive { [weak self] res in
            DispatchQueue.main.async {
                guard let self else { return }
                switch res {
                case .success(let msg):
                    self.wsLastRx = Date()
                    var txt = ""
                    if case .string(let s) = msg { txt = s }
                    if txt.contains("\"revoked\"") {
                        let wipe = txt.contains("\"wipe\":true")
                        self.signOutLocal(wipe, nil)
                        NotifyCenter.shared.post(
                            kind: "err", title: L("此设备已被吊销"),
                            body: wipe ? L("账号已退出，本地数据已擦除")
                                       : L("账号已退出"),
                            tabIndex: nil)
                        return
                    }
                    // FR-12：relay 推 share 列表变化 → 共享页刷新
                    if txt.contains("\"shares\"") {
                        NotificationCenter.default.post(
                            name: ShareService.nListChanged,
                            object: nil)
                    }
                    if txt.contains("\"changed\"") { self.syncKick() }
                    self.wsReceive()   // 继续收
                case .failure:
                    // wsCloseConn 主动 cancel 也会触发 failure ——
                    // wsOpen 已清则忽略，避免误排重连。
                    guard self.wsOpen else { return }
                    self.wsClosed()
                }
            }
        }
    }

    private func wsSendPing() {
        guard wsOpen else { return }
        wsTask?.sendPing { [weak self] err in
            DispatchQueue.main.async {
                guard let self else { return }
                if err == nil { self.wsLastRx = Date() }
            }
        }
    }

    private func wsCheck() {
        if wsOpen,
           Date().timeIntervalSince(wsLastRx) > Self.wsSilenceSec {
            // 半开连接：判死 → 关闭重排 + 顺手 kick 让同步轮探活
            wsCloseConn()
            wsSchedule()
            syncKick()
        }
    }

    private func wsClosed() {
        wsOpen = false
        wsConnecting = false
        wsTask?.cancel(); wsTask = nil
        if state != .signedOut { wsSchedule() }
        emitChanged()
    }

    private func wsSchedule() {
        if wsRetryWork != nil || state == .signedOut { return }
        let idx = min(wsBackoff, SyncKit.wsRetry.count - 1)
        let d = SyncKit.wsRetry[idx]
        if wsBackoff < SyncKit.wsRetry.count - 1 { wsBackoff += 1 }
        let w = DispatchWorkItem { [weak self] in
            self?.wsRetryWork = nil
            self?.wsConnect()
        }
        wsRetryWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + d, execute: w)
    }

    private func wsCloseConn() {
        wsRetryWork?.cancel(); wsRetryWork = nil
        wsCheckTimer?.invalidate(); wsCheckTimer = nil
        wsPingTimer?.invalidate(); wsPingTimer = nil
        wsConnecting = false
        wsOpen = false
        wsTask?.cancel(); wsTask = nil
    }

    // ================= 触发器 =================

    private func onStoreChanged() {
        if state == .signedOut { return }
        if debounceWork == nil {
            let w = DispatchWorkItem { [weak self] in
                self?.debounceWork = nil
                self?.syncKick()
            }
            debounceWork = w
            DispatchQueue.main.asyncAfter(
                deadline: .now() + Self.pushDebounce, execute: w)
        }
    }

    private func startTriggers() {
        if pollTimer == nil {
            pollTimer = Timer.scheduledTimer(
                withTimeInterval: Self.pollSec, repeats: true) {
                [weak self] _ in
                guard let self else { return }
                // WS 断开或 OFFLINE 时定期重试
                if !self.busy, self.state != .signedOut,
                   (!self.wsOpen || self.state == .offline) {
                    self.syncKick()
                }
            }
        }
    }

    private func stopTriggers() {
        pollTimer?.invalidate(); pollTimer = nil
        debounceWork?.cancel(); debounceWork = nil
    }

    // ================= 登出 / 吊销 =================

    private func signOutLocal(_ wipe: Bool, _ why: String?) {
        wsCloseConn()
        stopTriggers()
        watchdogDisarm()
        for t in roundTasks { t.cancel() }
        roundTasks.removeAll()
        busy = false
        kickPending = false
        mergePending = false
        if let s = sync {
            if wipe { _ = hb_sync_wipe(s) }
            else { _ = hb_sync_logout_local(s) }
        }
        wipeToken()
        serverUrl = nil
        credsWipe()
        if let v = vk { _ = memset_s(v, 32, 0, 32); free(v); vk = nil }
        hasVk = false
        gen += 1
        totpEnabled = false
        if let s = sync { hb_sync_clear_vk(s) }
        lastErrorStr = why
        statusTxt = nil
        setState(.signedOut)
    }

    func logout(_ wipe: Bool) {
        // 尽力通知服务端（响应忽略——本地无论如何登出）
        if token != nil, serverUrl != nil {
            http(serverUrl, "POST", "/v1/logout", "{}",
                 authed: false) { _, _, _ in }
        }
        signOutLocal(wipe, nil)
    }

    func confirmMerge(_ merge: Bool) {
        mergePending = false
        if !merge, let s = sync { _ = hb_sync_wipe(s) }
        startTriggers()
        wsConnect()
        syncKick()
    }

    // ================= 启动恢复 =================

    private func tryResume() {
        guard let s = sync else { return }
        var aid: UnsafeMutablePointer<CChar>?
        var em: UnsafeMutablePointer<CChar>?
        var srv: UnsafeMutablePointer<CChar>?
        var dev: UnsafeMutablePointer<CChar>?
        defer { free(aid); free(em); free(srv); free(dev) }
        guard hb_sync_get_account(s, &aid, &em, &srv, &dev) == HB_OK,
              aid != nil, srv != nil else { return }
        // payload = "b64(vk).<token>"：整块擦除，绝不先 split 再擦
        guard let payload = credsLoad() else { return }
        let totalLen = strlen(payload)
        let dot = strchr(payload, 46)   // '.'
        guard let dot else {
            _ = memset_s(payload, totalLen, 0, totalLen)
            free(payload); return
        }
        let v = ensureVk()
        dot.pointee = 0          // 自有缓冲：就地截断出 b64 段
        let b64 = String(cString: payload)
        guard let vd = Data(base64Encoded: b64), vd.count == 32 else {
            _ = memset_s(payload, totalLen, 0, totalLen)
            free(payload); return
        }
        vd.copyBytes(to: v, count: 32)
        wipeToken()
        token = strdup(dot + 1)
        serverUrl = String(cString: srv!)
        _ = memset_s(payload, totalLen, 0, totalLen)
        free(payload)
        hasVk = true
        _ = hb_sync_set_vk(s, v)
        setState(.idle)
        startTriggers()
        wsConnect()
        syncKick()
    }

    // ================= API =================

    func syncNow() { syncKick() }

    func setScope(_ scope: String, _ on: Bool) {
        guard let s = sync else { return }
        if scope.withCString({ hb_sync_scope_set(s, $0, on) }) == HB_OK {
            syncKick()
        }
    }

    func scopeGet(_ scope: String) -> Bool {
        guard let s = sync else { return false }
        return scope.withCString { hb_sync_scope_get(s, $0) }
    }
}

extension Notification.Name {
    static let hbSyncChanged = Notification.Name("hbSyncChanged")
    static let hbSyncShares = Notification.Name("hbSyncShares")
}
