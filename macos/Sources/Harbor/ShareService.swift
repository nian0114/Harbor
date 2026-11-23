import AppKit
import CHarbor
import HarborKit

/// FR-12 共享服务（对齐 linux/src/hb-share.c）。
/// 传输层：REST（URLSession）+ relay WebSocket（URLSessionWebSocketTask）；
/// 加密/路由全在 core hb_share_owner / hb_share_peer。
/// 凭据经 SyncService 窄访问器取；signed-out 时 owner/peer 均不可用。
/// 永不记录帧、密钥、配对码或 meta 明文。
final class ShareService {
    static let shared = ShareService()

    /// 对齐 hb-share.c 的 GObject signals：
    /// nChanged/nOwnerEvent object = shareId(String)；
    /// nPeerEvent/nPeerEnded object = PeerCtx；userInfo["ev"] = ShareEv。
    static let nChanged = Notification.Name("hb-share-changed")
    static let nOwnerEvent = Notification.Name("hb-share-owner-event")
    static let nPeerEvent = Notification.Name("hb-share-peer-event")
    static let nPeerEnded = Notification.Name("hb-share-peer-ended")
    /// 同账号 share 列表变化（SyncService WS "shares" 推）。
    static let nListChanged = Notification.Name("hb-share-list-changed")

    private static let wsPingSec: TimeInterval = 5
    private static let reqTimeout: TimeInterval = 30

    /// hb_share_event 的 Swift 快照（字符串已拷贝，可安全持有）。
    struct ShareEv {
        var type = hb_share_ev_type(rawValue: 0)   // HB_SH_EV_*
        var id: UInt32 = 0
        var code: Int32 = 0
        var name = ""
        var device = ""
        var text = ""
    }

    struct Part {
        var id: UInt32
        var name: String
        var device: String
        var guest: Bool
    }

    struct ShareItem {
        var shareId: String
        var metaB64: String
        var ownerDevice: String
        var created: Int64
    }

    struct OwnInfo {
        var shareId: String
        var title: String
        var host: String
        var mode: Int
        weak var tab: TermTab?
    }

    // ---------------- ctx ----------------

    private var owners: [String: OwnerCtx] = [:]
    private(set) var peers: [PeerCtx] = []
    /// Session 对象 → share_id（对齐 by_sess）。
    private var bySess: [ObjectIdentifier: String] = [:]
    private let session = URLSession(configuration: .default)

    final class OwnerCtx {
        let shareId: String
        let title: String
        let host: String
        var eng: OpaquePointer?          // hb_share_owner*
        weak var sess: Session?          // borrowed
        weak var tab: TermTab?           // bind_tab（卡片点击切换）
        var ws: URLSessionWebSocketTask?
        var wsOpen = false
        var fdSrc: DispatchSourceRead?
        var tickWork: DispatchWorkItem?
        var pingTimer: Timer?
        var active = false
        var stopping = false
        var parts: [UInt32: Part] = [:]
        var cb: ((String?, String?) -> Void)?
        var dead = false                 // ctx 已释放 → 迟到回调丢弃

        init(id: String, title: String, host: String, eng: OpaquePointer,
             sess: Session, cb: ((String?, String?) -> Void)?) {
            self.shareId = id; self.title = title; self.host = host
            self.eng = eng; self.sess = sess; self.cb = cb
        }
    }

    final class PeerCtx {
        let shareId: String
        let role: String                 // "member"|"guest"
        var eng: OpaquePointer?          // hb_share_peer*
        var ws: URLSessionWebSocketTask?
        var wsOpen = false
        var fdSrc: DispatchSourceRead?
        var tickWork: DispatchWorkItem?
        var pingTimer: Timer?
        var ended = false
        var cb: ((PeerCtx?, String?) -> Void)?
        var dead = false

        init(id: String, role: String, eng: OpaquePointer,
             cb: ((PeerCtx?, String?) -> Void)?) {
            self.shareId = id; self.role = role; self.eng = eng; self.cb = cb
        }

        var vt: OpaquePointer? { eng.flatMap { hb_share_peer_vt($0) } }
        var perm: Int { eng.map { Int(hb_share_peer_perm($0)) } ?? 0 }
        var peerTitle: String? {
            eng.flatMap { hb_share_peer_title($0).map {
                String(cString: $0) } }
        }
        /// hello 之后拿到了自身 id（对齐 ctx joined 判定）。
        var joined: Bool {
            eng.map { hb_share_peer_self_id($0) != 0 } ?? false
        }
        /// 对齐 hb_share_peer_ctx_engine + hb_share_peer_input。
        func input(_ s: String) -> hb_err {
            guard let eng else { return HB_ERR_INVALID }
            return s.withCString {
                hb_share_peer_input(eng, $0, strlen($0))
            }
        }
    }

    private init() {}

    // ---------------- helpers ----------------

    private var sync: SyncService { SyncService.shared }

    private var signedIn: Bool {
        sync.state != .signedOut && sync.tokenString() != nil
    }

    /// http(s):// → ws(s)://（对齐 ws_url()，实现在 ShareKit）。
    static func wsURL(_ server: String, _ pathQ: String) -> String {
        ShareKit.wsURL(server, pathQ)
    }

    /// 带 Bearer 的 JSON REST（对齐 http()）。cb 在主线程。
    private func http(_ method: String, _ path: String, _ json: String?,
                      auth: Bool, cb: ((Int, String?) -> Void)?) {
        guard let server = sync.server,
              let url = URL(string: server + path)
        else { cb?(0, nil); return }
        var req = URLRequest(url: url)
        req.httpMethod = method
        req.timeoutInterval = Self.reqTimeout
        if auth, let t = sync.tokenString() {
            req.setValue("Bearer \(t)", forHTTPHeaderField: "Authorization")
        }
        if let json {
            req.httpBody = json.data(using: .utf8)
            req.setValue("application/json",
                         forHTTPHeaderField: "Content-Type")
        }
        session.dataTask(with: req) { data, resp, _ in
            DispatchQueue.main.async {
                let st = (resp as? HTTPURLResponse)?.statusCode ?? 0
                let txt = data.flatMap {
                    String(data: $0, encoding: .utf8) }
                cb?(st, txt)
            }
        }.resume()
    }

    // ---------------- ws plumbing ----------------

    private func wsSend(_ ws: URLSessionWebSocketTask?,
                        open: Bool, _ f: inout hb_share_frame) {
        guard open, let ws else { return }
        if f.is_text, let d = f.data {
            let s = d.withMemoryRebound(to: CChar.self,
                                        capacity: Int(f.len)) {
                String(cString: $0) }
            ws.send(.string(s)) { _ in }
        } else if let d = f.data, f.len > 0 {
            ws.send(.data(Data(bytes: d, count: f.len))) { _ in }
        }
    }

    private static func evSnap(_ ev: hb_share_event) -> ShareEv {
        ShareEv(
            type: ev.type, id: ev.id, code: ev.code,
            name: ev.name.map { String(cString: $0) } ?? "",
            device: ev.device.map { String(cString: $0) } ?? "",
            text: ev.text.map { String(cString: $0) } ?? "")
    }

    private func postChanged(_ shareId: String) {
        NotificationCenter.default.post(
            name: Self.nChanged, object: shareId)
    }

    // ================= owner =================

    /// 异步：engine → POST /v1/shares → WS role=owner → attach session。
    /// cb(nil err) 时 share 已活跃；sess 由调用方拥有，停止前必须存活。
    func start(sess: Session, title: String, host: String, mode: Int,
               cb: ((String?, String?) -> Void)? = nil) {
        guard signedIn else {
            cb?(nil, L("未登录：请先在账号页登录")); return
        }
        // 一个 session 同时只允许一个 owner ctx（重复 start 复用既有
        // share；否则旧 ctx 失联，tab 关闭时按 sess 反查只剩最后一个）。
        if let existing = bySess[ObjectIdentifier(sess)] {
            cb?(existing, nil); return
        }
        var vk = [UInt8](repeating: 0, count: 32)
        guard sync.vkCopy(&vk) else {
            cb?(nil, L("账号密钥不可用")); return
        }
        let uuid = UUID().uuidString.lowercased()
        var engOpt: OpaquePointer?
        uuid.withCString { id in
            engOpt = vk.withUnsafeBufferPointer { p in
                hb_share_owner_new(id, p.baseAddress, Int32(mode))
            }
        }
        vk.withUnsafeMutableBufferPointer {
            $0.baseAddress?.update(repeating: 0, count: 32) }
        guard let eng = engOpt else {
            cb?(nil, L("引擎创建失败")); return
        }
        let c = OwnerCtx(id: uuid, title: title, host: host,
                         eng: eng, sess: sess, cb: cb)
        owners[uuid] = c
        bySess[ObjectIdentifier(sess)] = uuid

        let meta = hb_share_owner_meta(eng, title, host, sync.deviceName)
        let metaStr = meta.map { String(cString: $0) } ?? ""
        free(meta)
        let body = "{\"share_id\":\"\(uuid)\",\"meta\":\"\(metaStr)\"}"
        http("POST", "/v1/shares", body, auth: true) {
            [weak self, weak c] st, _ in
            guard let self, let c, !c.dead else { return }
            self.ownerPostDone(c, st)
        }
    }

    private func ownerPostDone(_ c: OwnerCtx, _ st: Int) {
        if st != 200 {
            c.cb?(nil, L("创建共享失败（服务器拒绝）")); c.cb = nil
            ownerDrop(c, delete: false)
            return
        }
        guard let server = sync.server else {
            c.cb?(nil, "no server"); c.cb = nil
            ownerDrop(c, delete: true); return
        }
        let url = Self.wsURL(server,
            "/v1/share/\(c.shareId)/ws?role=owner")
        guard let u = URL(string: url) else {
            c.cb?(nil, "bad url"); c.cb = nil
            ownerDrop(c, delete: true); return
        }
        var req = URLRequest(url: u)
        if let t = sync.tokenString() {
            req.setValue("Bearer \(t)", forHTTPHeaderField: "Authorization")
        }
        let ws = session.webSocketTask(with: req)
        c.ws = ws
        ws.maximumMessageSize = 8 * 1024 * 1024
        // URLSession 无握手回调：首个 receive 完成 = 握手结果
        // （对齐 soup ws_connect_async → owner_ws_ready）。
        ws.receive { [weak self, weak c] res in
            DispatchQueue.main.async {
                guard let self, let c, !c.dead else { return }
                switch res {
                case .success(let m):
                    c.wsOpen = true
                    c.pingTimer = Timer.scheduledTimer(
                        withTimeInterval: Self.wsPingSec,
                        repeats: true) { [weak c] _ in
                            c?.ws?.sendPing { _ in } }
                    self.ownerReady(c)
                    self.ownerHandleMsg(c, m)
                    self.ownerReceive(c)
                case .failure:
                    c.cb?(nil, L("共享连接失败")); c.cb = nil
                    self.ownerDrop(c, delete: true)
                }
            }
        }
        ws.resume()
    }

    /// 对齐 owner_finish_start：active → attach → fd+tick → drain → cb。
    private func ownerReady(_ c: OwnerCtx) {
        c.active = true
        if let s = c.sess { hb_share_owner_attach(c.eng, s.sess) }
        armOwnerFd(c)
        ownerArmTick(c)
        postChanged(c.shareId)
        let cb = c.cb; c.cb = nil
        cb?(c.shareId, nil)
    }

    private func ownerHandleMsg(_ c: OwnerCtx,
                                _ m: URLSessionWebSocketTask.Message) {
        var d = Data(); var txt = false
        if case .string(let s) = m { d = Data(s.utf8); txt = true }
        else if case .data(let b) = m { d = b }
        d.withUnsafeBytes { p in
            hb_share_owner_incoming(
                c.eng,
                p.baseAddress?.assumingMemoryBound(to: UInt8.self),
                p.count, txt)
        }
        ownerDrain(c); ownerEmit(c)
    }

    private func ownerReceive(_ c: OwnerCtx) {
        guard let ws = c.ws else { return }
        ws.receive { [weak self, weak c] res in
            DispatchQueue.main.async {
                guard let self, let c, !c.dead else { return }
                switch res {
                case .success(let m):
                    self.ownerHandleMsg(c, m)
                    self.ownerReceive(c)
                case .failure:
                    c.wsOpen = false; c.ws = nil
                    c.pingTimer?.invalidate(); c.pingTimer = nil
                    // 对齐 owner_ws_closed：relay 断开 = share 结束
                    if c.active && !c.stopping {
                        self.stop(c.shareId)
                    }
                }
            }
        }
    }

    private func ownerDrop(_ c: OwnerCtx, delete: Bool) {
        // POST 已建 share：DELETE 清理
        if delete {
            http("DELETE", "/v1/shares/\(c.shareId)", nil,
                 auth: true, cb: nil)
        }
        owners.removeValue(forKey: c.shareId)
        if let s = c.sess {
            bySess.removeValue(forKey: ObjectIdentifier(s))
        }
        ownerCtxFree(c)
    }

    private func ownerDrain(_ c: OwnerCtx) {
        guard let eng = c.eng else { return }
        var f = hb_share_frame()
        while hb_share_owner_next_frame(eng, &f) {
            wsSend(c.ws, open: c.wsOpen, &f)
            hb_share_frame_clear(&f)
        }
    }

    private func ownerEmit(_ c: OwnerCtx) {
        guard let eng = c.eng else { return }
        var ev = hb_share_event()
        while hb_share_owner_next_event(eng, &ev) {
            let e = Self.evSnap(ev)
            ownerTrack(c, e)
            NotificationCenter.default.post(
                name: Self.nOwnerEvent, object: c.shareId,
                userInfo: ["ev": e])
            hb_share_event_clear(&ev)
        }
        postChanged(c.shareId)
    }

    private func ownerTrack(_ c: OwnerCtx, _ ev: ShareEv) {
        switch ev.type {
        case HB_SH_EV_JOINED, HB_SH_EV_PAKE_OK:
            c.parts[ev.id] = Part(
                id: ev.id,
                name: ev.name.isEmpty ? "guest" : ev.name,
                device: ev.device,
                guest: ev.device.isEmpty)
        case HB_SH_EV_LEFT:
            c.parts.removeValue(forKey: ev.id)
        default: break
        }
    }

    private func armOwnerFd(_ c: OwnerCtx) {
        guard let eng = c.eng else { return }
        let fd = hb_share_owner_fd(eng)
        let src = DispatchSource.makeReadSource(
            fileDescriptor: fd, queue: .main)
        src.setEventHandler { [weak self, weak c] in
            guard let self, let c, !c.dead else { return }
            self.ownerDrain(c); self.ownerEmit(c)
        }
        src.resume()
        c.fdSrc = src
    }

    private func ownerArmTick(_ c: OwnerCtx) {
        c.tickWork?.cancel(); c.tickWork = nil
        guard let eng = c.eng else { return }
        let ms = hb_share_owner_next_ms(eng)
        guard ms >= 0 else { return }
        let w = DispatchWorkItem { [weak self, weak c] in
            guard let self, let c, !c.dead, c.eng != nil else { return }
            hb_share_owner_tick(c.eng)
            self.ownerDrain(c); self.ownerEmit(c)
            self.ownerArmTick(c)
        }
        c.tickWork = w
        DispatchQueue.main.asyncAfter(
            deadline: .now() + .milliseconds(max(Int(ms), 50)),
            execute: w)
    }

    private func ownerCtxFree(_ c: OwnerCtx) {
        c.dead = true
        c.fdSrc?.cancel(); c.fdSrc = nil
        c.tickWork?.cancel(); c.tickWork = nil
        c.pingTimer?.invalidate(); c.pingTimer = nil
        if let ws = c.ws {
            ws.cancel(with: .normalClosure, reason: nil)
            c.ws = nil
        }
        c.wsOpen = false
        if let eng = c.eng { hb_share_owner_free(eng); c.eng = nil }
    }

    /// DELETE + close WS + free engine。幂等。
    func stop(_ shareId: String?) {
        guard let shareId, let c = owners[shareId] else { return }
        c.stopping = true
        if c.active {
            http("DELETE", "/v1/shares/\(shareId)", nil,
                 auth: true, cb: nil)
        }
        if let s = c.sess {
            bySess.removeValue(forKey: ObjectIdentifier(s))
        }
        owners.removeValue(forKey: shareId)
        ownerCtxFree(c)
        postChanged(shareId)
    }

    /// session 退出/页关闭时按 sess 反查停止（对齐 stop_session）。
    func stopSession(_ s: Session) {
        if let id = bySess[ObjectIdentifier(s)] { stop(id) }
    }

    func shareForSession(_ s: Session) -> String? {
        bySess[ObjectIdentifier(s)]
    }

    func sessionShared(_ s: Session) -> Bool {
        bySess[ObjectIdentifier(s)] != nil
    }

    func starting(_ id: String) -> Bool {
        owners[id].map { !$0.active } ?? false
    }

    private func ownCtx(_ id: String?) -> OwnerCtx? {
        guard let id, let c = owners[id], c.active else { return nil }
        return c
    }

    func link(_ id: String) -> String? {
        guard let c = ownCtx(id), let server = sync.server,
              let eng = c.eng else { return nil }
        return hb_share_owner_link(eng, server).map {
            let s = String(cString: $0); free($0); return s }
    }

    func code(_ id: String) -> String? {
        ownCtx(id).flatMap { c in
            c.eng.flatMap { hb_share_owner_code($0).map {
                String(cString: $0) } } }
    }

    func mode(_ id: String) -> Int {
        ownCtx(id).flatMap { $0.eng }.map {
            Int(hb_share_owner_mode($0)) } ?? 0
    }

    func count(_ id: String) -> Int {
        ownCtx(id).flatMap { $0.eng }.map {
            Int(hb_share_owner_count($0)) } ?? 0
    }

    /// owner 当前参与者快照（对齐 hb_share_service_participants）。
    func participants(_ id: String) -> [Part] {
        ownCtx(id).map { $0.parts.values.sorted {
            $0.id < $1.id } } ?? []
    }

    func setMode(_ id: String, _ m: Int) {
        guard let c = ownCtx(id), let eng = c.eng else { return }
        hb_share_owner_set_mode(eng, Int32(m))
        putMeta(c)
        ownerDrain(c); ownerEmit(c); ownerArmTick(c)
    }

    private func putMeta(_ c: OwnerCtx) {
        guard let eng = c.eng else { return }
        let meta = hb_share_owner_meta(eng, c.title, c.host,
                                       sync.deviceName)
        let m = meta.map { String(cString: $0) } ?? ""
        free(meta)
        http("PUT", "/v1/shares/\(c.shareId)",
             "{\"meta\":\"\(m)\"}", auth: true, cb: nil)
    }

    /// 重新生成配对码/link secret → POST /shares/{id}/guest（解锁 PAKE）。
    func regen(_ id: String) {
        guard let c = ownCtx(id), let eng = c.eng else { return }
        hb_share_owner_regen_guest(eng)
        http("POST", "/v1/shares/\(id)/guest", nil, auth: true, cb: nil)
        ownerDrain(c); ownerEmit(c)
        postChanged(id)
    }

    func kick(_ id: String, _ pid: UInt32) {
        guard let c = ownCtx(id), let eng = c.eng else { return }
        hb_share_owner_kick(eng, pid)
        ownerDrain(c); ownerEmit(c)
    }

    /// 本机 share 绑定 tab（视图卡片点击切换用）。
    func bindTab(_ id: String, _ tab: TermTab) {
        owners[id]?.tab = tab
    }

    /// 本机活跃 shares（含 starting）。
    func ownShares() -> [OwnInfo] {
        owners.values.map {
            OwnInfo(shareId: $0.shareId, title: $0.title,
                    host: $0.host,
                    mode: $0.eng.map { Int(hb_share_owner_mode($0)) } ?? 0,
                    tab: $0.tab)
        }
    }

    // ================= peer list =================

    /// GET /v1/shares → 本账号进行中的共享。
    func list(_ cb: @escaping ([ShareItem]?, String?) -> Void) {
        guard signedIn else { cb(nil, L("未登录")); return }
        http("GET", "/v1/shares", nil, auth: true) { st, body in
            guard st == 200, let body,
                  let d = body.data(using: .utf8),
                  let o = try? JSONSerialization.jsonObject(
                      with: d) as? [String: Any],
                  let arr = o["shares"] as? [[String: Any]]
            else { cb(nil, L("获取共享列表失败")); return }
            cb(arr.map { ShareItem(
                shareId: $0["share_id"] as? String ?? "",
                metaB64: $0["meta"] as? String ?? "",
                ownerDevice: $0["owner_device"] as? String ?? "",
                created: ($0["created"] as? NSNumber)?.int64Value ?? 0)
            }, nil)
        }
    }

    // ================= peer join =================

    /// member：share_id + GET /shares 的 meta_b64（引擎解出 K_out/title）。
    func joinMember(_ shareId: String, _ metaB64: String,
                    _ cb: ((PeerCtx?, String?) -> Void)?) {
        guard signedIn, !shareId.isEmpty, !metaB64.isEmpty else {
            cb?(nil, L("未登录或参数缺失")); return
        }
        var vk = [UInt8](repeating: 0, count: 32)
        guard sync.vkCopy(&vk) else {
            cb?(nil, L("账号密钥不可用")); return
        }
        var err = HB_OK
        let devId = sync.deviceId ?? ""
        var engOpt: OpaquePointer?
        shareId.withCString { sid in
            metaB64.withCString { mb in
                devId.withCString { did in
                    sync.deviceName.withCString { dn in
                        engOpt = vk.withUnsafeBufferPointer { p in
                            hb_share_peer_new_member(
                                sid, p.baseAddress, mb, did, dn, &err)
                        }
                    }
                }
            }
        }
        vk.withUnsafeMutableBufferPointer {
            $0.baseAddress?.update(repeating: 0, count: 32) }
        guard let eng = engOpt else {
            cb?(nil, L("加入失败：无法解密共享信息")); return
        }
        let c = PeerCtx(id: shareId, role: "member", eng: eng, cb: cb)
        peers.append(c)
        peerConnect(c, sync.server)
    }

    /// guest：完整 link + 6 位配对码。
    func joinGuest(_ link: String, _ code: String,
                   _ cb: ((PeerCtx?, String?) -> Void)?) {
        guard !link.isEmpty, !code.isEmpty else {
            cb?(nil, L("需要链接和配对码")); return
        }
        var err = HB_OK
        var engOpt: OpaquePointer?
        link.withCString { l in
            code.withCString { cd in
                engOpt = hb_share_peer_new_guest(l, cd, &err)
            }
        }
        guard let eng = engOpt else {
            cb?(nil, L("链接或配对码无效")); return
        }
        let sid = hb_share_peer_share_id(eng).map {
            String(cString: $0) } ?? ""
        let c = PeerCtx(id: sid, role: "guest", eng: eng, cb: cb)
        peers.append(c)
        var server = hb_share_peer_server(eng).map {
            String(cString: $0) }
        if server == nil { server = sync.server }
        peerConnect(c, server)
    }

    private func peerConnect(_ c: PeerCtx, _ server: String?) {
        guard let server else {
            c.cb?(nil, "no server"); c.cb = nil
            releasePeer(c); return
        }
        let url = Self.wsURL(server,
            "/v1/share/\(c.shareId)/ws?role=\(c.role)")
        guard let u = URL(string: url) else {
            c.cb?(nil, "bad url"); c.cb = nil
            releasePeer(c); return
        }
        var req = URLRequest(url: u)
        if c.role == "member", let t = sync.tokenString() {
            req.setValue("Bearer \(t)",
                         forHTTPHeaderField: "Authorization")
        }
        let ws = session.webSocketTask(with: req)
        c.ws = ws
        ws.maximumMessageSize = 8 * 1024 * 1024
        // 首个 receive = 握手结果（对齐 peer_ws_ready）。
        ws.receive { [weak self, weak c] res in
            DispatchQueue.main.async {
                guard let self, let c, !c.dead else { return }
                switch res {
                case .success(let m):
                    c.wsOpen = true
                    c.pingTimer = Timer.scheduledTimer(
                        withTimeInterval: Self.wsPingSec,
                        repeats: true) { [weak c] _ in
                            c?.ws?.sendPing { _ in } }
                    self.peerReady(c)
                    self.peerHandleMsg(c, m)
                    self.peerReceive(c)
                case .failure:
                    c.cb?(nil, L("共享连接失败")); c.cb = nil
                    self.releasePeer(c)
                }
            }
        }
        ws.resume()
    }

    /// 对齐 peer_ws_ready：fd+tick+drain+emit → join cb。
    private func peerReady(_ c: PeerCtx) {
        wirePeerVtInput(c)
        armPeerFd(c)
        peerArmTick(c)
        peerDrain(c)      // JOIN / PAKE1 排队帧立即发出
        peerEmit(c)
        let cb = c.cb; c.cb = nil
        cb?(c, nil)
    }

    /// Linux 未接 peer vt 的 out_fn（UI 打键在 collab 也丢）；
    /// macOS 按 hb-terminal.h 注释接上：vt 编码输入 → INPUT 帧。
    private func wirePeerVtInput(_ c: PeerCtx) {
        guard let eng = c.eng, let v = hb_share_peer_vt(eng) else { return }
        hb_vt_set_output_fn(v, { bytes, n, ud in
            guard let ud, let bytes else { return }
            let c = Unmanaged<PeerCtx>.fromOpaque(ud)
                .takeUnretainedValue()
            guard !c.dead, let eng = c.eng else { return }
            _ = hb_share_peer_input(eng, bytes, n)
        }, Unmanaged.passUnretained(c).toOpaque())
    }

    private func peerHandleMsg(_ c: PeerCtx,
                               _ m: URLSessionWebSocketTask.Message) {
        var d = Data(); var txt = false
        if case .string(let s) = m { d = Data(s.utf8); txt = true }
        else if case .data(let b) = m { d = b }
        d.withUnsafeBytes { p in
            hb_share_peer_incoming(
                c.eng,
                p.baseAddress?.assumingMemoryBound(to: UInt8.self),
                p.count, txt)
        }
        peerDrain(c); peerEmit(c); peerArmTick(c)
    }

    private func peerReceive(_ c: PeerCtx) {
        guard let ws = c.ws else { return }
        ws.receive { [weak self, weak c] res in
            DispatchQueue.main.async {
                guard let self, let c, !c.dead else { return }
                switch res {
                case .success(let m):
                    self.peerHandleMsg(c, m)
                    self.peerReceive(c)
                case .failure:
                    c.wsOpen = false; c.ws = nil
                    c.pingTimer?.invalidate(); c.pingTimer = nil
                    // 对齐 peer_ws_closed：ws 断 → owner_gone
                    if !c.ended {
                        c.ended = true
                        NotificationCenter.default.post(
                            name: Self.nPeerEnded, object: c,
                            userInfo: ["reason": "owner_gone"])
                    }
                }
            }
        }
    }

    private func peerDrain(_ c: PeerCtx) {
        guard let eng = c.eng else { return }
        var f = hb_share_frame()
        while hb_share_peer_next_frame(eng, &f) {
            wsSend(c.ws, open: c.wsOpen, &f)
            hb_share_frame_clear(&f)
        }
    }

    private func peerEmit(_ c: PeerCtx) {
        guard let eng = c.eng else { return }
        var ev = hb_share_event()
        while hb_share_peer_next_event(eng, &ev) {
            let e = Self.evSnap(ev)
            NotificationCenter.default.post(
                name: Self.nPeerEvent, object: c,
                userInfo: ["ev": e])
            let ended = ev.type == HB_SH_EV_ENDED
            hb_share_event_clear(&ev)
            // 引擎结束（pake 失败/被踢/owner 停止）：立即收 ws，
            // 否则 guest 占 relay pending-PAKE 槽位，重试被 4009 拒。
            if ended { closePeer(c) }
        }
    }

    private func armPeerFd(_ c: PeerCtx) {
        guard let eng = c.eng else { return }
        let fd = hb_share_peer_fd(eng)
        let src = DispatchSource.makeReadSource(
            fileDescriptor: fd, queue: .main)
        src.setEventHandler { [weak self, weak c] in
            guard let self, let c, !c.dead else { return }
            self.peerDrain(c); self.peerEmit(c)
        }
        src.resume()
        c.fdSrc = src
    }

    private func peerArmTick(_ c: PeerCtx) {
        c.tickWork?.cancel(); c.tickWork = nil
        guard let eng = c.eng else { return }
        let ms = hb_share_peer_next_ms(eng)
        guard ms >= 0 else { return }
        let w = DispatchWorkItem { [weak self, weak c] in
            guard let self, let c, !c.dead, c.eng != nil else { return }
            hb_share_peer_tick(c.eng)
            self.peerDrain(c); self.peerEmit(c)
            self.peerArmTick(c)
        }
        c.tickWork = w
        DispatchQueue.main.asyncAfter(
            deadline: .now() + .milliseconds(max(Int(ms), 50)),
            execute: w)
    }

    /// WS 关闭，引擎保留（对齐 hb_share_peer_ctx_close）。
    func closePeer(_ c: PeerCtx) {
        guard !c.ended else { return }
        c.ended = true
        if let ws = c.ws {
            ws.cancel(with: .normalClosure, reason: nil)
            c.ws = nil
        }
        c.wsOpen = false
        c.pingTimer?.invalidate(); c.pingTimer = nil
    }

    /// viewer/tab 关闭时归还：close + free。
    func releasePeer(_ c: PeerCtx) {
        closePeer(c)
        peers.removeAll { $0 === c }
        peerCtxFree(c)
    }

    private func peerCtxFree(_ c: PeerCtx) {
        c.dead = true
        c.fdSrc?.cancel(); c.fdSrc = nil
        c.tickWork?.cancel(); c.tickWork = nil
        c.pingTimer?.invalidate(); c.pingTimer = nil
        if let ws = c.ws {
            ws.cancel(with: .normalClosure, reason: nil)
            c.ws = nil
        }
        c.wsOpen = false
        // 摘掉 vt 输入线（vt 随 eng 释放）
        if let eng = c.eng, let v = hb_share_peer_vt(eng) {
            hb_vt_set_output_fn(v, nil, nil)
        }
        if let eng = c.eng { hb_share_peer_free(eng); c.eng = nil }
    }
}
