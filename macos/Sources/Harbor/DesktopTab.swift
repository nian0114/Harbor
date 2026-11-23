import AppKit
import CHarbor
import HarborKit

/// 帧缓冲画布（linux RdCanvas）：BGRX 位图上下文 + 损坏区增量拷贝，
/// fit=缩放适应 / orig=1:1（超出可滚）。呈现由 AppKit 按显示率合并。
final class RdCanvasView: NSView {
    /// 借用（page 拥有；detach 后置 nil）
    weak var sess: RdSession?
    private(set) var fbW = 0, fbH = 0
    private var ctx: CGContext?
    private var buf: UnsafeMutableRawPointer?
    private var bufSize = 0
    var orig = false { didSet { needsDisplay = true } }
    /// 远端光标（CURSOR 事件生成）
    var remoteCursor: NSCursor? {
        didSet { window?.invalidateCursorRects(for: self) }
    }
    /// 断线时保留最后一帧（dim 层由 page 盖在上面）
    var onInput: ((RdInput) -> Void)?
    /// 光标进入/位置更新（剪贴板补偿 + scroll 用）
    var onMove: ((NSPoint) -> Void)?
    private var lastPos = NSPoint.zero
    private var havePos = false

    enum RdInput {
        case pointer(x: Int, y: Int, mask: UInt32)
        case wheel(x: Int, y: Int, mask: UInt32, count: Int)
        case key(keysym: UInt32, kc: UInt32, down: Bool)
        case releaseAll
    }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for e: NSEvent?) -> Bool { true }

    /// 当前绘制矩形（几何同源 RdKit）。
    func drawRect() -> (x: Double, y: Double, w: Double, h: Double) {
        RdKit.drawRect(availW: Double(bounds.width),
                       availH: Double(bounds.height),
                       fbW: fbW, fbH: fbH, orig: orig)
    }
    /// 控件点 → fb 点（letterbox 外 nil）
    func fbPoint(_ p: NSPoint) -> (Int, Int)? {
        guard let m = RdKit.mapPoint(x: Double(p.x), y: Double(p.y),
                                     rect: drawRect(),
                                     fbW: fbW, fbH: fbH)
        else { return nil }
        return (Int(m.fx), Int(m.fy))
    }

    /// FRAME 事件：只把损坏区从 core fb 拷进位图上下文。
    private var needFull = true
    func frame(damaged r: NSRect?) {
        guard let f = sess?.lockFb() else { sess = nil; return }
        if f.w != fbW || f.h != fbH { allocBuf(w: f.w, h: f.h) }
        if let buf {
            let dmg = needFull ? nil : r
            needFull = false
            let bpp = 4
            let myStride = fbW * bpp
            if let r = dmg, !r.isNull && !r.isInfinite {
                /* 损坏区按行拷贝（r 为 fb 坐标） */
                let x0 = max(0, Int(r.origin.x))
                let y0 = max(0, Int(r.origin.y))
                let x1 = min(fbW, Int(r.origin.x + r.size.width + 0.5))
                let y1 = min(fbH, Int(r.origin.y + r.size.height + 0.5))
                if x1 > x0 && y1 > y0 {
                    for y in y0..<y1 {
                        memcpy(buf + y * myStride + x0 * bpp,
                               f.px + y * f.stride + x0 * bpp,
                               (x1 - x0) * bpp)
                    }
                }
            } else {
                for y in 0..<fbH {
                    memcpy(buf + y * myStride, f.px + y * f.stride,
                           fbW * bpp)
                }
            }
        }
        sess?.unlockFb()
        needsDisplay = true
    }
    /// 远端尺寸变化。
    func setSize(_ w: Int, _ h: Int) {
        if w != fbW || h != fbH { allocBuf(w: w, h: h) }
        needsDisplay = true
    }
    func detach() {
        sess = nil
    }
    private func allocBuf(w: Int, h: Int) {
        fbW = w; fbH = h
        if let b = buf { free(b) ; buf = nil }
        ctx = nil
        guard w > 0, h > 0 else { return }
        bufSize = w * h * 4
        buf = malloc(bufSize)
        guard let buf else { return }
        memset(buf, 0, bufSize)
        needFull = true
        ctx = CGContext(data: buf, width: w, height: h, bitsPerComponent: 8,
                        bytesPerRow: w * 4, space: CGColorSpace(name:
                            CGColorSpace.sRGB)!, bitmapInfo: RdKit.fbBitmapInfo)
    }

    override func draw(_ dirty: NSRect) {
        guard let c = NSGraphicsContext.current?.cgContext else { return }
        c.setFillColor(NSColor.black.cgColor)
        c.fill(bounds)
        let r = drawRect()
        guard r.w > 0, r.h > 0, let img = ctx?.makeImage() else { return }
        c.interpolationQuality = .medium
        /* isFlipped 上下文里 CGImage 会再翻一次——手动翻回（core fb
         * 是顶行在前的 BGRX）。 */
        c.saveGState()
        c.translateBy(x: CGFloat(r.x), y: CGFloat(r.y + r.h))
        c.scaleBy(x: 1, y: -1)
        c.draw(img, in: NSRect(x: 0, y: 0, width: r.w, height: r.h))
        c.restoreGState()
    }

    // ---------- 输入 ----------
    private func emit(_ i: RdInput) { onInput?(i) }
    private func pt(_ e: NSEvent) -> (Int, Int)? {
        fbPoint(convert(e.locationInWindow, from: nil))
    }
    override func mouseMoved(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        lastPos = p; havePos = true
        if let (x, y) = pt(e) { emit(.pointer(x: x, y: y, mask: btnMask)) }
    }
    override func mouseEntered(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        lastPos = p; havePos = true
        onMove?(p)
    }
    override func mouseExited(with e: NSEvent) { havePos = false }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach { removeTrackingArea($0) }
        addTrackingArea(NSTrackingArea(rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited,
                      .activeInKeyWindow, .inVisibleRect],
            owner: self, userInfo: nil))
    }
    private var btnMask: UInt32 = 0
    private func press(_ e: NSEvent, bit: UInt32) {
        window?.makeFirstResponder(self)
        btnMask |= bit
        if let (x, y) = pt(e) { emit(.pointer(x: x, y: y, mask: btnMask)) }
    }
    private func release(_ e: NSEvent, bit: UInt32) {
        btnMask &= ~bit
        if let (x, y) = pt(e) ?? (fbPoint(lastPos)) {
            emit(.pointer(x: x, y: y, mask: btnMask))
        }
    }
    override func mouseDown(with e: NSEvent) { press(e, bit: 1) }
    override func mouseUp(with e: NSEvent) { release(e, bit: 1) }
    override func rightMouseDown(with e: NSEvent) { press(e, bit: 4) }
    override func rightMouseUp(with e: NSEvent) { release(e, bit: 4) }
    override func otherMouseDown(with e: NSEvent) {
        press(e, bit: RdKit.buttonBit(e.buttonNumber))
    }
    override func otherMouseUp(with e: NSEvent) {
        release(e, bit: RdKit.buttonBit(e.buttonNumber))
    }
    private func drag(_ e: NSEvent) {
        if let (x, y) = pt(e) { emit(.pointer(x: x, y: y, mask: btnMask)) }
    }
    override func mouseDragged(with e: NSEvent) { drag(e) }
    override func rightMouseDragged(with e: NSEvent) { drag(e) }
    override func otherMouseDragged(with e: NSEvent) { drag(e) }

    private var accY = 0.0, accX = 0.0
    override func scrollWheel(with e: NSEvent) {
        guard havePos, let (x, y) = fbPoint(lastPos) else { return }
        /* 精确滚动（触控板）：像素增量聚合成行；滚轮鼠标：delta 即行数 */
        let u: Double = e.hasPreciseScrollingDeltas ? 16 : 1
        let sy = RdKit.scrollSteps(acc: accY, delta: Double(e.deltaY),
                                   unit: u)
        accY = sy.rest
        if sy.steps != 0 {
            /* AppKit deltaY>0 = 向上 → bit3（linux 同语义） */
            emit(.wheel(x: x, y: y,
                        mask: btnMask | RdKit.wheelMask(dy: sy.steps),
                        count: abs(sy.steps)))
        }
        let sx = RdKit.scrollSteps(acc: accX, delta: Double(e.deltaX),
                                   unit: u)
        accX = sx.rest
        if sx.steps != 0 {
            emit(.wheel(x: x, y: y,
                        mask: btnMask | RdKit.hwheelMask(dx: -sx.steps),
                        count: abs(sx.steps)))
        }
    }

    /// 透传开关（页面设置）：⌘ 组合键发远端而非被本地菜单吃掉的部分；
    /// ⌘ 本身总是映射为 Super（linux passthrough）。
    var passthrough = false
    private var modsDown = NSEvent.ModifierFlags()
    /// 按下中的 keyCode → keysym（linux keys 表：up 必须复用
    /// down 时的 keyval——Shift 中途松开不改 keysym，防卡键）。
    private var downSyms: [UInt16: UInt32] = [:]

    override func keyDown(with e: NSEvent) {
        /* passthrough 关：⌘ 组合留给 macOS 菜单（不吃掉） */
        if !passthrough && e.modifierFlags.contains(.command) {
            super.keyDown(with: e); return
        }
        let sym = keysym(e)
        if !e.isARepeat { downSyms[e.keyCode] = sym }
        emit(.key(keysym: sym, kc:
                 RdKit.keyInfo(e.keyCode)?.x11kc ?? 0, down: true))
    }
    override func keyUp(with e: NSEvent) {
        if !passthrough && e.modifierFlags.contains(.command) {
            super.keyUp(with: e); return
        }
        let sym = downSyms.removeValue(forKey: e.keyCode)
            ?? keysym(e)
        emit(.key(keysym: sym, kc:
                 RdKit.keyInfo(e.keyCode)?.x11kc ?? 0, down: false))
    }
    /// 事件 → keysym：取产生字符（含 Shift），控制码退化为未修饰
    /// 小写字符（linux：GDK keyval 即此值）。
    private func keysym(_ e: NSEvent) -> UInt32 {
        func ks(_ s: String?) -> UInt32 {
            guard let u = s?.unicodeScalars.first else { return 0 }
            return RdKit.charKeysym(u.value)
        }
        if let k = RdKit.keyInfo(e.keyCode), k.keysym != 0 {
            return k.keysym
        }
        var sym = ks(e.characters)
        if sym == 0 {
            sym = ks(e.charactersIgnoringModifiers?.lowercased())
        }
        return sym
    }
    override func flagsChanged(with e: NSEvent) {
        /* 修饰键按下/抬起：发对应 keysym + x11kc；
         * ⌘ 仅在透传时发（否则 macOS 本地使用）。 */
        let f = e.modifierFlags
        let isCmd = e.keyCode == 0x37 || e.keyCode == 0x36
        if isCmd && !passthrough {
            modsDown = f.intersection([.shift, .control, .option,
                                       .command, .capsLock])
            return          // ⌘ 留给 macOS（快捷键透传关闭时）
        }
        func mod(_ flag: NSEvent.ModifierFlags, _ k: RdKit.KeyInfo) {
            let was = modsDown.contains(flag), now = f.contains(flag)
            if was != now {
                emit(.key(keysym: k.keysym, kc: k.x11kc, down: now))
            }
        }
        if let k = RdKit.keyInfo(e.keyCode) {
            // 左/右侧按 keyCode 各自定位，修饰位共享同 flag
            switch e.keyCode {
            case 0x38, 0x3C: mod(.shift, k)
            case 0x3B, 0x3E: mod(.control, k)
            case 0x3A, 0x3D: mod(.option, k)
            case 0x37, 0x36: mod(.command, k)
            case 0x39:       mod(.capsLock, k)
            default: break
            }
        }
        modsDown = f.intersection([.shift, .control, .option, .command,
                                   .capsLock])
    }
    override func resignFirstResponder() -> Bool {
        btnMask = 0
        emit(.releaseAll)
        return super.resignFirstResponder()
    }
    override func resetCursorRects() {
        if let c = remoteCursor {
            addCursorRect(bounds, cursor: c)
        } else {
            addCursorRect(bounds, cursor: .arrow)
        }
    }
    deinit { if let b = buf { free(b) } }
}

/// 远程桌面标签页（linux hb-desktop-page.c 移植）：
/// 连接进度 → 桌面；断线 → 旧帧 + 倒计时自动重连；
/// 隧道 = tab 私有 ConnCtl(.forward) 临时 local 转发 127.0.0.1:0。
final class DesktopTab: TermTab, ConnDelegate {
    private let hostId: String
    private var proto = "vnc"
    /// 主机快照（linux load_host → p->h*）
    private var hname = "", haddr = "", huser = "", hdomain = ""
    private var hport = 0
    private var useTunnel = false, viaId: String?, viaName = ""
    private var viaUser = "", viaSame = false
    private var boundPort = 0
    private var quality: Int = 1       // HB_RD_Q_BALANCED
    private var viewOnly = false, passthrough = false
    private var origSize = false

    private var rd: RdSession?
    private var ctl: ConnCtl?
    private var tunSess: Session?
    private var connected = false, everConnected = false
    private var gen = 1
    private var boK = 0, countdown = 0
    private var boTimer: Timer?
    private static let boTab = [1, 2, 5, 10, 30]
    private var logId: Int64 = 0, logT0 = Date()
    private var lastRemoteClip = ""
    private var dead = false

    // 视图
    private var stageView: NSView?
    private var desktopView: NSView!
    private var canvas: RdCanvasView!
    private var scroll: NSScrollView!
    private var dim: NSView!, discOv: NSView!, discLbl: NSTextField!
    private var bannerLbl: NSTextField!, secLbl: NSTextField!
    private var passHint: NSTextField!, roPill: NSTextField!
    private var qPop: NSPopUpButton!
    private var openAlert: NSAlert?           // 自管的 rd prompt/cert
    private var stageLines = ""
    private var logLabel: NSTextField?
    private var logAttr = NSMutableAttributedString()
    private var dots: [NSTextField] = []
    private var stageErrCard: NSView?
    private var errTitleL: NSTextField?
    private var errDetailL: NSTextField?
    private var subLbl: NSTextField?
    private static let stages = [L("解析"), "TCP", L("密钥交换"),
                                 L("认证"), L("隧道"), L("桌面")]

    init(hostId: String, title: String) {
        self.hostId = hostId
        if let o = Vault.shared.get(hostId) {
            proto = hb_obj_get_str(o, "proto").str
            hb_obj_free(o)
        }
        if proto != "rdp" { proto = "vnc" }
        super.init(title: title, view: NSView())
        iconName = "monitor"
        buildDesktop()
        loadHost()
        bannerRefresh()
    }
    required init?(coder: NSCoder) { fatalError() }

    // ---------- 主机快照（linux load_host） ----------
    @discardableResult
    private func loadHost() -> Bool {
        let v = Vault.shared
        guard let h = v.get(hostId) else {
            showError(L("主机对象不存在或已删除"), nil)
            return false
        }
        hname = hb_obj_get_str(h, "name").str
        if hname.isEmpty { hname = hb_obj_get_str(h, "address").str }
        haddr = hb_obj_get_str(h, "address").str
        huser = hb_obj_get_str(h, "user").str
        hdomain = hb_obj_get_str(h, "domain").str
        hport = Int(hb_obj_get_int(h, "port",
                                   Int64(proto == "rdp" ? 3389 : 5900)))
        quality = RdKit.qualityOf(hb_obj_get_str(h, "rd_quality").str)
        let via = hb_obj_get_str(h, "via").str
        useTunnel = !via.isEmpty
            && hb_obj_get_str(h, "tunnel").str == "ssh"
        viaId = useTunnel ? via : nil
        if useTunnel, let vid = viaId {
            guard let vo = v.get(vid) else {
                hb_obj_free(h)
                showError(L("经由 SSH 主机不存在或已删除"), nil)
                return false
            }
            let vn = hb_obj_get_str(vo, "name").str
            let va = hb_obj_get_str(vo, "address").str
            viaName = vn.isEmpty ? va : vn
            viaUser = hb_obj_get_str(vo, "user").str
            viaSame = RdKit.isLoopback(haddr)
                || (!va.isEmpty
                    && haddr.caseInsensitiveCompare(va) == .orderedSame)
            hb_obj_free(vo)
        }
        hb_obj_free(h)
        qPop?.selectItem(at: quality)
        return true
    }

    // ---------- 日志（linux dlog_*） ----------
    private func dlogStart() {
        guard logId <= 0, let vv = Vault.shared.vault else { return }
        var l = hb_log()
        var id: Int64 = 0
        let pr = proto == "rdp" ? "RDP" : "VNC"
        let protoS = useTunnel ? pr + "/Tunnel" : pr
        let c = [huser, hname, hostId, protoS, "connecting"]
            .map { $0.withCString { strdup($0)! } }
        l.user = UnsafePointer(c[0]); l.host = UnsafePointer(c[1])
        l.host_id = UnsafePointer(c[2]); l.proto = UnsafePointer(c[3])
        l.result = UnsafePointer(c[4]); l.duration_s = -1
        _ = hb_vault_log_add(vv, &l, &id)
        c.forEach { free($0) }
        logId = id; logT0 = Date()
    }
    private func dlogFinish(_ result: String, _ reason: String?) {
        guard logId > 0, let vv = Vault.shared.vault else {
            logId = 0; return
        }
        _ = hb_vault_log_finish(vv, logId, result, reason,
                                Int64(Date().timeIntervalSince(logT0)))
        logId = 0
    }

    // ---------- rd 启动（linux start_rd） ----------
    private func startRd(_ host: String, _ port: Int) {
        var o = hb_rd_opts()
        var allocs: [UnsafeMutablePointer<CChar>] = []
        func cs(_ s: String?) -> UnsafePointer<CChar>? {
            guard let s, !s.isEmpty else { return nil }
            let p = strdup(s)!; allocs.append(p)
            return UnsafePointer(p)
        }
        let sec = Vault.shared.secretC(hostId, "password")
        o.proto = proto == "rdp" ? HB_RD_RDP : HB_RD_VNC
        o.host = cs(host)
        o.port = Int32(port)
        o.user = cs(huser)
        o.domain = cs(hdomain)
        if let sec, strlen(sec) > 0 { o.password = UnsafePointer(sec) }
        o.quality = hb_rd_quality(rawValue: UInt32(quality))
        o.view_only = viewOnly
        o.connect_timeout_s = 15
        if o.proto == HB_RD_RDP {
            o.tls_hostname = cs(haddr)
            let dir = Vault.shared.dataDir
                .appendingPathComponent("freerdp").path
            try? FileManager.default.createDirectory(
                atPath: dir, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
            chmod(dir, 0o700)
            o.freerdp_config_dir = cs(dir)
            let cw = Int(desktopView?.bounds.width ?? 0)
            let ch = Int(desktopView?.bounds.height ?? 0) - 30
            if cw > 0 && ch > 0 {
                o.width = Int32(cw & ~1); o.height = Int32(ch & ~1)
            }
            if let h = Vault.shared.get(hostId) {
                o.cert_fp = cs(hb_obj_get_str(h, "rdp_cert_fp").str)
                hb_obj_free(h)
            }
        }
        let r = RdSession(opts: &o)
        if let sec { memset(sec, 0, strlen(sec)); hb_secret_free(sec) }
        allocs.forEach { free($0) }
        guard r.rd != nil else {
            showError(L("无法创建远程桌面会话"), nil)
            return
        }
        rd = r
        r.onEvent = { [weak self] ev in self?.rdEv(ev) }
        canvas?.sess = r
        canvas?.frame(damaged: nil)
    }

    // ---------- 隧道（linux ConnCtl FORWARD 段） ----------
    private func startTunnel() {
        if ctl == nil {
            let c = ConnCtl(mode: .forward, hostId: viaId,
                            window: view.window)
            c.delegate = self
            ctl = c
        } else {
            ctl?.hostId = viaId
            ctl?.window = view.window
        }
        ctl?.setForward(HB_PF_LOCAL,
            bindAddr: "127.0.0.1", bindPort: 0,
            destAddr: viaSame ? "127.0.0.1" : haddr,
            destPort: Int32(hport))
        ctl?.start(rows: 24, cols: 80)
    }

    // ---------- ConnDelegate（隧道） ----------
    func conn(_ c: ConnCtl, stage idx: Int, line: String,
              mark: Character) {
        stage(idx, line, mark)
    }
    func connOSInfo(_ c: ConnCtl, id: String, pretty: String) {}
    func connSession(_ c: ConnCtl, _ s: Session) {
        tunSess = s
        s.onEvent = { [weak self, weak ctl = self.ctl] ev in
            guard let self else { return }
            var e = ev
            if ctl?.handleEvent(&e) == true { return }
            if ev.type == HB_SESS_EV_FORWARD, let pf = ev.pf {
                self.onPf(pf.pointee)
            }
        }
        s.drain()
    }
    private func onPf(_ st: hb_pf_status) {
        switch st.kind {
        case HB_PF_EV_LISTENING:
            boundPort = Int(st.bound_port)
            let dest = viaSame ? "127.0.0.1" : haddr
            stage(4, String(format:
                L("开通 Local 转发 127.0.0.1:%d → %@:%d"),
                boundPort, dest, hport), " ")
            bannerRefresh()
            stage(5, String(format: L("%@ 握手中…"),
                            proto == "rdp" ? "RDP" : "VNC"), " ")
            startRd("127.0.0.1", boundPort)
        case HB_PF_EV_WARN:
            stage(-1, st.message.map { String(cString: $0) } ?? "", "e")
        default: break
        }
    }
    func connConnected(_ c: ConnCtl) {}
    func connFailed(_ c: ConnCtl, code: Int, title: String,
                    details: String?) {
        showError(title, details?.isEmpty == false ? details
                                                 : L("连接失败"))
    }
    func connDisconnected(_ c: ConnCtl) {
        /* ctl 自带重连会让两条退避线互踩：关掉，由页面 backoff 统一
         * 驱动（linux 同：HB_CONN_FORWARD 的自动重连归页面）。 */
        c.cancel()
        if everConnected { startBackoff() }
        else { showError(L("SSH 隧道断开"), nil) }
    }
    func connExited(_ c: ConnCtl, code: Int, reason: String) {}

    // ---------- 一轮连接（linux start_attempt） ----------
    private func startAttempt(_ isReconn: Bool) {
        gen += 1
        forceCloseOwnAlert()
        if !isReconn { stageReset() }
        clearErr()
        let desk = everConnected && isReconn
        stageView?.isHidden = desk
        desktopView?.isHidden = !desk
        statusDot = Theme.dotPend
        guard loadHost() else { return }
        subLbl?.stringValue = useTunnel
            ? "\(proto.uppercased()) · \(L("经由 SSH 隧道"))"
            : proto.uppercased()
        if !useTunnel {
            stage(0, String(format: L("正在连接 %@:%d…"),
                            haddr, hport), " ")
            startRd(haddr, hport)
            return
        }
        startTunnel()
    }

    // ---------- rd 事件（linux rd_ev_one） ----------
    private func rdEv(_ ev: hb_rd_event) {
        switch ev.type {
        case HB_RD_EV_STAGE:
            stage(5, ev.text.str, " ")
        case HB_RD_EV_CONNECTED:
            connected = true; everConnected = true
            boK = 0; stopBackoff(); hideDisc()
            stageView?.isHidden = true; desktopView?.isHidden = false
            secLbl?.stringValue = ev.text2.str
            canvas?.sess = rd
            canvas?.setSize(Int(ev.w), Int(ev.h))
            layoutCanvas()
            dlogStart()
            statusDot = Theme.dotOk
            view.window?.makeFirstResponder(canvas)
        case HB_RD_EV_RESIZE:
            canvas?.setSize(Int(ev.w), Int(ev.h))
            layoutCanvas()
        case HB_RD_EV_FRAME:
            canvas?.frame(damaged: ev.w > 0 || ev.h > 0
                ? NSRect(x: Double(ev.x), y: Double(ev.y),
                         width: Double(ev.w), height: Double(ev.h))
                : nil)
            #if DEBUG
            devFpsT.append(Date().timeIntervalSince1970)
            if devFpsT.count > 600 { devFpsT.removeFirst(devFpsT.count - 600) }
            #endif
        case HB_RD_EV_CURSOR:
            if let px = ev.px, ev.w > 0, ev.h > 0 {
                if let rep = NSBitmapImageRep(
                    bitmapDataPlanes: nil, pixelsWide: Int(ev.w),
                    pixelsHigh: Int(ev.h), bitsPerSample: 8,
                    samplesPerPixel: 4, hasAlpha: true,
                    isPlanar: false,
                    colorSpaceName: .deviceRGB,
                    bytesPerRow: Int(ev.w) * 4, bitsPerPixel: 32) {
                    if let dst = rep.bitmapData {
                        // BGRA → RGBA 交换 R/B
                        for i in 0..<Int(ev.w) * Int(ev.h) {
                            let o = i * 4
                            dst[o] = px[o + 2]
                            dst[o + 1] = px[o + 1]
                            dst[o + 2] = px[o]
                            dst[o + 3] = px[o + 3]
                        }
                    }
                    let img = NSImage(size: rep.size)
                    img.addRepresentation(rep)
                    canvas?.remoteCursor = NSCursor(image: img,
                        hotSpot: NSPoint(x: CGFloat(ev.hx),
                                         y: CGFloat(ev.hy)))
                }
            }
        case HB_RD_EV_CLIPBOARD:
            if let t = ev.text {
                lastRemoteClip = t.str
                let pb = NSPasteboard.general
                pb.clearContents()
                pb.setString(t.str, forType: .string)
            }
        case HB_RD_EV_CERT:
            showCert(Int(ev.code), ev.text.str, ev.text2.str)
        case HB_RD_EV_PROMPT:
            showPrompt(Int(ev.code), ev.text.str)
        case HB_RD_EV_ERROR:
            rdError(Int(ev.code), ev.text.str, ev.text2.str)
        case HB_RD_EV_EXIT:
            if connected {
                connected = false
                if everConnected { startBackoff() }
            }
        default: break
        }
    }
    private func rdError(_ code: Int, _ text: String, _ detail: String) {
        connected = false
        statusDot = Theme.dotErr
        switch code {
        case Int(HB_RD_ERR_DISCONNECTED.rawValue):
            dlogFinish("error", "disconnected"); logId = 0
            if everConnected { startBackoff() }
            else { showError(L("连接中断"), nil) }
        case Int(HB_RD_ERR_AUTH.rawValue):
            showError(L("认证失败：用户名或密码错误"), detail)
        case Int(HB_RD_ERR_CERT_REJECTED.rawValue):
            showError(L("证书被拒绝"), detail)
        case Int(HB_RD_ERR_CANCELLED.rawValue):
            showError(L("已取消"), detail)
        default:
            showError(text.isEmpty ? L("连接失败") : text, detail)
        }
    }

    // ---------- 对话框（linux show_prompt / show_cert） ----------
    private func forceCloseOwnAlert() {
        if let a = openAlert {
            view.window?.endSheet(a.window)
            openAlert = nil
        }
    }

    private func showPrompt(_ code: Int, _ text: String) {
        forceCloseOwnAlert()
        let rdp = proto == "rdp"
        let a = NSAlert()
        a.messageText = rdp ? L("RDP 凭据") : L("VNC 密码")
        a.informativeText = text.isEmpty
            ? (rdp ? L("远程桌面需要登录凭据")
                   : L("服务器要求 VNC 密码")) : text
        a.addButton(withTitle: L("连接"))
        a.addButton(withTitle: L("取消"))
        let userF = NSTextField(frame: .zero)
        let pwF = NSSecureTextField(frame: .zero)
        let domF = NSTextField(frame: .zero)
        let saveCk = NSButton(checkboxWithTitle: L("保存到 Vault"),
                              target: nil, action: nil)
        let stack = NSStackView()
        stack.orientation = .vertical; stack.spacing = 6
        stack.alignment = .left
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.widthAnchor.constraint(equalToConstant: 260).isActive = true
        if code == 1 {
            stack.addArrangedSubview(NSTextField(
                labelWithString: L("用户名")))
            userF.stringValue = huser
            userF.setAccessibilityIdentifier("rd-prompt-user")
            stack.addArrangedSubview(userF)
        }
        stack.addArrangedSubview(NSTextField(labelWithString: L("密码")))
        pwF.setAccessibilityIdentifier("rd-prompt-pass")
        stack.addArrangedSubview(pwF)
        if code == 1 {
            stack.addArrangedSubview(NSTextField(
                labelWithString: L("域（可空）")))
            domF.stringValue = hdomain
            domF.setAccessibilityIdentifier("rd-prompt-dom")
            stack.addArrangedSubview(domF)
        }
        saveCk.setAccessibilityIdentifier("rd-prompt-save")
        stack.addArrangedSubview(saveCk)
        a.accessoryView = stack
        openAlert = a
        let g = gen
        DialogRegistry.shared.present(alert: a, kind: "rd-prompt",
                                      in: view.window) { [weak self] ok in
            guard let self else { return }
            let u = code == 1 ? userF.stringValue : nil
            let p = pwF.stringValue
            let d = code == 1 ? domF.stringValue : nil
            pwF.stringValue = ""          // 抹掉对话框明文
            self.openAlert = nil
            guard self.gen == g, let rd = self.rd, !self.dead
            else { return }
            if ok {
                rd.promptReply(user: u, password: p, domain: d)
                if saveCk.state == .on, !p.isEmpty {
                    let v = Vault.shared
                    v.setSecret(self.hostId, "password", p)
                    if let h = v.get(self.hostId) {
                        if let u, !u.isEmpty {
                            hb_obj_set_str(h, "user", u)
                        }
                        if let d {
                            hb_obj_set_str(h, "domain",
                                d.isEmpty ? nil : d)
                        }
                        v.put(h); hb_obj_free(h)
                    }
                }
            } else {
                rd.promptReply(user: nil, password: nil, domain: nil)
            }
        }
    }

    private func showCert(_ code: Int, _ fp: String, _ summary: String) {
        forceCloseOwnAlert()
        let changed = code == 1
        let a = NSAlert()
        if changed { a.alertStyle = .critical }
        a.messageText = changed ? L("警告：远程桌面证书已变更")
                                : L("验证远程桌面证书")
        var info = changed
            ? L("远程主机的证书与本地记录不一致。这可能是重装或配置变更，"
              + "也可能是中间人攻击——除非已带外核实，否则应断开。")
            : L("该远程桌面证书未受信任。请通过带外渠道核对指纹后再信任。")
        if changed, let h = Vault.shared.get(hostId) {
            let old = hb_obj_get_str(h, "rdp_cert_fp").str
            hb_obj_free(h)
            info += "\n\n" + L("旧指纹") + ": " + old
                + "\n" + L("新指纹") + ": " + fp
        } else {
            info += "\n\n" + fp
        }
        if !summary.isEmpty { info += "\n" + summary }
        a.informativeText = info
        if changed {
            a.addButton(withTitle: L("断开"))            // 默认 Return
            a.addButton(withTitle: L("我已核实，更新指纹并继续"))
        } else {
            a.addButton(withTitle: L("信任此证书"))
            a.addButton(withTitle: L("取消"))
        }
        openAlert = a
        let g = gen
        DialogRegistry.shared.present(alert: a,
            kind: changed ? "rd-cert-changed" : "rd-cert",
            in: view.window) { [weak self] ok in
            guard let self else { return }
            self.openAlert = nil
            guard self.gen == g, let rd = self.rd, !self.dead
            else { return }
            let accept = changed ? !ok : ok
            rd.certReply(accept)
            if accept, !fp.isEmpty, let h = Vault.shared.get(self.hostId) {
                hb_obj_set_str(h, "rdp_cert_fp", fp)
                Vault.shared.put(h); hb_obj_free(h)
            }
        }
        // 变更警告：默认焦点在「断开」（firstButton 即默认钮）
    }

    // ---------- 断线/重连（linux backoff 段） ----------
    private func stopBackoff() { boTimer?.invalidate(); boTimer = nil }
    private func showDisc(_ text: String) {
        discLbl.stringValue = text
        discOv.isHidden = false; dim.isHidden = false
    }
    private func hideDisc() {
        discOv.isHidden = true; dim.isHidden = true
    }
    private func startBackoff() {
        let idx = boK
        let d = idx < DesktopTab.boTab.count ? DesktopTab.boTab[idx] : 30
        boK += 1
        countdown = d
        showDisc(String(format: L("连接中断 · %ds 后重连"), d))
        stopBackoff()
        let g = gen
        boTimer = Timer.scheduledTimer(withTimeInterval: 1,
                                       repeats: true) { [weak self] _ in
            guard let self, self.gen == g else { return }
            self.countdown -= 1
            if self.countdown <= 0 {
                self.boTimer?.invalidate(); self.boTimer = nil
                self.reconnectNow()
            } else {
                self.showDisc(String(format:
                    L("连接中断 · %ds 后重连"), self.countdown))
            }
        }
    }
    private func reconnectNow() {
        stopBackoff()
        gen += 1
        forceCloseOwnAlert()
        showDisc(L("正在重连…"))
        detachSession()
        startAttempt(true)
    }

    // ---------- 会话摘除（linux kill_session_async） ----------
    /// 摘走 rd + tunnel session（含对话框应答），后台线程收尾。
    private func detachSession() {
        let r = rd
        rd = nil
        canvas?.detach()
        if let r { r.onEvent = nil; r.stopSource() }
        tunSess = nil
        let s = ctl?.stealSession()
        ctl?.cancel()
        s?.onEvent = nil; s?.stopSource()
        if r != nil || s != nil {
            DispatchQueue.global(qos: .userInitiated).async {
                if let r { r.close(); r.free_() }
                if let s { s.close(); s.free_() }
            }
        }
    }

    // ---------- 错误卡（linux show_error → connect view error） ----------
    private func showError(_ t: String, _ detail: String?) {
        stopBackoff()
        dlogFinish("error", detail)
        hideDisc()
        stageView?.isHidden = false
        desktopView?.isHidden = true
        stageErrCard?.isHidden = false
        errTitleL?.stringValue = t
        errDetailL?.stringValue = detail ?? stageLines
        statusDot = Theme.dotErr
    }
    private func clearErr() { stageErrCard?.isHidden = true }

    // ---------- 输入分发（linux in_*） ----------
    private func onCanvasInput(_ i: RdCanvasView.RdInput) {
        guard let rd else { return }
        switch i {
        case .pointer(let x, let y, let m):
            if !viewOnly { rd.pointer(x, y, m) }
        case .wheel(let x, let y, let m, let n):
            guard !viewOnly else { return }
            for _ in 0..<n {
                rd.pointer(x, y, m)
                rd.pointer(x, y, m & ~0x78)
            }
        case .key(let sym, let kc, let down):
            if !viewOnly { rd.key(sym, kc, down) }
        case .releaseAll:
            if !viewOnly { rd.pointer(0, 0, 0) }
        }
    }

    // ---------- 剪贴板（linux maybe_send_clipboard） ----------
    private func maybeSendClipboard() {
        guard let rd, connected, !viewOnly else { return }
        /* linux is_active：仅活动 tab 上传本地剪贴板 */
        guard let w = view.window as? MainWindow,
              w.selTab >= 0, w.selTab < w.tabs.count,
              w.tabs[w.selTab] === self
        else { return }
        let pb = NSPasteboard.general
        guard let txt = pb.string(forType: .string),
              txt != lastRemoteClip else { return }
        lastRemoteClip = txt
        rd.clipboard(txt)
    }
    private func pollClipboard() {
        // AppKit 无剪贴板变化信号：changeCount 轮询（1s）
        let pb = NSPasteboard.general
        var last = pb.changeCount
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true)
        { [weak self] t in
            guard let self, !self.dead else { t.invalidate(); return }
            let c = pb.changeCount
            if c != last { last = c; self.maybeSendClipboard() }
        }
    }
    private func cad() {
        guard let rd else { return }
        for k in RdKit.cad { rd.key(k.keysym, k.kc, true) }
        for k in RdKit.cad.reversed() { rd.key(k.keysym, k.kc, false) }
    }
    private func sendWin() {
        guard let rd else { return }
        rd.key(RdKit.winKey.keysym, RdKit.winKey.kc, true)
        rd.key(RdKit.winKey.keysym, RdKit.winKey.kc, false)
    }

    // ---------- 横幅 ---------- 
    private func bannerRefresh() {
        if useTunnel {
            let dest = viaSame ? "127.0.0.1" : haddr
            bannerLbl.stringValue = String(format:
                L("经由 %@@%@ SSH 隧道 · 127.0.0.1:%d → 目标 %@:%d · "
                + "目标服务未暴露公网"),
                viaUser.isEmpty ? "?" : viaUser,
                viaName.isEmpty ? "?" : viaName,
                boundPort, dest, hport)
        } else {
            bannerLbl.stringValue = String(format: L("直连 %@:%d"),
                                           haddr, hport)
        }
    }
    private func saveQuality() {
        guard let h = Vault.shared.get(hostId) else { return }
        hb_obj_set_str(h, "rd_quality", RdKit.qualityStr(quality))
        Vault.shared.put(h); hb_obj_free(h)
    }
    @objc private func qualityPick(_ p: NSPopUpButton) {
        let i = p.indexOfSelectedItem
        guard (0..<3).contains(i) else { return }
        quality = i
        rd?.setQuality(quality)
        saveQuality()
    }
    @objc private func roToggle(_ s: NSMenuItem) {
        viewOnly = s.state != .on
        s.state = viewOnly ? .on : .off
        roPill.isHidden = !viewOnly
    }
    @objc private func fitToggle(_ s: NSMenuItem) {
        origSize = s.state != .on    // 勾选=适应窗口
        s.state = origSize ? .off : .on
        canvas.orig = origSize
        layoutCanvas()
    }
    @objc private func passToggle(_ s: NSMenuItem) {
        passthrough = s.state != .on
        s.state = passthrough ? .on : .off
        canvas.passthrough = passthrough
        passHint.isHidden = !passthrough
    }
    @objc private func menuCad() { cad() }
    @objc private func menuWin() { sendWin() }
    @objc private func discReconnect() { reconnectNow() }
    @objc private func discClose() { closeSelf() }
    @objc private func closeSelf() {
        guard let w = view.window as? MainWindow,
              let i = w.tabs.firstIndex(where: { $0 === self })
        else { return }
        w.closeTab(i)
    }
    @objc private func retry() {
        stageErrCard?.isHidden = true
        startAttempt(false)
    }
    @objc private func editHost() {
        (view.window as? MainWindow)?.editHostById(hostId)
    }

    // ---------- 视图构建 ----------
    private func stage(_ idx: Int, _ line: String, _ mark: Character) {
        if !line.isEmpty {
            let attr = NSAttributedString(string: (mark == "e" ? "✗ " : "")
                + line + "\n",
                attributes: [.foregroundColor:
                    mark == "e" ? NSColor.systemRed
                                : NSColor.secondaryLabelColor])
            logAttr.append(attr)
            stageLines += line + "\n"
        }
        logLabel?.attributedStringValue = logAttr
        if idx >= 0 && idx < dots.count {
            for i in 0...idx {
                dots[i].stringValue = "●\n" + DesktopTab.stages[i]
            }
        }
    }
    private func stageReset() {
        stageLines = ""; logAttr = NSMutableAttributedString()
        logLabel?.attributedStringValue = logAttr
        for (i, s) in DesktopTab.stages.enumerated() {
            dots[i].stringValue = "○\n" + s
        }
    }

    private func buildStageView() {
        let v = NSView()
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.page.color.cgColor
        v.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(v)
        NSLayoutConstraint.activate([
            v.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            v.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            v.topAnchor.constraint(equalTo: view.topAnchor),
            v.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        let tile = NSView()
        tile.wantsLayer = true
        tile.layer?.cornerRadius = 10
        tile.layer?.backgroundColor = osTile(proto).bg.color.cgColor
        tile.translatesAutoresizingMaskIntoConstraints = false
        let img = NSImageView(image: hbIcon(osTile(proto).icon,
                                            size: 22, tint: .white))
        img.translatesAutoresizingMaskIntoConstraints = false
        tile.addSubview(img)
        NSLayoutConstraint.activate([
            img.centerXAnchor.constraint(equalTo: tile.centerXAnchor),
            img.centerYAnchor.constraint(equalTo: tile.centerYAnchor),
            tile.widthAnchor.constraint(equalToConstant: 44),
            tile.heightAnchor.constraint(equalToConstant: 44),
        ])
        let nameL = NSTextField(labelWithString: title)
        nameL.font = .boldSystemFont(ofSize: 15)
        nameL.translatesAutoresizingMaskIntoConstraints = false
        let sub = NSTextField(labelWithString: "")
        sub.font = .systemFont(ofSize: 11)
        sub.textColor = .secondaryLabelColor
        sub.translatesAutoresizingMaskIntoConstraints = false
        subLbl = sub
        let steps = NSView()
        steps.translatesAutoresizingMaskIntoConstraints = false
        dots = []
        for (i, s) in DesktopTab.stages.enumerated() {
            let d = NSTextField(labelWithString: "○\n" + s)
            d.alignment = .center
            d.font = .systemFont(ofSize: 10)
            d.setAccessibilityIdentifier("rd-stage-\(i)")
            steps.addSubview(d); dots.append(d)
        }
        let card = NSView()
        card.wantsLayer = true
        card.layer?.cornerRadius = 8
        card.layer?.backgroundColor = Theme.card.color.cgColor
        card.translatesAutoresizingMaskIntoConstraints = false
        let l = NSTextField(wrappingLabelWithString: "")
        l.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        l.translatesAutoresizingMaskIntoConstraints = false
        l.setAccessibilityIdentifier("rd-stage-log")
        card.addSubview(l); logLabel = l
        // 错误卡
        let err = NSView()
        err.wantsLayer = true
        err.layer?.cornerRadius = 8
        err.layer?.backgroundColor = Theme.card.color.cgColor
        err.translatesAutoresizingMaskIntoConstraints = false
        err.isHidden = true
        let et = NSTextField(wrappingLabelWithString: "")
        et.font = .boldSystemFont(ofSize: 13)
        et.textColor = .systemRed
        et.translatesAutoresizingMaskIntoConstraints = false
        et.setAccessibilityIdentifier("rd-err-title")
        let ed = NSTextField(wrappingLabelWithString: "")
        ed.font = .systemFont(ofSize: 11)
        ed.textColor = .secondaryLabelColor
        ed.translatesAutoresizingMaskIntoConstraints = false
        ed.setAccessibilityIdentifier("rd-err-details")
        let retryB = NSButton(title: L("重试"), target: self,
                              action: #selector(retry))
        retryB.bezelStyle = .rounded
        retryB.translatesAutoresizingMaskIntoConstraints = false
        retryB.setAccessibilityIdentifier("rd-retry")
        let editB = NSButton(title: L("编辑主机"), target: self,
                             action: #selector(editHost))
        editB.bezelStyle = .rounded
        editB.translatesAutoresizingMaskIntoConstraints = false
        editB.setAccessibilityIdentifier("rd-edit-host")
        let closeB = NSButton(title: L("关闭"), target: self,
                              action: #selector(closeSelf))
        closeB.bezelStyle = .rounded
        closeB.translatesAutoresizingMaskIntoConstraints = false
        closeB.setAccessibilityIdentifier("rd-close")
        [et, ed, retryB, editB, closeB].forEach { err.addSubview($0) }
        errTitleL = et; errDetailL = ed; stageErrCard = err
        [tile, nameL, sub, steps, card, err].forEach { v.addSubview($0) }
        NSLayoutConstraint.activate([
            tile.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            tile.topAnchor.constraint(equalTo: v.topAnchor, constant: 40),
            nameL.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            nameL.topAnchor.constraint(equalTo: tile.bottomAnchor,
                                       constant: 10),
            sub.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            sub.topAnchor.constraint(equalTo: nameL.bottomAnchor,
                                     constant: 2),
            steps.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            steps.topAnchor.constraint(equalTo: sub.bottomAnchor,
                                       constant: 24),
            steps.widthAnchor.constraint(equalToConstant: 480),
            steps.heightAnchor.constraint(equalToConstant: 40),
            card.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            card.topAnchor.constraint(equalTo: steps.bottomAnchor,
                                      constant: 16),
            card.widthAnchor.constraint(equalToConstant: 560),
            card.heightAnchor.constraint(greaterThanOrEqualToConstant: 80),
            card.bottomAnchor.constraint(lessThanOrEqualTo:
                v.bottomAnchor, constant: -20),
            l.topAnchor.constraint(equalTo: card.topAnchor, constant: 10),
            l.leadingAnchor.constraint(equalTo: card.leadingAnchor,
                                       constant: 12),
            l.trailingAnchor.constraint(equalTo: card.trailingAnchor,
                                        constant: -12),
            l.bottomAnchor.constraint(lessThanOrEqualTo:
                card.bottomAnchor, constant: -10),
            err.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            err.topAnchor.constraint(equalTo: card.bottomAnchor,
                                     constant: 10),
            err.widthAnchor.constraint(equalToConstant: 560),
            err.heightAnchor.constraint(equalToConstant: 120),
            err.bottomAnchor.constraint(lessThanOrEqualTo:
                v.bottomAnchor, constant: -10),
            et.topAnchor.constraint(equalTo: err.topAnchor, constant: 10),
            et.leadingAnchor.constraint(equalTo: err.leadingAnchor,
                                        constant: 12),
            et.trailingAnchor.constraint(equalTo: err.trailingAnchor,
                                         constant: -12),
            ed.topAnchor.constraint(equalTo: et.bottomAnchor, constant: 4),
            ed.leadingAnchor.constraint(equalTo: err.leadingAnchor,
                                        constant: 12),
            ed.trailingAnchor.constraint(equalTo: err.trailingAnchor,
                                         constant: -12),
            ed.heightAnchor.constraint(lessThanOrEqualToConstant: 40),
            retryB.leadingAnchor.constraint(equalTo: ed.leadingAnchor),
            retryB.bottomAnchor.constraint(equalTo: err.bottomAnchor,
                                           constant: -8),
            editB.leadingAnchor.constraint(equalTo:
                retryB.trailingAnchor, constant: 8),
            editB.bottomAnchor.constraint(equalTo: err.bottomAnchor,
                                          constant: -8),
            closeB.trailingAnchor.constraint(equalTo: err.trailingAnchor,
                                             constant: -12),
            closeB.bottomAnchor.constraint(equalTo: err.bottomAnchor,
                                           constant: -8),
        ])
        for (i, d) in dots.enumerated() {
            d.translatesAutoresizingMaskIntoConstraints = false
            d.centerXAnchor.constraint(equalTo: steps.leadingAnchor,
                constant: CGFloat(480) / CGFloat(dots.count)
                    * (CGFloat(i) + 0.5)).isActive = true
            d.centerYAnchor.constraint(equalTo: steps.centerYAnchor)
                .isActive = true
        }
        stageView = v
    }

    private func buildDesktop() {
        let dv = NSView()
        dv.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(dv)
        NSLayoutConstraint.activate([
            dv.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            dv.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            dv.topAnchor.constraint(equalTo: view.topAnchor),
            dv.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        desktopView = dv
        dv.isHidden = true
        buildStageView()

        // 横幅
        let bar = NSView()
        bar.wantsLayer = true
        bar.layer?.backgroundColor = Theme.card.color.cgColor
        bar.translatesAutoresizingMaskIntoConstraints = false
        dv.addSubview(bar)
        let lockI = NSImageView(image: hbIcon("key", size: 12,
                                              tint: .secondaryLabelColor))
        lockI.translatesAutoresizingMaskIntoConstraints = false
        bannerLbl = NSTextField(labelWithString: "")
        bannerLbl.lineBreakMode = .byTruncatingMiddle
        bannerLbl.translatesAutoresizingMaskIntoConstraints = false
        bannerLbl.setContentCompressionResistancePriority(.defaultLow,
                                                        for: .horizontal)
        passHint = NSTextField(labelWithString: L("快捷键透传中"))
        passHint.textColor = .systemOrange
        passHint.font = .systemFont(ofSize: 10)
        passHint.isHidden = true
        passHint.translatesAutoresizingMaskIntoConstraints = false
        passHint.setAccessibilityIdentifier("rd-pass-hint")
        roPill = NSTextField(labelWithString: L("只读"))
        roPill.font = .systemFont(ofSize: 10)
        roPill.textColor = .white
        roPill.wantsLayer = true
        roPill.layer?.cornerRadius = 7
        roPill.layer?.backgroundColor = NSColor.systemGray.cgColor
        roPill.isHidden = true
        roPill.translatesAutoresizingMaskIntoConstraints = false
        roPill.setAccessibilityIdentifier("rd-ro-pill")
        secLbl = NSTextField(labelWithString: "")
        secLbl.textColor = .secondaryLabelColor
        secLbl.font = .systemFont(ofSize: 10)
        secLbl.translatesAutoresizingMaskIntoConstraints = false
        secLbl.setAccessibilityIdentifier("rd-sec")
        qPop = NSPopUpButton()
        qPop.addItems(withTitles: [RdKit.qualityLabel(0),
                                 RdKit.qualityLabel(1),
                                 RdKit.qualityLabel(2)])
        qPop.selectItem(at: quality)
        qPop.bezelStyle = .inline
        qPop.target = self; qPop.action = #selector(qualityPick(_:))
        qPop.translatesAutoresizingMaskIntoConstraints = false
        qPop.setAccessibilityIdentifier("rd-quality")
        let moreB = NSPopUpButton()
        moreB.pullsDown = true
        moreB.addItem(withTitle: "")
        moreB.item(at: 0)?.image = hbIcon("dots", size: 13,
                                          tint: .labelColor)
        moreB.item(at: 0)?.isHidden = true
        moreB.bezelStyle = .inline
        moreB.translatesAutoresizingMaskIntoConstraints = false
        moreB.setAccessibilityIdentifier("rd-more")
        let menu = NSMenu()
        let cadI = menu.addItem(withTitle: L("发送 Ctrl+Alt+Del"),
                action: #selector(menuCad), keyEquivalent: "")
        cadI.target = self
        let winI = menu.addItem(withTitle: L("发送 Win"),
                action: #selector(menuWin), keyEquivalent: "")
        winI.target = self
        let roI = menu.addItem(withTitle: L("只读观看"),
                action: #selector(roToggle(_:)), keyEquivalent: "")
        roI.target = self
        let fitI = menu.addItem(withTitle: L("适应窗口（取消=原始大小）"),
                action: #selector(fitToggle(_:)), keyEquivalent: "")
        fitI.target = self
        fitI.state = .on
        let passI = menu.addItem(withTitle: L("快捷键透传"),
                action: #selector(passToggle(_:)), keyEquivalent: "")
        passI.target = self
        moreB.menu = menu
        let discB = NSButton(title: L("断开"), target: self,
                             action: #selector(discClose))
        discB.bezelStyle = .rounded
        discB.translatesAutoresizingMaskIntoConstraints = false
        discB.setAccessibilityIdentifier("rd-disconnect")
        [lockI, bannerLbl, passHint, roPill, secLbl, qPop, moreB, discB]
            .forEach { bar.addSubview($0) }
        // 画布区
        let wrap = NSView()
        wrap.wantsLayer = true
        wrap.layer?.backgroundColor = NSColor.black.cgColor
        wrap.translatesAutoresizingMaskIntoConstraints = false
        dv.addSubview(wrap)
        scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        wrap.addSubview(scroll)
        canvas = RdCanvasView()
        canvas.onInput = { [weak self] i in self?.onCanvasInput(i) }
        canvas.onMove = { [weak self] _ in self?.maybeSendClipboard() }
        scroll.documentView = canvas
        dim = NSView()
        dim.wantsLayer = true
        dim.layer?.backgroundColor = NSColor.black
            .withAlphaComponent(0.45).cgColor
        dim.translatesAutoresizingMaskIntoConstraints = false
        dim.isHidden = true
        wrap.addSubview(dim)
        discOv = NSView()
        discOv.wantsLayer = true
        discOv.layer?.backgroundColor = NSColor.black
            .withAlphaComponent(0.6).cgColor
        discOv.layer?.cornerRadius = 8
        discOv.translatesAutoresizingMaskIntoConstraints = false
        discOv.isHidden = true
        discLbl = NSTextField(labelWithString: L("连接中断"))
        discLbl.textColor = .white
        discLbl.font = .systemFont(ofSize: 13)
        discLbl.translatesAutoresizingMaskIntoConstraints = false
        discLbl.setAccessibilityIdentifier("rd-disc")
        let reB = NSButton(title: L("立即重连"), target: self,
                           action: #selector(discReconnect))
        reB.bezelStyle = .rounded
        reB.keyEquivalent = "\r"
        reB.translatesAutoresizingMaskIntoConstraints = false
        reB.setAccessibilityIdentifier("rd-reconnect")
        let clB = NSButton(title: L("关闭"), target: self,
                           action: #selector(discClose))
        clB.bezelStyle = .rounded
        clB.translatesAutoresizingMaskIntoConstraints = false
        clB.setAccessibilityIdentifier("rd-disc-close")
        [discLbl, reB, clB].forEach { discOv.addSubview($0) }
        wrap.addSubview(discOv)
        NSLayoutConstraint.activate([
            bar.leadingAnchor.constraint(equalTo: dv.leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: dv.trailingAnchor),
            bar.topAnchor.constraint(equalTo: dv.topAnchor),
            bar.heightAnchor.constraint(equalToConstant: 30),
            lockI.leadingAnchor.constraint(equalTo: bar.leadingAnchor,
                                           constant: 8),
            lockI.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            bannerLbl.leadingAnchor.constraint(equalTo:
                lockI.trailingAnchor, constant: 6),
            bannerLbl.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            passHint.leadingAnchor.constraint(equalTo:
                bannerLbl.trailingAnchor, constant: 8),
            passHint.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            roPill.leadingAnchor.constraint(equalTo:
                passHint.trailingAnchor, constant: 8),
            roPill.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            secLbl.leadingAnchor.constraint(equalTo:
                roPill.trailingAnchor, constant: 8),
            secLbl.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            discB.trailingAnchor.constraint(equalTo: bar.trailingAnchor,
                                            constant: -8),
            discB.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            discB.widthAnchor.constraint(equalToConstant: 52),
            moreB.trailingAnchor.constraint(equalTo:
                discB.leadingAnchor, constant: -4),
            moreB.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            moreB.widthAnchor.constraint(equalToConstant: 30),
            qPop.trailingAnchor.constraint(equalTo:
                moreB.leadingAnchor, constant: -4),
            qPop.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            qPop.widthAnchor.constraint(equalToConstant: 80),
            wrap.leadingAnchor.constraint(equalTo: dv.leadingAnchor),
            wrap.trailingAnchor.constraint(equalTo: dv.trailingAnchor),
            wrap.topAnchor.constraint(equalTo: bar.bottomAnchor),
            wrap.bottomAnchor.constraint(equalTo: dv.bottomAnchor),
            scroll.leadingAnchor.constraint(equalTo: wrap.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: wrap.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: wrap.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: wrap.bottomAnchor),
            dim.leadingAnchor.constraint(equalTo: wrap.leadingAnchor),
            dim.trailingAnchor.constraint(equalTo: wrap.trailingAnchor),
            dim.topAnchor.constraint(equalTo: wrap.topAnchor),
            dim.bottomAnchor.constraint(equalTo: wrap.bottomAnchor),
            discOv.centerXAnchor.constraint(equalTo: wrap.centerXAnchor),
            discOv.centerYAnchor.constraint(equalTo: wrap.centerYAnchor),
            discOv.widthAnchor.constraint(equalToConstant: 260),
            discOv.heightAnchor.constraint(equalToConstant: 100),
            discLbl.centerXAnchor.constraint(equalTo: discOv.centerXAnchor),
            discLbl.topAnchor.constraint(equalTo: discOv.topAnchor,
                                         constant: 14),
            reB.leadingAnchor.constraint(equalTo: discOv.leadingAnchor,
                                         constant: 40),
            reB.bottomAnchor.constraint(equalTo: discOv.bottomAnchor,
                                        constant: -14),
            clB.trailingAnchor.constraint(equalTo: discOv.trailingAnchor,
                                          constant: -40),
            clB.bottomAnchor.constraint(equalTo: discOv.bottomAnchor,
                                        constant: -14),
        ])
        scroll.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            forName: NSView.boundsDidChangeNotification,
            object: scroll.contentView, queue: .main)
        { [weak self] _ in self?.layoutCanvas() }
        canvas.frame = NSRect(x: 0, y: 0, width: 640, height: 480)
        layoutCanvas()
        pollClipboard()
    }
    /// 画布尺寸跟随可视区（fit）或远端尺寸（orig）。
    private func layoutCanvas() {
        var clip = scroll.contentView.bounds.size
        if clip.width < 2 || clip.height < 2 {
            clip = scroll.frame.size
        }
        if origSize {
            canvas.frame = NSRect(x: 0, y: 0,
                width: max(clip.width, CGFloat(canvas?.fbW ?? 0)),
                height: max(clip.height, CGFloat(canvas?.fbH ?? 0)))
        } else {
            canvas.frame = NSRect(origin: .zero, size: clip)
        }
        canvas.needsDisplay = true
    }
    /// 布局定稿后开始一轮连接（对齐 SSHTab.startSession 时机）。
    override func startSession() { startAttempt(false) }

    // ---------- dev 钩子（DEBUG only；dev.sock `rd*` 命令） ----------
    #if DEBUG
    private var devFpsT: [Double] = []
    /// `rd`：页面状态 JSON。
    var devState: String {
        let o: [String: Any] = [
            "proto": proto, "connected": connected,
            "boundPort": boundPort, "useTunnel": useTunnel,
            "via": viaName, "quality": quality,
            "viewOnly": viewOnly, "passthrough": passthrough,
            "fbW": canvas?.fbW ?? 0, "fbH": canvas?.fbH ?? 0,
            "countdown": countdown,
            "desktop": !(desktopView?.isHidden ?? true),
            "errorShown": !(stageErrCard?.isHidden ?? true),
            "banner": bannerLbl?.stringValue ?? "",
            "disc": !(discOv?.isHidden ?? true),
        ]
        let d = (try? JSONSerialization.data(withJSONObject: o)) ?? Data()
        return String(data: d, encoding: .utf8) ?? "{}"
    }
    /// `rd-type <text>`：逐字符 keysym down/up。
    func devType(_ s: String) {
        for ch in s.unicodeScalars {
            let sym = RdKit.charKeysym(ch.value)
            guard sym != 0 else { continue }
            rd?.key(sym, 0, true); rd?.key(sym, 0, false)
        }
    }
    /// `rd-click <x> <y> [btn]`：fb 坐标点击（1/2/3）。
    func devClick(_ x: Int, _ y: Int, _ btn: UInt32 = 1) {
        rd?.pointer(x, y, btn); rd?.pointer(x, y, 0)
    }
    /// `rd-move <x> <y>`。
    func devMove(_ x: Int, _ y: Int) { rd?.pointer(x, y, 0) }
    /// `rd-key <keysym-hex>` down/up。
    func devKey(_ sym: UInt32) {
        rd?.key(sym, 0, true); rd?.key(sym, 0, false)
    }
    /// `rd-clip <text>`：写本地剪贴板并立即上传远端。
    func devClip(_ s: String) {
        let pb = NSPasteboard.general
        pb.clearContents(); pb.setString(s, forType: .string)
        lastRemoteClip = ""; maybeSendClipboard()
    }
    /// `rd-fps [sec]`：近 sec 秒内 FRAME 事件数 → fps。
    func devFps(_ sec: Double = 0) -> String {
        let now = Date().timeIntervalSince1970
        let win = sec > 0 ? sec : 10
        let n = devFpsT.filter { now - $0 <= win }.count
        return String(format: "%.1f fps (%d frames / %.0fs)",
                      Double(n) / win, n, win)
    }
    #endif

    // ---------- Tab 生命周期 ----------
    override var running: Bool { connected }
    /// linux hb_desktop_page_connected 分支：已连桌面 +1。
    override var onlineCount: Int { connected ? 1 : 0 }
    /// linux hb_desktop_page_disconnect：hb_rd_close（断开后走
    /// 已有 backoff 重连流程）。
    override func disconnect() { rd?.close() }
    /// 唤醒重连走页内 backoff（linux 同路径），不经 restart()。
    override var wakeReconnectable: Bool { false }
    override func kill() {
        dead = true
        gen += 1
        stopBackoff()
        forceCloseOwnAlert()
        connected = false
        let r = rd; rd = nil
        canvas?.detach()
        r?.onEvent = nil; r?.stopSource()
        tunSess = nil
        ctl?.delegate = nil
        let s = ctl?.stealSession()
        ctl?.cancel()
        ctl = nil
        s?.onEvent = nil; s?.stopSource()
        /* 异步收尾：hb_rd_free/hb_session_free 在工作线程汇合，
         * 不阻塞 UI（linux kill_thread）。 */
        if r != nil || s != nil {
            DispatchQueue.global(qos: .userInitiated).async {
                if let r { r.close(); r.free_() }
                if let s { s.close(); s.free_() }
            }
        }
        dlogFinish("closed", nil)
        super.kill()
    }
}
