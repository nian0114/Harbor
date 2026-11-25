import AppKit
import CHarbor
import HarborKit

/// SFTP 双栏浏览器（FR-05/FR-19，对齐 linux hb-sftp-view.c）。
/// SftpTab.view == SftpView；每栏 select → conn → browse 三态。

enum XKind { case up, down, relay, local }
enum XState { case run, done, fail, cancel }

/// 传输任务（linux Xfer）。
final class Xfer {
    var id = 0
    var kind: XKind = .up
    var state: XState = .run
    var skipped = false
    weak var sp: SftpPane?
    weak var dp: SftpPane?
    var name = "", srcPath = "", dstDir = ""
    var srcLabel = "", dstLabel = ""
    var policy: Int32 = 0             // hb_sftp_conflict
    var req: UInt32 = 0
    weak var sess: Session?
    // relay
    var tmpDir: String?, tmpPath: String?
    var phase = 0                     // 0=download 1=upload
    // local copy
    var lcop: LocalCopy?
    // progress
    var done: UInt64 = 0, total: UInt64 = 0
    var acc: UInt64 = 0
    var t0 = Date()
    var ema = 0.0
    var emaT: Date?
    var emaDone: UInt64 = 0
    var err: String?
    var tick: Timer?
    // row UI
    var row: NSView?
    var fill: NSView?
    var pct: NSTextField?
    var xbtn: NSButton?
    var fillW: NSLayoutConstraint?
}

/// 本地拷贝（lc_walk 等价）：256K 块、递归、不跟随链接、冲突策略、
/// 取消时清理自建目录/半成品文件。
final class LocalCopy {
    let src, dst: String
    let policy: Int32
    var cancelled = false
    var done: UInt64 = 0, total: UInt64 = 0
    var err: String?
    var skipped = false
    private var nskip = 0
    private var created: String?     // 本次新建的最外层目录
    private var partial: String?     // 写入中的新建文件

    init(src: String, dst: String, policy: Int32) {
        self.src = src; self.dst = dst; self.policy = policy
    }
    func cancel() { cancelled = true }

    /// 后台线程执行；完成后主线程回调 ok/skip/err/cancelled。
    func run(_ finish: @escaping () -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            guard let st = SftpKit.statPath(self.src) else {
                self.err = String(cString: strerror(errno))
                DispatchQueue.main.async(execute: finish); return
            }
            do {
                var topSkip = false
                guard let rd = self.resolveDst(self.dst,
                        isDir: (st.st_mode & S_IFMT) == S_IFDIR,
                        skipped: &topSkip) else {
                    if topSkip { self.skipped = true }
                    else if self.err == nil {
                        self.err = L("无法解析目标路径")
                    }
                    DispatchQueue.main.async(execute: finish); return
                }
                self.addTotal(self.src, top: true)
                if self.walk(self.src, rd, top: true), !self.cancelled,
                   self.err == nil {
                    self.done = self.total
                }
            }
            DispatchQueue.main.async(execute: finish)
        }
    }

    private func addTotal(_ path: String, top: Bool) {
        guard let st = SftpKit.statPath(path, follow: top) else {
            return
        }
        if (st.st_mode & S_IFMT) == S_IFDIR {
            if let ns = try? FileManager.default
                .contentsOfDirectory(atPath: path) {
                for n in ns { addTotal(SftpKit.pathJoin(path, n), top: false) }
            }
        } else if (st.st_mode & S_IFMT) == S_IFREG {
            total += UInt64(st.st_size)
        }
    }

    /// lc_dst：NULL+skipped=跳过，NULL+err=失败，非 NULL=目标路径。
    private func resolveDst(_ dst: String, isDir: Bool,
                            skipped: inout Bool) -> String? {
        var st = stat()
        skipped = false
        if lstat(dst, &st) != 0 { return dst }
        if isDir && (st.st_mode & S_IFMT) == S_IFDIR { return dst }
        if policy == Int32(HB_SFTP_SKIP.rawValue) {
            skipped = true; return nil
        }
        if policy == Int32(HB_SFTP_RENAME.rawValue) {
            let name = URL(fileURLWithPath: dst).lastPathComponent
            let dir = SftpKit.parentDir(dst)
            for n in 1..<1000 {
                let c = SftpKit.pathJoin(dir,
                    SftpKit.renVariant(name, isDir: isDir, n))
                var st2 = stat()
                if lstat(c, &st2) != 0 { return c }
            }
            err = L("无法生成重命名目标"); return nil
        }
        if (st.st_mode & S_IFMT) == S_IFDIR && !isDir {
            err = L("目标是目录"); return nil
        }
        return dst
    }

    /// lc_copy_file：256K 块读写，fsync 收尾，取消留痕清理。
    private func copyFile(_ s: String, _ d: String,
                          mode: mode_t) -> Bool {
        var ex = stat()
        if lstat(d, &ex) != 0 { partial = d }
        let inp = Darwin.open(s, O_RDONLY)
        if inp < 0 { err = String(cString: strerror(errno)); return false }
        let out = Darwin.open(d, O_WRONLY | O_CREAT | O_TRUNC,
                              mode & 0o777)
        if out < 0 {
            err = String(cString: strerror(errno))
            close(inp); return false
        }
        defer { close(inp); close(out) }
        let bsz = 256 * 1024
        let buf = UnsafeMutableRawPointer.allocate(byteCount: bsz,
                                                   alignment: 4096)
        defer { buf.deallocate() }
        while true {
            if cancelled { return false }
            let n = read(inp, buf, bsz)
            if n < 0 { err = String(cString: strerror(errno)); return false }
            if n == 0 { break }
            var w = 0
            while w < n {
                let k = write(out, buf + w, n - w)
                if k < 0 {
                    err = String(cString: strerror(errno)); return false
                }
                w += k
            }
            done += UInt64(n)
        }
        fsync(out)
        return true
    }

    /// lc_walk：top 跟随符号链接（显式选择），递归内部跳过链接/特殊文件。
    private func walk(_ s: String, _ d: String, top: Bool) -> Bool {
        if cancelled { return false }
        guard let st = SftpKit.statPath(s, follow: top) else {
            err = String(cString: strerror(errno)); return false
        }
        if !top && (st.st_mode & S_IFMT) == S_IFLNK { nskip += 1; return true }
        if (st.st_mode & S_IFMT) == S_IFDIR {
            var sk = false
            guard let rd = resolveDst(d, isDir: true, skipped: &sk) else {
                if sk { nskip += 1; return true }
                if err == nil { err = L("无法解析目标路径") }
                return false
            }
            var ex = stat()
            if lstat(rd, &ex) != 0 {
                if Darwin.mkdir(rd, st.st_mode & 0o777) != 0 {
                    err = String(cString: strerror(errno)); return false
                }
                if created == nil { created = rd }
            }
            guard let ns = try? FileManager.default
                .contentsOfDirectory(atPath: s) else {
                err = String(cString: strerror(errno)); return false
            }
            for n in ns {
                if !walk(SftpKit.pathJoin(s, n),
                         SftpKit.pathJoin(rd, n), top: false) {
                    return false
                }
            }
            return true
        }
        if (st.st_mode & S_IFMT) != S_IFREG { nskip += 1; return true }
        var sk = false
        guard let rd = resolveDst(d, isDir: false, skipped: &sk) else {
            if sk { nskip += 1; return true }
            return false
        }
        return copyFile(s, rd, mode: st.st_mode)
    }

    /// 取消时清掉自建目录或半成品文件（lc_done 清理路径）。
    func cleanupPartial() {
        if let c = created { _ = LocalFs.rmrf(c) }
        else if let p = partial { unlink(p) }
    }
}

// ============================ toast ============================

/// 轻量 toast 条（linux hb_toast_undo_destroy / toast_action2 等价）：
/// 底部居中，文本 + 可选动作按钮，ttl 秒后自动消失。
final class SftpToast: NSView {
    private var timer: Timer?
    private var onDismiss: (() -> Void)?
    init(text: String, action: String? = nil,
         ttl: TimeInterval = 9,
         onAction: (() -> Void)? = nil,
         onDismiss: (() -> Void)? = nil) {
        super.init(frame: .zero)
        self.onDismiss = onDismiss
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.backgroundColor = NSColor(white: 0.12, alpha: 0.92).cgColor
        let l = NSTextField(labelWithString: text)
        l.textColor = .white
        l.font = .systemFont(ofSize: 12)
        l.translatesAutoresizingMaskIntoConstraints = false
        addSubview(l)
        var cs: [NSLayoutConstraint] = [
            l.leadingAnchor.constraint(equalTo: leadingAnchor,
                                       constant: 14),
            l.centerYAnchor.constraint(equalTo: centerYAnchor),
            heightAnchor.constraint(equalToConstant: 34),
        ]
        if let action, let onAction {
            let b = NSButton(title: action, target: nil, action: nil)
            b.bezelStyle = .rounded
            b.font = .systemFont(ofSize: 12)
            b.translatesAutoresizingMaskIntoConstraints = false
            b.setAccessibilityIdentifier("toast-action")
            b.target = self
            b.action = #selector(fire)
            action_ = onAction
            addSubview(b)
            cs += [
                b.leadingAnchor.constraint(equalTo: l.trailingAnchor,
                                           constant: 10),
                b.centerYAnchor.constraint(equalTo: centerYAnchor),
                b.trailingAnchor.constraint(equalTo: trailingAnchor,
                                            constant: -10),
            ]
        } else {
            cs.append(l.trailingAnchor.constraint(
                equalTo: trailingAnchor, constant: -14))
        }
        NSLayoutConstraint.activate(cs)
        timer = Timer.scheduledTimer(withTimeInterval: ttl,
                                     repeats: false) { [weak self] _ in
            self?.dismiss()
        }
    }
    required init?(coder: NSCoder) { nil }
    private var action_: (() -> Void)?
    @objc private func fire() { action_?(); dismiss() }
    func dismiss() {
        timer?.invalidate(); timer = nil
        let d = onDismiss; onDismiss = nil
        removeFromSuperview()
        d?()
    }
    deinit { timer?.invalidate() }
}

// ============================ Pane ============================

final class SftpPane: NSObject, ConnDelegate, NSTableViewDataSource,
                      NSTableViewDelegate {
    enum State { case select, conn, browse }
    unowned let v: SftpView
    let side: Int
    private(set) var state: State = .select

    let container = NSView()
    private let selectView = NSView()
    private let connView = NSView()
    private let browseView = NSView()

    // select
    private var selHostBtns: [NSButton] = []
    private let selFilter = NSTextField()

    // conn
    var ctl: ConnCtl?
    var connHostId: String?
    private var connHead: [NSView] = []
    private var logBox: NSStackView?
    private var logScroll: NSScrollView?
    private var logWrap: NSView?
    private var hideLogBtn: HBtn?
    private var logHidden = false
    private var logTop: NSLayoutConstraint?
    private var logHeight: NSLayoutConstraint?
    private var hadError = false
    private var editBtn: HBtn?

    // browse
    private(set) var isLocal = false
    private(set) var hostId: String?
    private(set) var hname = ""
    private var osKey = "generic"
    var fs: Fs?
    var sess: Session?
    private(set) var cwd: String?
    private var back: [String] = []
    private var fwd: [String] = []
    private var items: [FsEntry] = []
    var shown: [FsEntry] = []
    private var showHidden = false
    private var loading = false
    private var listGen: UInt = 0
    private var gen: UInt = 0
    private var backB, fwdB, upB: NSButton!
    private var crumbsBox: NSStackView!
    private var filterRow: NSView!
    private let filterEntry = NSTextField()
    private var headTile: NSView?
    private var titleL: NSTextField?
    let table = SftpTable()
    private var tableScroll: NSScrollView?
    private var emptyLbl: NSTextField?

    // delete-undo / open
    private var pendDels: [PendDel] = []
    private var openCtxs: [OpenCtx] = []
    private var openReqs: [UInt32: OpenCtx] = [:]

    init(v: SftpView, side: Int) {
        self.v = v; self.side = side
        super.init()
        build()
        setState(.select)
        table.pane = self
    }

    // ---------- 状态 ----------
    func setState(_ s: State) {
        state = s
        closeActions()
        selectView.isHidden = s != .select
        connView.isHidden = s != .conn
        browseView.isHidden = s != .browse
    }

    private func build() {
        [selectView, connView, browseView].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            container.addSubview($0)
        }
        NSLayoutConstraint.activate(
            [selectView, connView, browseView].flatMap { x in
                [x.leadingAnchor.constraint(equalTo: container.leadingAnchor),
                 x.trailingAnchor.constraint(equalTo: container.trailingAnchor),
                 x.topAnchor.constraint(equalTo: container.topAnchor),
                 x.bottomAnchor.constraint(equalTo: container.bottomAnchor)]
            })
        buildSelect(); buildConn(); buildBrowse()
    }

    // ---------- select ----------
    private func buildSelect() {
        selectView.wantsLayer = true
        selectView.layer?.hbBg = "card"
        let head = NSTextField(labelWithString:
            L("选择主机") + " · " + (side == 0 ? L("左栏") : L("右栏")))
        head.font = .systemFont(ofSize: 13.5, weight: .bold)
        head.translatesAutoresizingMaskIntoConstraints = false
        selectView.addSubview(head)
        let lb = HBtn(L("Local"), style: .primary, sm: true, icon: "monitor")
        lb.target = self
        lb.action = #selector(pickLocal)
        lb.translatesAutoresizingMaskIntoConstraints = false
        lb.setAccessibilityIdentifier("sftp-local-\(side)")
        selectView.addSubview(lb)
        sizeBtn(lb, sm: true)
        selFilter.placeholderString = L("过滤…")
        selFilter.translatesAutoresizingMaskIntoConstraints = false
        selFilter.target = self
        selFilter.action = #selector(rebuildHosts)
        selFilter.setAccessibilityIdentifier("sftp-selfilter-\(side)")
        selectView.addSubview(selFilter)
        let scr = NSScrollView()
        scr.translatesAutoresizingMaskIntoConstraints = false
        scr.hasVerticalScroller = false
        scr.scrollerStyle = .overlay
        scr.drawsBackground = false
        let box = FlippedStack()
        box.orientation = .vertical
        box.spacing = 10
        box.edgeInsets = NSEdgeInsets(top: 10, left: 14,
                                      bottom: 10, right: 14)
        box.translatesAutoresizingMaskIntoConstraints = false
        box.setAccessibilityIdentifier("sftp-hostlist-\(side)")
        scr.documentView = box
        hostBox = box
        selectView.addSubview(scr)
        let hint = NSTextField(labelWithString:
            L("选择两端后可双栏互传（远端↔远端 / 远端↔本地）· 双击文件即开始传输"))
        hint.font = .systemFont(ofSize: 11)
        hint.hbText = "text3"
        hint.alignment = .center
        hint.translatesAutoresizingMaskIntoConstraints = false
        selectView.addSubview(hint)
        NSLayoutConstraint.activate([
            head.topAnchor.constraint(equalTo: selectView.topAnchor,
                                      constant: 12),
            head.leadingAnchor.constraint(equalTo: selectView.leadingAnchor,
                                          constant: 16),
            lb.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            lb.trailingAnchor.constraint(equalTo: selectView.trailingAnchor,
                                         constant: -16),
            selFilter.topAnchor.constraint(equalTo: head.bottomAnchor,
                                           constant: 8),
            selFilter.leadingAnchor.constraint(
                equalTo: selectView.leadingAnchor, constant: 16),
            selFilter.trailingAnchor.constraint(
                equalTo: selectView.trailingAnchor, constant: -16),
            scr.topAnchor.constraint(equalTo: selFilter.bottomAnchor,
                                     constant: 6),
            scr.leadingAnchor.constraint(equalTo: selectView.leadingAnchor),
            scr.trailingAnchor.constraint(equalTo: selectView.trailingAnchor),
            scr.bottomAnchor.constraint(equalTo: hint.topAnchor,
                                        constant: -8),
            box.leadingAnchor.constraint(equalTo: scr.leadingAnchor),
            box.trailingAnchor.constraint(equalTo: scr.trailingAnchor),
            box.topAnchor.constraint(equalTo: scr.topAnchor),
            box.widthAnchor.constraint(equalTo: scr.widthAnchor),
            hint.leadingAnchor.constraint(
                equalTo: selectView.leadingAnchor, constant: 16),
            hint.trailingAnchor.constraint(
                equalTo: selectView.trailingAnchor, constant: -16),
            hint.bottomAnchor.constraint(
                equalTo: selectView.bottomAnchor, constant: -16),
        ])
        rebuildHosts()
    }
    private var hostBox: NSStackView!

    @objc private func rebuildHosts() {
        for b in selHostBtns { b.removeFromSuperview() }
        selHostBtns.removeAll()
        let q = selFilter.stringValue
        for o in Vault.shared.list("host") {
            defer { hb_obj_free(o) }
            let pr = hb_obj_get_str(o, "proto").str
            if !pr.isEmpty && pr != "ssh" { continue }
            let nm = hb_obj_get_str(o, "name").str
            let us = hb_obj_get_str(o, "user").str
            let ad = hb_obj_get_str(o, "address").str
            let os = hb_obj_get_str(o, "os").str
            let label = nm.isEmpty ? (ad.isEmpty ? "?" : ad) : nm
            if !q.isEmpty && !label.contains(q) && !us.contains(q) {
                continue
            }
            let b = HostItem()
            b.target = self
            b.action = #selector(pickHost(_:))
            b.setAccessibilityIdentifier("sftp-host-\(side)")
            let row = NSStackView()
            row.orientation = .horizontal
            row.spacing = 12
            row.edgeInsets = NSEdgeInsets(top: 10, left: 12,
                                        bottom: 10, right: 12)
            let tile = Palette.osTileView(
                os.isEmpty || os == "auto" ? "generic" : os, size: 30)
            tile.translatesAutoresizingMaskIntoConstraints = false
            row.addArrangedSubview(tile)
            let col = NSStackView()
            col.orientation = .vertical
            col.spacing = 1
            col.alignment = .leading
            let t = NSTextField(labelWithString: label)
            t.font = .systemFont(ofSize: 13, weight: .medium)
            let s = NSTextField(labelWithString: "ssh, \(us)")
            s.font = .systemFont(ofSize: 11)
            s.hbText = "text3"
            col.addArrangedSubview(t); col.addArrangedSubview(s)
            row.addArrangedSubview(col)
            row.translatesAutoresizingMaskIntoConstraints = false
            b.addSubview(row)
            b.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                row.leadingAnchor.constraint(equalTo: b.leadingAnchor),
                row.trailingAnchor.constraint(equalTo: b.trailingAnchor),
                row.topAnchor.constraint(equalTo: b.topAnchor),
                row.bottomAnchor.constraint(equalTo: b.bottomAnchor),
            ])
            b.identifier = .init(hb_obj_id(o).str)  // 存 host id
            selHostBtns.append(b)
            hostBox.addArrangedSubview(b)
            b.leadingAnchor.constraint(equalTo: hostBox.leadingAnchor)
                .isActive = true
            b.trailingAnchor.constraint(equalTo: hostBox.trailingAnchor)
                .isActive = true
        }
    }
    @objc private func pickLocal() { pick(nil) }
    @objc private func pickHost(_ b: NSButton) {
        pick(b.identifier?.rawValue)
    }

    // ---------- conn（prototype sftpConnectPanel） ----------
    private func buildConn() {
        connView.wantsLayer = true
        connView.layer?.hbBg = "card"
        // head：os tile + name + sub + 隐藏日志（居中 max 430）
        let head = NSView()
        head.translatesAutoresizingMaskIntoConstraints = false
        connView.addSubview(head)
        connHead.append(head)
        let hb = HBtn(L("隐藏日志"), style: .gray, sm: true)
        hb.target = self
        hb.action = #selector(toggleLog)
        hb.translatesAutoresizingMaskIntoConstraints = false
        hb.setAccessibilityIdentifier("sftp-hidelog-\(side)")
        hideLogBtn = hb
        head.addSubview(hb)
        sizeBtn(hb, sm: true)
        NSLayoutConstraint.activate([
            head.topAnchor.constraint(equalTo: connView.topAnchor,
                                      constant: 40),
            head.centerXAnchor.constraint(equalTo: connView.centerXAnchor),
            head.heightAnchor.constraint(equalToConstant: 34),
            hb.trailingAnchor.constraint(equalTo: head.trailingAnchor),
            hb.centerYAnchor.constraint(equalTo: head.centerYAnchor),
        ])
        pinCenterWidth(head, in: connView)
        // log：灰底圆角盒（prototype connect-log）
        let wrap = NSView()
        wrap.wantsLayer = true
        wrap.layer?.hbBg = "subtle"
        wrap.layer?.cornerRadius = 10
        wrap.translatesAutoresizingMaskIntoConstraints = false
        connView.addSubview(wrap)
        logWrap = wrap
        let scr = NSScrollView()
        scr.translatesAutoresizingMaskIntoConstraints = false
        scr.hasVerticalScroller = false
        scr.drawsBackground = false
        scr.contentView.drawsBackground = false
        scr.scrollerStyle = .overlay
        wrap.addSubview(scr)
        logScroll = scr
        let lb = FlippedStack()
        lb.orientation = .vertical
        lb.alignment = .leading
        lb.spacing = 6
        lb.edgeInsets = NSEdgeInsets(top: 16, left: 18,
                                     bottom: 16, right: 18)
        lb.translatesAutoresizingMaskIntoConstraints = false
        scr.documentView = lb
        logBox = lb
        let top = wrap.topAnchor.constraint(equalTo: head.bottomAnchor,
                                            constant: 30)
        let h = wrap.heightAnchor.constraint(equalToConstant: 200)
        logTop = top
        logHeight = h
        NSLayoutConstraint.activate([
            top, h,
            wrap.centerXAnchor.constraint(equalTo: connView.centerXAnchor),
            scr.leadingAnchor.constraint(equalTo: wrap.leadingAnchor),
            scr.trailingAnchor.constraint(equalTo: wrap.trailingAnchor),
            scr.topAnchor.constraint(equalTo: wrap.topAnchor),
            scr.bottomAnchor.constraint(equalTo: wrap.bottomAnchor),
            lb.leadingAnchor.constraint(equalTo: scr.leadingAnchor),
            lb.trailingAnchor.constraint(equalTo: scr.trailingAnchor),
            lb.topAnchor.constraint(equalTo: scr.topAnchor),
            lb.widthAnchor.constraint(equalTo: scr.widthAnchor),
        ])
        pinCenterWidth(wrap, in: connView)
        // 按钮行：关闭 …（编辑主机）… 重新开始
        let foot = NSView()
        foot.translatesAutoresizingMaskIntoConstraints = false
        connView.addSubview(foot)
        let cb = HBtn(L("关闭"), style: .gray)
        cb.target = self
        cb.action = #selector(connClose)
        cb.translatesAutoresizingMaskIntoConstraints = false
        cb.setAccessibilityIdentifier("sftp-close-\(side)")
        foot.addSubview(cb)
        sizeBtn(cb, sm: false)
        let eb = HBtn(L("编辑主机"), style: .gray)
        eb.target = self
        eb.action = #selector(editHost)
        eb.translatesAutoresizingMaskIntoConstraints = false
        eb.isHidden = true
        editBtn = eb
        foot.addSubview(eb)
        sizeBtn(eb, sm: false)
        let rb = HBtn(L("重新开始"), style: .primary)
        rb.target = self
        rb.action = #selector(connRetry)
        rb.translatesAutoresizingMaskIntoConstraints = false
        rb.setAccessibilityIdentifier("sftp-retry-\(side)")
        foot.addSubview(rb)
        sizeBtn(rb, sm: false)
        NSLayoutConstraint.activate([
            foot.topAnchor.constraint(equalTo: scr.bottomAnchor,
                                      constant: 18),
            foot.centerXAnchor.constraint(equalTo: connView.centerXAnchor),
            foot.heightAnchor.constraint(equalToConstant: 32),
            foot.bottomAnchor.constraint(
                lessThanOrEqualTo: connView.bottomAnchor, constant: -12),
            cb.leadingAnchor.constraint(equalTo: foot.leadingAnchor),
            cb.centerYAnchor.constraint(equalTo: foot.centerYAnchor),
            eb.leadingAnchor.constraint(equalTo: cb.trailingAnchor,
                                        constant: 9),
            eb.centerYAnchor.constraint(equalTo: foot.centerYAnchor),
            rb.trailingAnchor.constraint(equalTo: foot.trailingAnchor),
            rb.centerYAnchor.constraint(equalTo: foot.centerYAnchor),
        ])
        pinCenterWidth(foot, in: connView)
    }
    /// prototype max-width:430 居中列：宽 430，窄 pane 时随 pane 收缩。
    private func pinCenterWidth(_ v: NSView, in parent: NSView) {
        let w = v.widthAnchor.constraint(equalToConstant: 430)
        w.priority = .defaultHigh
        NSLayoutConstraint.activate([
            w,
            v.widthAnchor.constraint(lessThanOrEqualTo: parent.widthAnchor,
                                     constant: -28),
        ])
    }
    private func sizeBtn(_ b: HBtn, sm: Bool) {
        b.widthAnchor.constraint(
            equalToConstant: b.intrinsicContentSize.width + (sm ? 20 : 28))
            .isActive = true
        b.heightAnchor.constraint(equalToConstant: sm ? 26 : 32).isActive = true
    }
    private func connReset() {
        hadError = false
        editBtn?.isHidden = true
        logBox?.arrangedSubviews.forEach {
            logBox?.removeArrangedSubview($0); $0.removeFromSuperview()
        }
        logHidden = false
        setLogHidden(false)
    }
    private func setLogHidden(_ h: Bool) {
        logScroll?.isHidden = h
        logWrap?.isHidden = h
        logTop?.constant = h ? 0 : 30
        logHeight?.constant = h ? 0 : 200
        hideLogBtn?.titleText = h ? L("显示日志") : L("隐藏日志")
    }
    @objc private func toggleLog() {
        logHidden.toggle()
        setLogHidden(logHidden)
    }
    @objc private func connClose() {
        teardownConn()
        setState(.select)
    }
    @objc private func connRetry() {
        let id = connHostId
        pick(id)
    }
    private func connError(_ title: String, details: String?, code: Int) {
        hadError = true
        editBtn?.isHidden = false
        appendLog(title.isEmpty ? L("连接失败") : title, err: true)
        if let d = details, !d.isEmpty { appendLog(d, err: true) }
    }
    @objc private func editHost() {
        guard let id = connHostId else { return }
        v.hostEditor?(id)
    }

    // ---------- ConnDelegate ----------
    func conn(_ c: ConnCtl, stage idx: Int, line: String, mark: Character) {
        appendLog(line, err: mark == "e")
    }
    private func appendLog(_ line: String, err: Bool) {
        let row = NSStackView()
        row.orientation = .horizontal
        row.spacing = 6
        let ic = NSImageView(image: hbIcon(err ? "x" : "user",
                            size: 11,
                            tint: err ? Theme.red.color
                                      : Theme.dyn("text2")))
        let l = NSTextField(wrappingLabelWithString: line)
        l.font = .systemFont(ofSize: 12.5)
        l.hbText = err ? "red" : "text"
        l.maximumNumberOfLines = 4
        l.preferredMaxLayoutWidth = 370
        row.addArrangedSubview(ic); row.addArrangedSubview(l)
        logBox?.addArrangedSubview(row)
        if let sv = logScroll {
            sv.layoutSubtreeIfNeeded()
            sv.documentView?.scroll(
                NSPoint(x: 0, y: sv.documentView!.bounds.maxY))
        }
    }
    func connSession(_ c: ConnCtl, _ s: Session) {
        sess = s
        hb_sftp_set_concurrency(s.sess, Int32(v.conc))
        s.onEvent = { [weak self] ev in
            guard let self else { return }
            var e = ev
            if self.ctl?.handleEvent(&e) == true { return }
            if let rf = self.fs as? RemoteFs, rf.handleEvent(&e) { return }
            if ev.type == HB_SESS_EV_SFTP, let r = ev.sftp {
                // "打开"下载 → openReqs；其余 → 传输队列
                if let oc = self.openReqs[r.pointee.req] {
                    self.openDone(oc: oc, r: r.pointee)
                } else {
                    self.v.xferDispatch(self, r.pointee)
                }
            }
        }
        s.drain()
    }
    func connConnected(_ c: ConnCtl) {
        guard sess != nil else { return }
        fs = RemoteFs(sess: sess!)
        fs?.realpath(nil) { [weak self] r in
            guard let self else { return }
            self.browseTo(r.ok ? (r.path ?? "/") : "/", nav: true)
        }
    }
    func connFailed(_ c: ConnCtl, code: Int, title: String,
                    details: String?) {
        connError(title, details: details, code: code)
        v.xferFailSession(sess, title.isEmpty ? L("连接失败") : title)
    }
    func connDisconnected(_ c: ConnCtl) {
        setState(.conn)
        connError(L("连接已断开"), details: nil, code: 0)
        v.xferFailSession(sess, L("连接已断开"))
        sess = nil
    }
    func connExited(_ c: ConnCtl, code: Int, reason: String) {
        connDisconnected(c)
    }
    func connOSInfo(_ c: ConnCtl, id: String, pretty: String) {}

    // ---------- 生命周期 ----------
    func teardownConn() {
        sess = nil
        for (_, oc) in openReqs { oc.free() }
        openReqs.removeAll()
        openCtxs.forEach { $0.free() }
        openCtxs.removeAll()
        // 待删项：pane 关闭/换主机前立即真删
        for var pd in pendDels {
            if !pd.undone, let fs {
                for p in pd.paths { fs.remove(p, recursive: true) { _ in } }
            }
            pd.paths.removeAll()
        }
        pendDels.removeAll()
        fs?.invalidate(); fs = nil
        if let c = ctl { c.cancel(); c.join(); ctl = nil }
        gen &+= 1
    }

    /// pane_pick：nil → Local（直接浏览 home）。
    func pick(_ hid: String?) {
        teardownConn()
        connHostId = hid
        hostId = hid
        back.removeAll(); fwd.removeAll()
        guard let hid else {
            isLocal = true
            hname = L("Local"); osKey = "local"
            fs = LocalFs()
            fs?.realpath(nil) { [weak self] r in
                guard let self else { return }
                self.browseTo(r.ok ? (r.path ?? NSHomeDirectory()) : "/",
                              nav: true)
            }
            return
        }
        isLocal = false
        if let h = Vault.shared.get(hid) {
            let nm = hb_obj_get_str(h, "name").str
            let ad = hb_obj_get_str(h, "address").str
            let po = hb_obj_get_int(h, "port", 22)
            let os = hb_obj_get_str(h, "os").str
            hname = nm.isEmpty ? hid : nm
            osKey = (!os.isEmpty && os != "auto") ? os : "generic"
            setConnHead(name: hname,
                        sub: "SFTP · SSH \(ad.isEmpty ? "?" : ad):\(po)")
            hb_obj_free(h)
        } else {
            hname = hid; osKey = "generic"
            setConnHead(name: hid, sub: "SFTP")
        }
        connReset()
        setState(.conn)
        if ctl == nil {
            let c = ConnCtl(mode: .sftp, hostId: hid,
                            window: container.window)
            c.delegate = self
            ctl = c
        }
        ctl?.start(rows: 24, cols: 80)
    }
    private func setConnHead(name: String, sub: String) {
        let head = connHead[0]
        head.subviews.filter { $0 !== hideLogBtn }
            .forEach { $0.removeFromSuperview() }
        let tile = Palette.osTileView(osKey, size: 30)
        tile.translatesAutoresizingMaskIntoConstraints = false
        head.addSubview(tile)
        let col = NSStackView()
        col.orientation = .vertical
        col.spacing = 1
        col.alignment = .leading
        let t = NSTextField(labelWithString: name)
        t.font = .systemFont(ofSize: 13, weight: .bold)
        let s = NSTextField(labelWithString: sub)
        s.font = .systemFont(ofSize: 12)
        s.hbText = "text3"
        col.addArrangedSubview(t); col.addArrangedSubview(s)
        col.translatesAutoresizingMaskIntoConstraints = false
        head.addSubview(col)
        NSLayoutConstraint.activate([
            tile.leadingAnchor.constraint(equalTo: head.leadingAnchor),
            tile.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            col.leadingAnchor.constraint(equalTo: tile.trailingAnchor,
                                         constant: 12),
            col.centerYAnchor.constraint(equalTo: head.centerYAnchor),
        ])
    }

    // ---------- browse 构建 ----------
    private func buildBrowse() {
        browseView.wantsLayer = true
        browseView.layer?.hbBg = "card"
        // head：tile + title + Filter + Actions▾（prototype sftp-head）
        let head = NSView()
        head.translatesAutoresizingMaskIntoConstraints = false
        browseView.addSubview(head)
        let tile = Palette.osTileView("generic", size: 30)
        tile.translatesAutoresizingMaskIntoConstraints = false
        headTile = tile
        head.addSubview(tile)
        let t = NSTextField(labelWithString: "")
        t.font = .systemFont(ofSize: 13.5, weight: .bold)
        t.translatesAutoresizingMaskIntoConstraints = false
        titleL = t
        head.addSubview(t)
        let fb = HBtn(L("Filter"), style: .ghost, sm: true, icon: "search")
        fb.target = self
        fb.action = #selector(toggleFilter)
        fb.translatesAutoresizingMaskIntoConstraints = false
        fb.setAccessibilityIdentifier("sftp-filter-\(side)")
        head.addSubview(fb)
        sizeBtn(fb, sm: true)
        let mb = HBtn(L("Actions ▾"), style: .ghost, sm: true)
        mb.target = self
        mb.action = #selector(showActions(_:))
        mb.translatesAutoresizingMaskIntoConstraints = false
        mb.setAccessibilityIdentifier("sftp-actions-\(side)")
        head.addSubview(mb)
        sizeBtn(mb, sm: true)
        NSLayoutConstraint.activate([
            head.topAnchor.constraint(equalTo: browseView.topAnchor,
                                      constant: 12),
            head.leadingAnchor.constraint(equalTo: browseView.leadingAnchor,
                                          constant: 16),
            head.trailingAnchor.constraint(
                equalTo: browseView.trailingAnchor, constant: -16),
            head.heightAnchor.constraint(equalToConstant: 30),
            tile.leadingAnchor.constraint(equalTo: head.leadingAnchor),
            tile.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            t.leadingAnchor.constraint(equalTo: tile.trailingAnchor,
                                       constant: 10),
            t.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            mb.trailingAnchor.constraint(equalTo: head.trailingAnchor),
            mb.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            fb.trailingAnchor.constraint(equalTo: mb.leadingAnchor,
                                         constant: -6),
            fb.centerYAnchor.constraint(equalTo: head.centerYAnchor),
        ])
        // filter row（默认隐藏）
        let fr = NSView()
        fr.translatesAutoresizingMaskIntoConstraints = false
        browseView.addSubview(fr)
        filterRow = fr
        filterEntry.placeholderString = L("过滤文件名…")
        filterEntry.translatesAutoresizingMaskIntoConstraints = false
        filterEntry.target = self
        filterEntry.action = #selector(filterChanged)
        filterEntry.setAccessibilityIdentifier("sftp-filterfield-\(side)")
        fr.addSubview(filterEntry)
        NSLayoutConstraint.activate([
            fr.topAnchor.constraint(equalTo: head.bottomAnchor,
                                    constant: 4),
            fr.leadingAnchor.constraint(equalTo: browseView.leadingAnchor,
                                        constant: 14),
            fr.trailingAnchor.constraint(
                equalTo: browseView.trailingAnchor, constant: -14),
            fr.heightAnchor.constraint(equalToConstant: 22),
            filterEntry.leadingAnchor.constraint(equalTo: fr.leadingAnchor),
            filterEntry.trailingAnchor.constraint(equalTo: fr.trailingAnchor),
            filterEntry.centerYAnchor.constraint(equalTo: fr.centerYAnchor),
        ])
        fr.isHidden = true
        // nav row：‹ › ↑ + crumbs（prototype navbtn / crumb）
        let nav = NSStackView()
        nav.orientation = .horizontal
        nav.spacing = 6
        nav.translatesAutoresizingMaskIntoConstraints = false
        browseView.addSubview(nav)
        func navBtn(_ glyph: String, _ sel: Selector, _ id: String) -> NSButton {
            let b = NavGlyph(glyph)
            b.target = self
            b.action = sel
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(equalToConstant: 26).isActive = true
            b.heightAnchor.constraint(equalToConstant: 24).isActive = true
            b.setAccessibilityIdentifier(id)
            nav.addArrangedSubview(b)
            return b
        }
        backB = navBtn("‹", #selector(goBack), "sftp-back-\(side)")
        fwdB = navBtn("›", #selector(goFwd), "sftp-fwd-\(side)")
        upB = navBtn("↑", #selector(goUp), "sftp-up-\(side)")
        let cb = NSStackView()
        cb.orientation = .horizontal
        cb.spacing = 6
        cb.translatesAutoresizingMaskIntoConstraints = false
        crumbsBox = cb
        nav.addArrangedSubview(cb)
        NSLayoutConstraint.activate([
            nav.topAnchor.constraint(equalTo: fr.bottomAnchor, constant: 4),
            nav.leadingAnchor.constraint(equalTo: browseView.leadingAnchor,
                                         constant: 16),
            nav.trailingAnchor.constraint(
                lessThanOrEqualTo: browseView.trailingAnchor, constant: -16),
            nav.heightAnchor.constraint(equalToConstant: 26),
        ])
        // table
        let scr = NSScrollView()
        scr.translatesAutoresizingMaskIntoConstraints = false
        scr.hasVerticalScroller = true
        scr.scrollerStyle = .overlay
        tableScroll = scr
        let cols: [(String, String, CGFloat, Bool)] = [
            ("name", L("Name"), 220, true),
            ("mtime", L("修改时间"), 90, false),
            ("size", L("大小"), 70, false),
            ("kind", L("类型"), 60, false),
            ("mode", L("权限"), 80, false),
        ]
        for (id, tt, w, grow) in cols {
            let c = NSTableColumn(identifier: .init(id))
            c.title = tt
            c.width = w
            if !grow { c.resizingMask = .userResizingMask }
            c.minWidth = min(w, 40)
            c.sortDescriptorPrototype = NSSortDescriptor(key: id,
                                                         ascending: true)
            table.addTableColumn(c)
        }
        // name 列随宽度伸展（linux expand=TRUE）
        table.columnAutoresizingStyle =
            .firstColumnOnlyAutoresizingStyle
        table.headerView = SftpHeaderView()
        table.dataSource = self
        table.delegate = self
        table.rowHeight = 33
        table.intercellSpacing = NSSize(width: 28, height: 0)
        table.selectionHighlightStyle = .none
        table.backgroundColor = Theme.dyn("card")
        table.allowsMultipleSelection = true
        table.allowsEmptySelection = true
        table.doubleAction = #selector(rowActivated)
        table.target = self
        table.setAccessibilityIdentifier("sftp-table-\(side)")
        table.registerForDraggedTypes([.string, .fileURL])
        table.setDraggingSourceOperationMask(.copy, forLocal: false)
        table.setDraggingSourceOperationMask(.copy, forLocal: true)
        scr.documentView = table
        browseView.addSubview(scr)
        let el = NSTextField(labelWithString: L("空目录 / 无匹配文件"))
        el.font = .systemFont(ofSize: 12.5)
        el.hbText = "text3"
        el.translatesAutoresizingMaskIntoConstraints = false
        emptyLbl = el
        scr.addSubview(el)
        NSLayoutConstraint.activate([
            scr.topAnchor.constraint(equalTo: nav.bottomAnchor, constant: 10),
            scr.leadingAnchor.constraint(equalTo: browseView.leadingAnchor),
            scr.trailingAnchor.constraint(
                equalTo: browseView.trailingAnchor),
            scr.bottomAnchor.constraint(equalTo: browseView.bottomAnchor),
            el.centerXAnchor.constraint(equalTo: scr.centerXAnchor),
            el.topAnchor.constraint(equalTo: scr.topAnchor, constant: 40),
        ])
        // ctx 菜单
        let cm = NSMenu()
        for (tt, sel) in [
            (L("打开"), #selector(actOpen)),
            (L("复制到对侧栏"), #selector(actCopy)),
            (L("重命名…"), #selector(actRename)),
            (L("删除"), #selector(actDelete)),
            (L("新建文件夹…"), #selector(actMkdir)),
            (L("修改权限…"), #selector(actChmod)),
            (L("刷新"), #selector(actRefresh)),
            (L("全选"), #selector(actSelectAll)),
            (L("显示隐藏文件"), #selector(actHidden)),
        ] as [(String, Selector)] {
            let i = cm.addItem(withTitle: tt, action: sel,
                               keyEquivalent: "")
            i.target = self
        }
        table.menu = cm
    }

    // ---------- 列表模型 ----------
    /// shown 代次：applyShown/rebuildShown 每次递增，后台算完的过期
    /// 结果（filter 连打、翻页中途）直接丢弃，只有最新一代写回。
    private var shownGen: UInt = 0
    /// 过滤 + 排序的纯计算（后台线程可跑；目录恒在前，同 linux
    /// dirfirst+cols）。2000 条 localized 排序在 debug 下单列 ~0.7s。
    private static func computeShown(_ src: [FsEntry], hidden: Bool,
                                     q: String, key: SftpKit.SortKey,
                                     asc: Bool) -> [FsEntry] {
        var f = src
        if !hidden { f = f.filter { !$0.name.hasPrefix(".") } }
        if !q.isEmpty {
            f = f.filter { $0.name.lowercased().contains(q) }
        }
        return SftpKit.sortEntries(f, key: key, ascending: asc,
            isDir: { $0.isDir }, name: { $0.name },
            value: { e in
                switch key {
                case .mtime: return e.mtime
                case .size: return Int64(e.size)
                case .mode: return Int64(e.mode & 0o777)
                default: return 0
                }
            })
    }
    /// shown 写回后的表刷新：全量 reloadData 会把全部可见 rowview
    /// 销毁重建（debug ~5ms/行）；行数变化通知 + 仅重载可见行即可，
    /// 离屏行滚动到视野时由 viewFor 惰性生成。
    private func reloadVisible() {
        emptyLbl?.isHidden = !shown.isEmpty
        table.noteNumberOfRowsChanged()
        let r = table.rows(in: table.visibleRect)
        guard r.length > 0, let range = Range(r) else {
            table.reloadData()
            return
        }
        table.reloadData(
            forRowIndexes: IndexSet(range),
            columnIndexes: IndexSet(integersIn:
                0..<table.numberOfColumns))
    }
    /// shown = items 过滤 + 排序：主线程只拍快照和写回，计算在后台。
    func applyShown() {
        let src = items
        let hidden = showHidden
        let q = filterEntry.stringValue.trimmingCharacters(
            in: .whitespaces).lowercased()
        let sd = table.sortDescriptors.first
        let key = SftpKit.SortKey(raw: sd?.key ?? "name")
        let asc = sd?.ascending ?? true
        shownGen &+= 1
        let g = shownGen
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let out = Self.computeShown(src, hidden: hidden, q: q,
                                        key: key, asc: asc)
            DispatchQueue.main.async {
                guard let self, self.shownGen == g else { return }
                self.shown = out
                self.reloadVisible()
            }
        }
    }
    func refresh() {
        guard let fs, let cwd else { return }
        loading = true
        let g = { () -> UInt in self.gen &+= 1; return self.gen }()
        let lgen = { () -> UInt in self.listGen &+= 1; return self.listGen }()
        fs.list(cwd) { [weak self] r in
            guard let self, self.listGen == lgen else { return }
            self.loading = false
            if !r.ok {
                self.v.toast(L("读取目录失败") + ": " +
                             (r.message ?? L("未知错误")))
                return
            }
            self.rebuildShown(r.entries, cwd: cwd, lgen: lgen)
        }
        _ = g
    }
    /// 大目录（2000+）：entry 重建 + 过滤 + 排序一次性在后台算完
    /// （原逐条 FsEntry 重建 + applyShown 各一遍，debug 下合计 ~1s
    /// 主线程），回主线程一次提交 items+shown+reloadData。
    private func rebuildShown(_ es: [FsEntry], cwd: String, lgen: UInt) {
        let hidden = showHidden
        let q = filterEntry.stringValue.trimmingCharacters(
            in: .whitespaces).lowercased()
        let sd = table.sortDescriptors.first
        let key = SftpKit.SortKey(raw: sd?.key ?? "name")
        let asc = sd?.ascending ?? true
        shownGen &+= 1
        let g = shownGen
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            // 一次性替换（linux splice 大目录修复同义）
            var out: [FsEntry] = []
            out.reserveCapacity(es.count)
            for e in es {
                if e.name == "." || e.name == ".." ||
                   e.name.contains("/") { continue }
                out.append(FsEntry(name: e.name,
                    path: SftpKit.pathJoin(cwd, e.name),
                    size: e.size, mtime: e.mtime, mode: e.mode,
                    isDir: e.isDir, isLink: e.isLink))
            }
            let sh = Self.computeShown(out, hidden: hidden, q: q,
                                       key: key, asc: asc)
            DispatchQueue.main.async {
                guard let self, self.listGen == lgen,
                      self.shownGen == g else { return }
                self.items = out
                self.shown = sh
                self.reloadVisible()
            }
        }
    }
    private func navSync() {
        backB.isEnabled = !back.isEmpty
        fwdB.isEnabled = !fwd.isEmpty
        upB.isEnabled = cwd != nil && cwd != "/"
        for s in crumbsBox.arrangedSubviews {
            crumbsBox.removeArrangedSubview(s)
            s.removeFromSuperview()
        }
        for (i, c) in SftpKit.crumbs(cwd ?? "/").enumerated() {
            if i > 0 {
                let sep = NSTextField(labelWithString: "›")
                sep.font = .systemFont(ofSize: 12.5)
                sep.hbText = "text3"
                crumbsBox.addArrangedSubview(sep)
            }
            let b = CrumbChip(c.label)
            b.target = self
            b.action = #selector(crumb(_:))
            b.setAccessibilityIdentifier("sftp-crumb-\(side)")
            b.identifier = .init(c.path)
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(
                equalToConstant: b.intrinsicContentSize.width + 20)
                .isActive = true
            b.heightAnchor.constraint(equalToConstant: 24).isActive = true
            crumbsBox.addArrangedSubview(b)
        }
        titleL?.stringValue = isLocal
            ? "Local · \(cwd ?? NSHomeDirectory())" : hname
        syncHeadTile()
    }
    private func syncHeadTile() {
        guard let old = headTile, let head = old.superview,
              let tl = titleL else { return }
        let tile = Palette.osTileView(isLocal ? "local" : osKey, size: 30)
        tile.translatesAutoresizingMaskIntoConstraints = false
        head.addSubview(tile)
        NSLayoutConstraint.activate([
            tile.leadingAnchor.constraint(equalTo: head.leadingAnchor),
            tile.centerYAnchor.constraint(equalTo: head.centerYAnchor),
            tl.leadingAnchor.constraint(equalTo: tile.trailingAnchor,
                                        constant: 10),
        ])
        old.removeFromSuperview()
        headTile = tile
    }
    func browseTo(_ path: String, nav: Bool) {
        if nav, let c = cwd { back.append(c); fwd.removeAll() }
        cwd = path.isEmpty ? "/" : path
        setState(.browse)
        navSync()
        refresh()
    }
    @objc private func crumb(_ b: NSButton) {
        guard let p = b.identifier?.rawValue, p != cwd else { return }
        browseTo(p, nav: true)
    }
    @objc private func goUp() {
        guard let c = cwd, c != "/" else { return }
        browseTo(SftpKit.parentDir(c), nav: true)
    }
    @objc private func goBack() {
        guard let t = back.popLast() else { return }
        if let c = cwd { fwd.append(c) }
        cwd = t; navSync(); refresh()
    }
    @objc private func goFwd() {
        guard let t = fwd.popLast() else { return }
        if let c = cwd { back.append(c) }
        cwd = t; navSync(); refresh()
    }
    // ---------- Actions 下拉（prototype .menu） ----------
    private var menuPanel: NSView?
    private var menuMonitor: Any?
    @objc private func showActions(_ b: NSButton) {
        closeActions()
        guard let cv = container.window?.contentView else { return }
        let hasSel = !selItems().isEmpty
        let rows: [(String, Selector?, Bool, Bool)] = [
            (L("打开"), #selector(actOpen), hasSel, false),
            (L("复制到对侧栏"), #selector(actCopy), hasSel, false),
            (L("重命名…"), #selector(actRename), hasSel, false),
            (L("删除"), #selector(actDelete), hasSel, true),
            ("", nil, false, false),
            (L("刷新"), #selector(actRefresh), true, false),
            (L("新建文件夹…"), #selector(actMkdir), true, false),
            (showHidden ? L("不显示隐藏文件") : L("显示隐藏文件"),
             #selector(actHidden), true, false),
            (L("修改权限…"), #selector(actChmod), hasSel, false),
            (L("全选"), #selector(actSelectAll), true, false),
            ("", nil, false, false),
            (L("关闭此栏"), #selector(actClosePane), true, true),
        ]
        let panel = NSView()
        panel.wantsLayer = true
        panel.layer?.hbBg = "card"
        panel.layer?.cornerRadius = 10
        panel.layer?.shadowColor = Theme.RGBA(0x141e37).color.cgColor
        panel.layer?.shadowOpacity = 0.22
        panel.layer?.shadowRadius = 20
        panel.layer?.shadowOffset = NSSize(width: 0, height: -6)
        let stack = FlippedStack()
        stack.orientation = .vertical
        stack.spacing = 0
        stack.edgeInsets = NSEdgeInsets(top: 6, left: 6,
                                      bottom: 6, right: 6)
        panel.addSubview(stack)
        var h: CGFloat = 12
        for (i, r) in rows.enumerated() {
            if r.1 == nil {
                let sep = NSView()
                sep.translatesAutoresizingMaskIntoConstraints = false
                stack.addArrangedSubview(sep)
                sep.heightAnchor.constraint(equalToConstant: 11).isActive = true
                sep.leadingAnchor.constraint(
                    equalTo: stack.leadingAnchor).isActive = true
                sep.trailingAnchor.constraint(
                    equalTo: stack.trailingAnchor).isActive = true
                let ln = NSView()
                ln.wantsLayer = true
                ln.layer?.hbBg = "line"
                ln.translatesAutoresizingMaskIntoConstraints = false
                sep.addSubview(ln)
                NSLayoutConstraint.activate([
                    ln.centerYAnchor.constraint(equalTo: sep.centerYAnchor),
                    ln.leadingAnchor.constraint(
                        equalTo: sep.leadingAnchor, constant: 8),
                    ln.trailingAnchor.constraint(
                        equalTo: sep.trailingAnchor, constant: -8),
                    ln.heightAnchor.constraint(equalToConstant: 1),
                ])
                h += 11
                continue
            }
            let it = MenuItem(r.0, red: r.3, enabled: r.2)
            it.fire = { [weak self] in
                self?.closeActions()
                if let s = r.1 { self?.perform(s) }
            }
            it.setAccessibilityIdentifier("sftp-menuitem-\(side)-\(i)")
            it.translatesAutoresizingMaskIntoConstraints = false
            stack.addArrangedSubview(it)
            it.widthAnchor.constraint(equalToConstant: 198).isActive = true
            it.heightAnchor.constraint(equalToConstant: 30).isActive = true
            h += 30
        }
        panel.frame = NSRect(x: 0, y: 0, width: 210, height: h)
        stack.frame = panel.bounds
        stack.autoresizingMask = [.width, .height]
        let r = b.convert(b.bounds, to: cv)
        var x = r.maxX - 210
        x = max(8, min(x, cv.bounds.width - 218))
        let below = cv.isFlipped ? r.maxY : cv.bounds.height - r.minY
        let top = max(8, min(below + 4, cv.bounds.height - h - 8))
        let oy = cv.isFlipped ? top : cv.bounds.height - top - h
        panel.frame.origin = NSPoint(x: x, y: oy)
        panel.setAccessibilityIdentifier("sftp-menu-\(side)")
        cv.addSubview(panel)
        menuPanel = panel
        menuMonitor = NSEvent.addLocalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown, .keyDown]) {
            [weak self] ev in
            guard let self, let p = self.menuPanel else { return ev }
            if ev.type == .keyDown {
                if ev.keyCode == 53 { self.closeActions(); return nil }
                return ev
            }
            let pt = p.convert(ev.locationInWindow, from: nil)
            if !p.bounds.contains(pt) { self.closeActions() }
            return ev
        }
    }
    func closeActions() {
        menuPanel?.removeFromSuperview()
        menuPanel = nil
        if let m = menuMonitor {
            NSEvent.removeMonitor(m)
            menuMonitor = nil
        }
    }
    @objc private func actClosePane() {
        closeActions()
        teardownConn()
        setState(.select)
    }
    @objc private func toggleFilter() {
        let on = filterRow.isHidden
        filterRow.isHidden = !on ? false : true
        if on { container.window?.makeFirstResponder(filterEntry) }
        else { filterEntry.stringValue = ""; filterChanged() }
    }
    @objc private func filterChanged() { applyShown() }

    // ---------- selection ----------
    func selItems() -> [FsEntry] {
        table.selectedRowIndexes.compactMap {
            $0 < shown.count ? shown[$0] : nil
        }
    }
    // ---------- DEBUG dev hooks ----------
    func devHidden(_ on: Bool) { showHidden = on; applyShown() }
    func devFilter(_ t: String) {
        filterRow.isHidden = false
        filterEntry.stringValue = t
        container.window?.makeFirstResponder(filterEntry)
        applyShown()
    }
    func devNav(_ op: String) {
        switch op {
        case "up": goUp()
        case "back": goBack()
        case "fwd": goFwd()
        default: break
        }
    }
    func devSelect(_ idxs: [Int]) {
        table.selectRowIndexes(
            IndexSet(idxs.filter { $0 >= 0 && $0 < shown.count }),
            byExtendingSelection: false)
    }
    func devActivate(_ i: Int) {
        guard i >= 0, i < shown.count else { return }
        let e = shown[i]
        if e.isDir { browseTo(e.path, nav: true) }
        else {
            v.xferEnqueue(sp: self, items: [e],
                          dp: v.panes[side ^ 1])
        }
    }
    /// dev：取消连接/关错误卡 → 回选择页（FR-19.1 cancel 语义）。
    func devCancel() { connClose() }

    @objc private func rowActivated() {
        let r = table.clickedRow
        guard r >= 0, r < shown.count else { return }
        let i = shown[r]
        if i.isDir { browseTo(i.path, nav: true) }
        else {
            v.xferEnqueue(sp: self, items: [i],
                          dp: v.panes[side ^ 1])
        }
    }

    // ---------- table dataSource/delegate ----------
    /// 行图标缓存：hbIcon 每次新建 NSImage+lockFocus 重绘，行渲染
    /// 下是热点；3 种图标全局复用（仅主线程访问）。
    private static var rowIcons: [String: NSImage] = [:]
    private static func rowIcon(dir: Bool, link: Bool) -> NSImage {
        let k = dir ? "d" : link ? "l" : "f"
        if let i = rowIcons[k] { return i }
        let i = hbIcon(dir ? "folder" : link ? "link" : "file",
                       size: 13,
                       tint: dir ? Theme.dyn("blue") : Theme.dyn("text"))
        rowIcons[k] = i
        return i
    }
    /// 文本列复用 cell：列 id 各自进池，重取后只更新文本/颜色/字体
    /// （原每格新建 NSTextField，debug 下 reload 重排是行渲染热点）。
    private func textCell(_ t: NSTableView,
                          _ id: NSUserInterfaceItemIdentifier,
                          _ s: String, dim: Bool,
                          mono: Bool = false) -> NSTextField {
        let l = (t.makeView(withIdentifier: id, owner: self)
                 as? NSTextField) ?? NSTextField(labelWithString: "")
        l.identifier = id
        l.stringValue = s
        l.hbText = dim ? "text2" : "text"
        l.font = mono ? .monospacedSystemFont(ofSize: 11.5,
                                              weight: .regular)
                      : .systemFont(ofSize: 12.5)
        return l
    }
    func numberOfRows(in t: NSTableView) -> Int { shown.count }
    func tableView(_ t: NSTableView, viewFor c: NSTableColumn?,
                   row: Int) -> NSView? {
        guard row < shown.count, let cid = c?.identifier
        else { return nil }
        let e = shown[row]
        switch cid.rawValue {
        case "name":
            let row_ = (t.makeView(withIdentifier: cid, owner: self)
                        as? NameCell) ?? NameCell()
            row_.identifier = cid
            row_.ic.image = Self.rowIcon(dir: e.isDir, link: e.isLink)
            row_.l.stringValue = e.name
            row_.toolTip = e.path
            return row_
        case "mtime":
            return textCell(t, cid, SftpKit.fmtMtime(e.mtime),
                            dim: true)
        case "size":
            return textCell(t, cid,
                            e.isDir ? "—" : SftpKit.fmtSize(e.size),
                            dim: false)
        case "kind":
            return textCell(t, cid,
                            e.isDir ? "folder" : e.isLink ? "link"
                                  : "file",
                            dim: true)
        case "mode":
            return textCell(t, cid, SftpKit.modeString(e.mode),
                            dim: true, mono: true)
        default: return nil
        }
    }
    func tableView(_ t: NSTableView, rowViewForRow row: Int)
        -> NSTableRowView? {
        let id = NSUserInterfaceItemIdentifier("sftp-row")
        let rv = (t.makeView(withIdentifier: id, owner: self)
                  as? SftpRowView) ?? SftpRowView()
        rv.identifier = id
        return rv
    }
    func tableView(_ t: NSTableView,
                   sortDescriptorsDidChange o: [NSSortDescriptor]) {
        applyShown()
    }

    // drag source：选中项名写入 "hbsftp|side\nnames"；本地 pane
    // 同时给 fileURL（可拖给 Finder）。
    func tableView(_ t: NSTableView, pasteboardWriterForRow row: Int)
        -> NSPasteboardWriting? {
        guard row < shown.count else { return nil }
        let e = shown[row]
        let item = NSPasteboardItem()
        if isLocal {
            item.setString(URL(fileURLWithPath: e.path).absoluteString,
                           forType: .fileURL)
        }
        let names = (table.selectedRowIndexes.contains(row)
                     ? selItems() : [e]).map { $0.name }
        item.setString("hbsftp|\(side)\n" + names.joined(separator: "\n"),
                       forType: .string)
        return item
    }
    // drop：对侧 pane 文本串 → 传输；fileURL → 从 Finder 上传。
    func tableView(_ t: NSTableView, validateDrop info: NSDraggingInfo,
                   proposedRow row: Int,
                   proposedDropOperation op: NSTableView.DropOperation)
        -> NSDragOperation {
        .copy
    }
    func tableView(_ t: NSTableView, acceptDrop info: NSDraggingInfo,
                   row: Int, dropOperation: NSTableView.DropOperation)
        -> Bool {
        v.acceptDrop(info, on: self)
    }

    // ---------- 操作 ----------
    @objc func actRefresh() { refresh() }
    @objc func actSelectAll() { table.selectAll(nil) }
    @objc func actHidden() { showHidden.toggle(); applyShown() }
    @objc func actCopy() {
        v.xferEnqueue(sp: self, items: selItems(),
                      dp: v.panes[side ^ 1])
    }
    @objc func actDelete() {
        let items = selItems()
        guard !items.isEmpty else { return }
        let ndir = items.filter { $0.isDir }.count
        if ndir > 0 {
            let a = NSAlert()
            a.messageText = L("删除项目")
            a.informativeText = String(format:
                L("将递归删除 %d 个项目（含 %d 个目录）。删除后 9 秒内可撤销。"),
                items.count, ndir)
            a.addButton(withTitle: L("删除"))
            a.addButton(withTitle: L("取消"))
            DialogRegistry.shared.present(alert: a, kind: "sftp-deldir",
                                          in: container.window) { ok in
                if ok { self.deleteItems(items) }
            }
            return
        }
        deleteItems(items)
    }
    private struct PendDel {
        var paths: [String]
        var items: [FsEntry]
        var gen: UInt
        var undone = false
        var timer: Timer?
    }
    private func deleteItems(_ its: [FsEntry]) {
        let paths = its.map { $0.path }
        items.removeAll { e in paths.contains(e.path) }
        applyShown()
        let pd = PendDel(paths: paths, items: its, gen: gen)
        pendDels.append(pd)
        v.toastAction(
            String(format: L("已删除 %d 个项目"), its.count),
            action: L("撤销"), ttl: 9,
            onAction: { [weak self] in
                guard let self else { return }
                if let i = self.pendDels.firstIndex(where: {
                    $0.paths == pd.paths }) {
                    self.pendDels[i].undone = true
                    self.pendDels[i].paths.removeAll()
                    self.items.append(contentsOf: pd.items)
                    self.applyShown()
                }
            },
            onDismiss: { [weak self] in
                guard let self else { return }
                guard let i = self.pendDels.firstIndex(where: {
                    $0.paths == pd.paths && !$0.undone }) else { return }
                let d = self.pendDels.remove(at: i)
                if self.gen == d.gen {
                    for p in d.paths {
                        self.fs?.remove(p, recursive: true) { [weak self] r in
                            self?.opRefresh(r)
                        }
                    }
                }
            })
    }
    func opRefresh(_ r: FsRes) {
        if !r.ok {
            v.toast(L("操作失败") + ": " + (r.message ?? ""))
        }
        refresh()
    }
    @objc func actMkdir() {
        promptSheet(title: L("新建文件夹"), field: L("名称"),
                    initial: "", accId: "sftp-mkdir") { [weak self] name in
            guard let self, let name, !name.isEmpty else { return }
            let full = SftpKit.pathJoin(self.cwd ?? "/", name)
            self.fs?.mkdir(full, mode: 0o755) { [weak self] r in
                self?.opRefresh(r)
            }
        }
    }
    @objc func actRename() {
        let items = selItems()
        guard let i = items.first else { return }
        promptSheet(title: L("重命名"), field: L("新文件名"),
                    initial: i.name, accId: "sftp-rename") { [weak self] name in
            guard let self, let name, !name.isEmpty else { return }
            let to = SftpKit.pathJoin(SftpKit.parentDir(i.path), name)
            self.fs?.rename(i.path, to, overwrite: false) { [weak self] r in
                self?.opRefresh(r)
            }
        }
    }
    @objc func actChmod() {
        let items = selItems()
        guard let first = items.first else { return }
        v.chmodSheet(paths: items.map { $0.path },
                     initial: first.mode & 0o777, pane: self)
    }
    @objc func actOpen() {
        let items = selItems()
        guard let i = items.first else { return }
        if isLocal {
            NSWorkspace.shared.open(URL(fileURLWithPath: i.path))
            return
        }
        // 远端：下载到 Caches/harbor/open/<rand>/ → 打开 → 监视改动
        guard let sess else { return }
        let base = v.openCacheDir()
        let dir = base + "/" + UUID().uuidString.prefix(8)
        do {
            try FileManager.default.createDirectory(
                atPath: String(dir), withIntermediateDirectories: true)
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o700], ofItemAtPath: String(dir))
        } catch {
            v.toast(L("无法创建缓存目录")); return
        }
        let oc = OpenCtx(pane: self, name: i.name, remotePath: i.path,
                         localPath: String(dir) + "/" + i.name,
                         dir: String(dir))
        let req = hb_sftp_download(sess.sess, i.path, oc.localPath,
                                   HB_SFTP_OVERWRITE)
        oc.req = req
        openReqs[req] = oc
    }
    private func openDone(oc: OpenCtx, r: hb_sftp_result) {
        if r.kind == HB_SFTP_R_PROGRESS { return }
        openReqs.removeValue(forKey: r.req)
        guard r.status == HB_SFTP_OK else {
            v.toast(L("下载失败") + ": " + (r.message.map {
                String(cString: $0) } ?? ""))
            oc.free(); return
        }
        openCtxs.append(oc)
        let url = URL(fileURLWithPath: oc.localPath)
        NSWorkspace.shared.open(url)
        oc.watch { [weak self] in
            guard let self else { return }
            self.v.toastAction(
                String(format: L("已修改 %@ · 上传回远端"), oc.name),
                action: L("上传"), ttl: 9,
                onAction: { [weak self] in
                    guard let s = self?.sess else { return }
                    _ = hb_sftp_upload(s.sess, oc.localPath,
                                       oc.remotePath, HB_SFTP_OVERWRITE)
                }, onDismiss: nil)
        }
    }

    // ---------- dialogs ----------
    /// hb_dialog_prompt 等价：标题 + 单行输入 + 取消/确定。
    private func promptSheet(title: String, field: String,
                             initial: String, accId: String,
                             done: @escaping (String?) -> Void) {
        let a = NSAlert()
        a.messageText = title
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 22))
        f.stringValue = initial
        f.placeholderString = field
        f.setAccessibilityIdentifier(accId)
        a.accessoryView = f
        a.addButton(withTitle: L("确定"))
        a.addButton(withTitle: L("取消"))
        DialogRegistry.shared.present(alert: a, kind: accId,
                                      in: container.window) { ok in
            done(ok ? f.stringValue : nil)
        }
        container.window?.makeFirstResponder(f)
    }

    // ---------- key / misc ----------
    func keyDown(_ ev: NSEvent) -> Bool {
        let foc = container.window?.firstResponder
        if let tf = foc as? NSTextView, tf === filterEntry.currentEditor() {
            if ev.keyCode == 96 { refresh(); return true }
            return false
        }
        switch ev.keyCode {
        case 96: refresh(); return true                    // F5
        case 120: actRename(); return true                 // F2
        case 51, 117: actDelete(); return true             // ⌫/⌦
        case 126:
            if ev.modifierFlags.contains(.option) { goUp(); return true }
            return false
        case 0:
            if ev.modifierFlags.contains(.command) {
                actSelectAll(); return true
            }
            return false
        default: return false
        }
    }

    private var isFilterFocused: Bool {
        guard let ed = filterEntry.currentEditor() else { return false }
        return (container.window?.firstResponder as? NSTextView) === ed
    }

    /// dev state（DEBUG hook 用）
    var devState: String {
        var sel: [String] = []
        table.selectedRowIndexes.forEach {
            if $0 < shown.count { sel.append(shown[$0].name) }
        }
        return "state=\(state) local=\(isLocal) cwd=\(cwd ?? "-") "
             + "items=\(items.count) shown=\(shown.count) "
             + "hidden=\(showHidden) sel=\(sel.joined(separator: ",")) "
             + "filter='\(filterEntry.stringValue)'"
             + (isFilterFocused ? " foc=filter" : "")
             + (hadError ? " errcard" : "")
    }
}

extension SftpKit.SortKey {
    init(raw: String) {
        switch raw {
        case "mtime": self = .mtime
        case "size": self = .size
        case "kind": self = .kind
        case "mode": self = .mode
        default: self = .name
        }
    }
}

/// prototype .menu button：左对齐 13px，radius 7 hover，red/disabled 变体。
private final class MenuItem: NSButton {
    var fire: (() -> Void)?
    private var hovered = false
    private let enabled_: Bool
    init(_ title: String, red: Bool, enabled: Bool) {
        enabled_ = enabled
        super.init(frame: .zero)
        self.title = ""
        isBordered = false
        isEnabled = enabled
        wantsLayer = true
        layer?.cornerRadius = 7
        let l = NSTextField(labelWithString: title)
        l.font = .systemFont(ofSize: 13)
        l.hbText = enabled ? (red ? "red" : "text") : "text3"
        l.lineBreakMode = .byTruncatingTail
        l.translatesAutoresizingMaskIntoConstraints = false
        addSubview(l)
        NSLayoutConstraint.activate([
            l.leadingAnchor.constraint(equalTo: leadingAnchor,
                                       constant: 12),
            l.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor,
                                        constant: -12),
            l.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
        target = self
        action = #selector(itemFire)
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        restyle()
    }
    required init?(coder: NSCoder) { fatalError() }
    @objc private func itemFire() { fire?() }
    private func restyle() {
        layer?.hbBgResolver = { [weak self] in
            guard let self, self.hovered, self.enabled_ else {
                return Theme.RGBA(0, 0).color.cgColor
            }
            return (Theme.isDark ? Theme.hover : Theme.RGBA(0xeef2f6))
                .color.cgColor
        }
    }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; restyle()
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; restyle()
    }
}

/// prototype .select-host-item：radius 10，hover 浅灰底。
private final class HostItem: NSButton {
    private var hovered = false
    init() {
        super.init(frame: .zero)
        title = ""
        isBordered = false
        wantsLayer = true
        layer?.cornerRadius = 10
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        restyle()
    }
    required init?(coder: NSCoder) { fatalError() }
    private func restyle() {
        layer?.hbBgResolver = { [weak self] in
            guard let self, self.hovered else {
                return Theme.RGBA(0, 0).color.cgColor
            }
            return (Theme.isDark ? Theme.hover : Theme.RGBA(0xe9eef4))
                .color.cgColor
        }
    }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; restyle()
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; restyle()
    }
}

/// prototype .navbtn：无底glyph按钮，hover 灰底，禁用半透明。
private final class NavGlyph: NSButton {
    private var hovered = false
    init(_ glyph: String) {
        super.init(frame: .zero)
        title = glyph
        isBordered = false
        font = .systemFont(ofSize: 15)
        contentTintColor = Theme.dyn("text2")
        wantsLayer = true
        layer?.cornerRadius = 6
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        restyle()
    }
    required init?(coder: NSCoder) { fatalError() }
    override var isEnabled: Bool {
        didSet { alphaValue = isEnabled ? 1 : 0.45 }
    }
    private func restyle() {
        layer?.hbBgResolver = { [weak self] in
            (self?.hovered == true ? Theme.btn : Theme.RGBA(0, 0))
                .color.cgColor
        }
    }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; restyle()
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; restyle()
    }
}

/// prototype .crumb：灰底圆角chip + folder 图标，hover 加深。
private final class CrumbChip: NSButton {
    private var hovered = false
    init(_ label: String) {
        super.init(frame: .zero)
        isBordered = false
        wantsLayer = true
        layer?.cornerRadius = 6
        let s = NSMutableAttributedString()
        let att = NSTextAttachment()
        att.image = hbIcon("folder", size: 12, tint: Theme.dyn("text2"))
        let m = NSMutableAttributedString(attachment: att)
        m.addAttribute(.baselineOffset, value: -2,
                       range: NSRange(location: 0, length: m.length))
        s.append(m)
        s.append(NSAttributedString(string: " " + label))
        s.addAttributes([.font: NSFont.systemFont(ofSize: 12.5),
                         .foregroundColor: Theme.dyn("text2")],
                        range: NSRange(location: 0, length: s.length))
        attributedTitle = s
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
        restyle()
    }
    required init?(coder: NSCoder) { fatalError() }
    private func restyle() {
        layer?.hbBgResolver = { [weak self] in
            guard let self else { return nil }
            if Theme.isDark {
                return (self.hovered ? Theme.btnHover : Theme.chip)
                    .color.cgColor
            }
            return Theme.RGBA(self.hovered ? 0xdde3ea : 0xe7ecf1)
                .color.cgColor
        }
    }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; restyle()
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; restyle()
    }
}

/// prototype .filetable tr：hover / selected 底色 + 行尾细分隔线。
private final class SftpRowView: NSTableRowView {
    private var hovered = false
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        addTrackingArea(NSTrackingArea(
            rect: .zero, options: [.mouseEnteredAndExited,
                                   .activeAlways, .inVisibleRect],
            owner: self, userInfo: nil))
    }
    required init?(coder: NSCoder) { fatalError() }
    override func prepareForReuse() {
        super.prepareForReuse()
        hovered = false
        needsDisplay = true
    }
    override func mouseEntered(with e: NSEvent) {
        hovered = true; needsDisplay = true
    }
    override func mouseExited(with e: NSEvent) {
        hovered = false; needsDisplay = true
    }
    override var isSelected: Bool { didSet { needsDisplay = true } }
    override func drawBackground(in dirtyRect: NSRect) {
        let c: Theme.RGBA?
        if isSelected {
            c = Theme.isDark ? Theme.sel : Theme.RGBA(0xddebfc)
        } else if hovered {
            c = Theme.isDark ? Theme.rowHover : Theme.RGBA(0xeef4fb)
        } else {
            c = nil
        }
        if let c { c.color.setFill(); dirtyRect.fill() }
        (Theme.isDark ? Theme.rowLine : Theme.RGBA(0xf0f3f6))
            .color.setFill()
        NSRect(x: 0, y: bounds.maxY - 1, width: bounds.width, height: 1)
            .fill()
    }
    override func drawSelection(in dirtyRect: NSRect) {}
}

/// prototype .filetable th：浅灰底 + 上下描边，12px semibold text2。
private final class SftpHeaderView: NSTableHeaderView {
    override var isFlipped: Bool { true }
    override var intrinsicContentSize: NSSize {
        NSSize(width: NSView.noIntrinsicMetric, height: 32)
    }
    override func draw(_ dirtyRect: NSRect) {
        (Theme.isDark ? Theme.tableHead : Theme.RGBA(0xf6f8fa))
            .color.setFill()
        bounds.fill()
        Theme.line.color.setFill()
        NSRect(x: 0, y: 0, width: bounds.width, height: 1).fill()
        NSRect(x: 0, y: bounds.maxY - 1, width: bounds.width, height: 1)
            .fill()
        guard let tv = tableView else { return }
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 12, weight: .semibold),
            .foregroundColor: Theme.dyn("text2"),
        ]
        for (i, col) in tv.tableColumns.enumerated() {
            let r = headerRect(ofColumn: i)
            guard r.intersects(dirtyRect) else { continue }
            let s = NSAttributedString(string: col.title, attributes: attrs)
            let sz = s.size()
            s.draw(at: NSPoint(x: r.minX + 14, y: r.midY - sz.height / 2))
            if tv.highlightedTableColumn === col {
                let asc = tv.sortDescriptors.first?.ascending ?? true
                let g = NSAttributedString(
                    string: asc ? " ▴" : " ▾",
                    attributes: [.font: NSFont.systemFont(ofSize: 9),
                                 .foregroundColor: Theme.dyn("text3")])
                g.draw(at: NSPoint(x: r.minX + 14 + sz.width + 2,
                                   y: r.midY - g.size().height / 2))
            }
        }
    }
}

/// name 列复用 cell（stack：图标 + 截断标签；makeView 池化）。
private final class NameCell: NSStackView {
    let ic = NSImageView()
    let l = NSTextField(labelWithString: "")
    init() {
        super.init(frame: .zero)
        orientation = .horizontal
        spacing = 8
        l.font = .systemFont(ofSize: 12.5)
        l.hbText = "text"
        l.lineBreakMode = .byTruncatingMiddle
        addArrangedSubview(ic)
        addArrangedSubview(l)
    }
    required init?(coder: NSCoder) { fatalError() }
}

/// 行快捷键：F5 刷新 / F2 重命名 / ⌫ 删除 / ⌥↑ 上级 / ⌘A 全选。
final class SftpTable: NSTableView {
    weak var pane: SftpPane?
    override func keyDown(with event: NSEvent) {
        if pane?.keyDown(event) == true { return }
        super.keyDown(with: event)
    }
}

/// “打开”下载 → 打开后监视文件改动的上下文（linux OpenCtx）。
final class OpenCtx {
    unowned let pane: SftpPane
    let name, remotePath, localPath, dir: String
    var req: UInt32 = 0
    private var src: DispatchSourceFileSystemObject?
    private var fd: Int32 = -1
    init(pane: SftpPane, name: String, remotePath: String,
         localPath: String, dir: String) {
        self.pane = pane; self.name = name; self.remotePath = remotePath
        self.localPath = localPath; self.dir = dir
    }
    /// 文件被改写 → cb（一次；之后继续监视）。
    func watch(_ cb: @escaping () -> Void) {
        fd = open(localPath, O_EVTONLY)
        guard fd >= 0 else { return }
        let s = DispatchSource.makeFileSystemObjectSource(
            fileDescriptor: fd, eventMask: [.write, .rename, .delete],
            queue: .main)
        s.setEventHandler { cb() }
        s.setCancelHandler { close(self.fd) }
        s.resume()
        src = s
    }
    func free() {
        src?.cancel(); src = nil
        if fd >= 0 { close(fd); fd = -1 }
    }
    deinit { free() }
}

// ============================ SftpView ============================

/// 双栏 + 传输队列 dock（对齐 linux HbSftpView）。
final class SftpView: NSView {
    unowned var tab: SFTPTab?
    var panes: [SftpPane] = []
    private(set) var xers: [Xfer] = []
    private var nextXid = 0
    private(set) var conc = 3
    /// 主机编辑器钩子（conn 失败卡 “编辑主机”）——由 tab/host 页注入。
    var hostEditor: ((String) -> Void)?

    private var xlist: NSStackView!
    private var xhead: NSTextField!
    private var xclear: NSButton!
    private var xwrap: NSView!
    private var xwrapZeroH: NSLayoutConstraint!
    private var toastBox: NSView!

    override init(frame: NSRect) {
        super.init(frame: frame)
        conc = Int(Vault.shared.settingsInt("sftp_concurrency", 3))
        conc = min(8, max(1, conc))
        wantsLayer = true
        layer?.hbBg = "page"

        let cols = NSStackView()
        cols.orientation = .horizontal
        cols.spacing = 0
        cols.distribution = .fillEqually
        cols.translatesAutoresizingMaskIntoConstraints = false
        addSubview(cols)
        for k in 0..<2 {
            let p = SftpPane(v: self, side: k)
            panes.append(p)
            let wrap = NSView()
            wrap.translatesAutoresizingMaskIntoConstraints = false
            if k == 0 {
                let sep = NSView()
                sep.wantsLayer = true
                sep.layer?.hbBg = "line"
                sep.translatesAutoresizingMaskIntoConstraints = false
                wrap.addSubview(sep)
                NSLayoutConstraint.activate([
                    sep.trailingAnchor.constraint(
                        equalTo: wrap.trailingAnchor),
                    sep.topAnchor.constraint(equalTo: wrap.topAnchor),
                    sep.bottomAnchor.constraint(
                        equalTo: wrap.bottomAnchor),
                    sep.widthAnchor.constraint(equalToConstant: 1),
                ])
            }
            p.container.translatesAutoresizingMaskIntoConstraints = false
            wrap.addSubview(p.container)
            NSLayoutConstraint.activate([
                p.container.leadingAnchor.constraint(
                    equalTo: wrap.leadingAnchor),
                p.container.trailingAnchor.constraint(
                    equalTo: wrap.trailingAnchor),
                p.container.topAnchor.constraint(equalTo: wrap.topAnchor),
                p.container.bottomAnchor.constraint(
                    equalTo: wrap.bottomAnchor),
            ])
            cols.addArrangedSubview(wrap)
        }
        // 队列 dock（常驻）
        xwrap = NSView()
        xwrap.translatesAutoresizingMaskIntoConstraints = false
        xwrap.wantsLayer = true
        xwrap.layer?.hbBg = "card"
        xwrap.layer?.borderWidth = 1
        xwrap.layer?.hbBorder = "line"
        addSubview(xwrap)
        let h = NSStackView()
        h.orientation = .horizontal
        h.spacing = 8
        h.edgeInsets = NSEdgeInsets(top: 4, left: 10,
                                    bottom: 4, right: 10)
        h.translatesAutoresizingMaskIntoConstraints = false
        let ic = NSImageView(image: hbIcon("fwd", size: 12,
                                           tint: Theme.dyn("text")))
        h.addArrangedSubview(ic)
        xhead = NSTextField(labelWithString: L("传输队列 · 0 进行中"))
        xhead.font = .systemFont(ofSize: 11.5)
        h.addArrangedSubview(xhead)
        let sp = NSView()
        h.addArrangedSubview(sp)
        xclear = NSButton(title: L("清空已完成"), target: self,
                          action: #selector(clearDone))
        xclear.bezelStyle = .rounded
        xclear.font = .systemFont(ofSize: 11.5)
        xclear.isHidden = true
        xclear.setAccessibilityIdentifier("xers-clear")
        h.addArrangedSubview(xclear)
        xwrap.addSubview(h)
        let scr = NSScrollView()
        scr.translatesAutoresizingMaskIntoConstraints = false
        scr.hasVerticalScroller = true
        xlist = FlippedStack()
        xlist.orientation = .vertical
        xlist.spacing = 2
        xlist.edgeInsets = NSEdgeInsets(top: 0, left: 10,
                                        bottom: 6, right: 10)
        xlist.translatesAutoresizingMaskIntoConstraints = false
        scr.documentView = xlist
        xwrap.addSubview(scr)
        let scrHug = scr.heightAnchor.constraint(equalTo: xlist.heightAnchor)
        scrHug.priority = .defaultLow
        NSLayoutConstraint.activate([
            cols.leadingAnchor.constraint(equalTo: leadingAnchor),
            cols.trailingAnchor.constraint(equalTo: trailingAnchor),
            cols.topAnchor.constraint(equalTo: topAnchor),
            cols.bottomAnchor.constraint(equalTo: xwrap.topAnchor),
            xwrap.leadingAnchor.constraint(equalTo: leadingAnchor),
            xwrap.trailingAnchor.constraint(equalTo: trailingAnchor),
            xwrap.bottomAnchor.constraint(equalTo: bottomAnchor),
            xwrap.heightAnchor.constraint(lessThanOrEqualToConstant: 160),
            h.leadingAnchor.constraint(equalTo: xwrap.leadingAnchor),
            h.trailingAnchor.constraint(equalTo: xwrap.trailingAnchor),
            h.topAnchor.constraint(equalTo: xwrap.topAnchor),
            h.heightAnchor.constraint(equalToConstant: 26),
            scr.topAnchor.constraint(equalTo: h.bottomAnchor),
            scr.leadingAnchor.constraint(equalTo: xwrap.leadingAnchor),
            scr.trailingAnchor.constraint(equalTo: xwrap.trailingAnchor),
            scr.bottomAnchor.constraint(equalTo: xwrap.bottomAnchor),
            scr.heightAnchor.constraint(lessThanOrEqualToConstant: 130),
            /* xwrap 仅有 ≤160 上限——无驱动则 scr 塌成 0、队列入不可见。
             * 低优先级跟随内容高度：短内容贴高，长内容被 ≤130 截断出滚动条。 */
            xlist.leadingAnchor.constraint(equalTo: scr.leadingAnchor),
            xlist.trailingAnchor.constraint(equalTo: scr.trailingAnchor),
            xlist.topAnchor.constraint(equalTo: scr.topAnchor),
            xlist.widthAnchor.constraint(equalTo: scr.widthAnchor),
            scrHug,
            sp.widthAnchor.constraint(greaterThanOrEqualToConstant: 0),
        ])
        cleanOpenCache()
        xwrapZeroH = xwrap.heightAnchor.constraint(equalToConstant: 0)
        xwrapZeroH.priority = .required
        xheadSync()
    }
    required init?(coder: NSCoder) { nil }

    // ---------------- toast ----------------
    func toast(_ text: String) {
        let t = SftpToast(text: text)
        showToast(t)
    }
    func toastAction(_ text: String, action: String, ttl: TimeInterval,
                     onAction: @escaping () -> Void,
                     onDismiss: (() -> Void)?) {
        let t = SftpToast(text: text, action: action, ttl: ttl,
                          onAction: onAction, onDismiss: onDismiss)
        showToast(t)
    }
    private func showToast(_ t: SftpToast) {
        t.translatesAutoresizingMaskIntoConstraints = false
        addSubview(t)
        NSLayoutConstraint.activate([
            t.centerXAnchor.constraint(equalTo: centerXAnchor),
            t.bottomAnchor.constraint(equalTo: xwrap.topAnchor,
                                      constant: -10),
        ])
    }

    // ---------------- open 缓存 ----------------
    func openCacheDir() -> String {
        let base = FileManager.default.urls(for: .cachesDirectory,
                        in: .userDomainMask)[0]
            .appendingPathComponent("harbor/open").path
        try? FileManager.default.createDirectory(
            atPath: base, withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700])
        return base
    }
    /// linux clean_open_cache：启动/释放时清 open 缓存。
    func cleanOpenCache() {
        let base = openCacheDir()
        if let subs = try? FileManager.default
            .contentsOfDirectory(atPath: base) {
            for s in subs {
                _ = LocalFs.rmrf(SftpKit.pathJoin(base, s))
            }
        }
    }

    // ---------------- drop（外源 → 本 pane） ----------------
    @discardableResult
    func acceptDrop(_ info: NSDraggingInfo, on dp: SftpPane) -> Bool {
        guard dp.state == .browse, dp.fs != nil else {
            toast(L("请先在对侧栏选择主机")); return false
        }
        let pb = info.draggingPasteboard
        // Finder fileURL → 源按 local 处理（同 linux on_drop）
        if let urls = pb.readObjects(forClasses: [NSURL.self],
            options: [.urlReadingFileURLsOnly: true]) as? [URL],
           !urls.isEmpty {
            let entries = urls.map { u -> FsEntry in
                var st = stat()
                let p = u.path
                let isD = lstat(p, &st) == 0 &&
                          (st.st_mode & S_IFMT) == S_IFDIR
                return FsEntry(name: u.lastPathComponent, path: p,
                               size: 0, mtime: 0, mode: 0,
                               isDir: isD, isLink: false)
            }
            let sp = panes.first { $0.isLocal && $0.state == .browse }
            xferEnqueue(sp: sp, items: entries, dp: dp)
            return true
        }
        // 内联文本 "hbsftp|side\nnames" → 对侧传输
        if let s = pb.string(forType: .string),
           s.hasPrefix("hbsftp|") {
            let head = s.dropFirst(7).prefix(while: { $0 != "\n" })
            guard let sside = Int(head), sside >= 0, sside <= 1 else {
                return false
            }
            let sp = panes[sside]
            let names = s.split(separator: "\n").dropFirst()
                .map(String.init)
            let entries = names.map {
                FsEntry(name: $0,
                        path: SftpKit.pathJoin(sp.cwd ?? "/", $0),
                        size: 0, mtime: 0, mode: 0,
                        isDir: false, isLink: false)
            }
            xferEnqueue(sp: sp, items: entries, dp: dp)
            return true
        }
        return false
    }

    // ============================ 传输队列 ============================

    /// 目标 stat → 冲突问对话框（policy_all 复用）→ 入队。
    /// 对齐 linux ConfCtx/stat_cb/conf_ask 流程。
    private struct ConfCtx {
        var todo: [FsEntry]
        var policyAll: Int32   // -1 未选
        var sp: SftpPane?
        var dp: SftpPane
    }
    private var confQueue: [ConfCtx] = []
    private var activeConf: ConfCtx?

    func xferEnqueue(sp: SftpPane?, items: [FsEntry], dp: SftpPane) {
        guard !items.isEmpty else { return }
        guard dp.state == .browse, dp.fs != nil else {
            toast(L("请先在对侧栏选择主机")); return
        }
        confQueue.append(ConfCtx(todo: items, policyAll: -1,
                                 sp: sp, dp: dp))
        confNext()
    }
    private func confNext() {
        if activeConf == nil {
            guard !confQueue.isEmpty else { return }
            activeConf = confQueue.removeFirst()
        }
        guard var cc = activeConf else { return }
        guard let it = cc.todo.first else {
            activeConf = nil
            confNext()
            return
        }
        cc.todo.removeFirst()
        activeConf = cc
        let dst = SftpKit.pathJoin(cc.dp.cwd ?? "/", it.name)
        cc.dp.fs?.stat(dst) { [weak self] r in
            guard let self, let cc = self.activeConf else { return }
            if r.ok && !r.entries.isEmpty {
                if cc.policyAll >= 0 {
                    self.enqOne(it, cc: cc, policy: cc.policyAll)
                    self.confNext()
                } else {
                    self.confAsk(it)
                }
            } else {
                self.enqOne(it, cc: cc, policy: HB_SFTP_OVERWRITE_I)
                self.confNext()
            }
        }
    }
    /// linux conf_ask：覆盖/跳过/重命名/取消 + “对本次其余项目都适用”。
    /// DialogRegistry.present 只回 Bool，4 分支用 track + raw response。
    private func confAsk(_ it: FsEntry) {
        confCurrent = it
        let a = NSAlert()
        a.messageText = L("目标已存在")
        a.informativeText = it.isDir
            ? String(format:
                L("「%@」已存在 — 将合并目录，按所选方式处理同名文件"),
                it.name)
            : String(format:
                L("「%@」在对侧目录已存在。选择处理方式："), it.name)
        let all = NSButton(checkboxWithTitle: L("对本次其余项目都适用"),
                           target: nil, action: nil)
        all.setAccessibilityIdentifier("conf-applyall")
        a.accessoryView = all
        a.addButton(withTitle: L("覆盖"))       // first = default
        a.addButton(withTitle: L("跳过"))
        a.addButton(withTitle: L("重命名"))
        a.addButton(withTitle: L("取消传输"))
        presentConflict4(a, applyAll: all)
    }
    /// 4 选项冲突 sheet（保留 dev 可观测 kind）。
    private func presentConflict4(_ a: NSAlert,
                                  applyAll all: NSButton) {
        guard let w = window else {
            _ = a.runModal(); return
        }
        a.beginSheetModal(for: w) { [weak self] resp in
            DialogRegistry.shared.untrack(alert: a)
            guard let self, var cc = self.activeConf else { return }
            let pol: Int32?
            switch resp {
            case .alertFirstButtonReturn:
                pol = Int32(HB_SFTP_OVERWRITE.rawValue)
            case .alertSecondButtonReturn:
                pol = Int32(HB_SFTP_SKIP.rawValue)
            case .alertThirdButtonReturn:
                pol = Int32(HB_SFTP_RENAME.rawValue)
            default: pol = nil
            }
            guard let p = pol else {
                // 取消传输：丢弃整个批次（linux conf_decide 同义）
                self.activeConf = nil
                self.confQueue.removeAll()
                return
            }
            if all.state == .on { cc.policyAll = p; self.activeConf = cc }
            guard let it = self.confCurrent else { return }
            self.confCurrent = nil
            self.enqOne(it, cc: cc, policy: p)
            self.confNext()
        }
        DialogRegistry.shared.track(alert: a, kind: "sftp-conflict")
    }
    /// 当前正在问的 item（confAsk → presentConflict4 链路传参）。
    private var confCurrent: FsEntry?

    private func enqOne(_ it: FsEntry, cc: ConfCtx,
                        policy: Int32) {
        let x = Xfer()
        nextXid += 1
        x.id = nextXid
        x.state = .run
        x.t0 = Date()
        x.sp = cc.sp; x.dp = cc.dp
        x.name = it.name
        x.srcPath = it.path
        x.dstDir = cc.dp.cwd ?? "/"
        x.srcLabel = (cc.sp == nil || cc.sp!.isLocal) ? L("Local") : cc.sp!.hname
        x.dstLabel = cc.dp.isLocal ? L("Local") : cc.dp.hname
        x.policy = policy
        if cc.sp != nil && !cc.sp!.isLocal && !cc.dp.isLocal {
            x.kind = .relay
        } else if (cc.sp == nil || cc.sp!.isLocal) && !cc.dp.isLocal {
            x.kind = .up
        } else if cc.sp != nil && !cc.sp!.isLocal && cc.dp.isLocal {
            x.kind = .down
        } else {
            x.kind = .local
        }
        buildXferRow(x)
        xers.append(x)
        xheadSync()
        updateRow(x)
        switch x.kind {
        case .up:
            guard let ds = x.dp?.sess else {
                finishXfer(x, .fail, L("连接已断开")); return
            }
            let t = SftpKit.pathJoin(x.dstDir, x.name)
            x.req = hb_sftp_upload(ds.sess, x.srcPath, t,
                                   hb_sftp_conflict(x.policy))
            x.sess = ds
        case .down:
            guard let ss = x.sp?.sess else {
                finishXfer(x, .fail, L("连接已断开")); return
            }
            let t = SftpKit.pathJoin(x.dstDir, x.name)
            x.req = hb_sftp_download(ss.sess, x.srcPath, t,
                                     hb_sftp_conflict(x.policy))
            x.sess = ss
        case .relay:
            guard let ss = x.sp?.sess, x.dp?.sess != nil else {
                finishXfer(x, .fail, L("连接已断开")); return
            }
            let d = NSTemporaryDirectory() + "harbor-relay-\(x.id)"
            do {
                try FileManager.default.createDirectory(
                    atPath: d, withIntermediateDirectories: true,
                    attributes: [.posixPermissions: 0o700])
            } catch {
                finishXfer(x, .fail, L("无法创建临时目录")); return
            }
            x.tmpDir = d
            x.tmpPath = SftpKit.pathJoin(d, x.name)
            x.phase = 0
            x.req = hb_sftp_download(ss.sess, x.srcPath,
                                     x.tmpPath!, HB_SFTP_OVERWRITE)
            x.sess = ss
        case .local:
            startLocal(x)
        }
    }

    // ---------------- row UI ----------------
    private func buildXferRow(_ x: Xfer) {
        let row = NSStackView()
        row.orientation = .horizontal
        row.spacing = 8
        row.edgeInsets = NSEdgeInsets(top: 3, left: 0,
                                      bottom: 3, right: 0)
        let ic = NSImageView(image: hbIcon(
            x.kind == .up ? "up" : x.kind == .down ? "down" : "fwd",
            size: 12, tint: Theme.dyn("text")))
        row.addArrangedSubview(ic)
        let nm = NSTextField(labelWithString: x.name)
        nm.lineBreakMode = .byTruncatingMiddle
        nm.font = .systemFont(ofSize: 11.5)
        nm.translatesAutoresizingMaskIntoConstraints = false
        nm.widthAnchor.constraint(equalToConstant: 130).isActive = true
        row.addArrangedSubview(nm)
        let dl = NSTextField(labelWithString:
                             "\(x.srcLabel) → \(x.dstLabel)")
        dl.font = .systemFont(ofSize: 11.5)
        dl.textColor = .secondaryLabelColor
        dl.lineBreakMode = .byTruncatingTail
        dl.translatesAutoresizingMaskIntoConstraints = false
        dl.widthAnchor.constraint(equalToConstant: 130).isActive = true
        row.addArrangedSubview(dl)
        let track = NSView()
        track.wantsLayer = true
        track.layer?.hbBg = "line"
        track.layer?.cornerRadius = 2
        track.translatesAutoresizingMaskIntoConstraints = false
        let fill = NSView()
        fill.wantsLayer = true
        fill.layer?.hbBg = "blue"
        fill.layer?.cornerRadius = 2
        fill.translatesAutoresizingMaskIntoConstraints = false
        track.addSubview(fill)
        let fillW = fill.widthAnchor.constraint(equalToConstant: 0)
        NSLayoutConstraint.activate([
            track.heightAnchor.constraint(equalToConstant: 4),
            fill.leadingAnchor.constraint(equalTo: track.leadingAnchor),
            fill.topAnchor.constraint(equalTo: track.topAnchor),
            fill.bottomAnchor.constraint(equalTo: track.bottomAnchor),
            fillW,
        ])
        x.fill = fill
        x.fillW = fillW
        row.addArrangedSubview(track)
        let pct = NSTextField(labelWithString: "0%")
        pct.font = .systemFont(ofSize: 11.5)
        pct.translatesAutoresizingMaskIntoConstraints = false
        pct.widthAnchor.constraint(equalToConstant: 160).isActive = true
        row.addArrangedSubview(pct)
        x.pct = pct
        let xb = NSButton(title: "×", target: self,
                          action: #selector(xferBtn(_:)))
        xb.bezelStyle = .rounded
        xb.font = .systemFont(ofSize: 12)
        xb.translatesAutoresizingMaskIntoConstraints = false
        xb.tag = x.id
        xb.setAccessibilityIdentifier("xfer-btn")
        row.addArrangedSubview(xb)
        x.xbtn = xb
        x.row = row
        xlist.addArrangedSubview(row)
    }

    private func xferByReq(_ req: UInt32, _ s: Session?) -> Xfer? {
        xers.first { $0.state == .run && $0.req == req &&
                   (s == nil || $0.sess === s) }
    }

    func updateRow(_ x: Xfer) {
        var pct = x.total > 0 ? Double(x.done) / Double(x.total) * 100
                            : (x.done > 0 ? 100 : 0)
        if x.state == .done { pct = 100 }
        pct = min(100, pct)
        x.fillW?.constant = pct * 2.2     // track 220px
        switch x.state {
        case .run:
            var t = "\(Int(pct))%"
            if x.phase == 1 { t += " · " + L("上传") }
            else if x.kind == .relay { t += " · " + L("下载") }
            if x.total > 0 && x.ema > 0 && x.done < x.total {
                let s = Int((Double(x.total - x.done)) / x.ema)
                t += " · ~\(max(1, s))s"
            }
            t += " · " + SftpKit.fmtSpeed(x.ema)
            x.pct?.stringValue = t
        case .done:
            let el = Date().timeIntervalSince(x.t0)
            if el > 0.05 && x.acc > 0 {
                x.pct?.stringValue = L("已完成") + " · "
                    + SftpKit.fmtSpeed(Double(x.acc) / el)
            } else {
                x.pct?.stringValue = L("已完成")
            }
            if x.skipped { x.pct?.stringValue = L("已跳过") }
            x.fill?.layer?.hbBg = "green"
        case .fail:
            x.pct?.stringValue = L("失败") + ": "
                + (x.err ?? L("未知错误"))
            x.fill?.layer?.hbBg = "red"
        case .cancel:
            x.pct?.stringValue = L("已取消")
            x.fill?.layer?.hbBg = "line"
        }
    }

    private func xheadSync() {
        var act = 0, term = 0
        for x in xers { x.state == .run ? (act += 1) : (term += 1) }
        xhead.stringValue = String(
            format: L("传输队列 · %d 进行中"), act)
        xclear.isHidden = term == 0
        let empty = xers.isEmpty
        xwrap.isHidden = empty
        xwrapZeroH?.isActive = empty
    }

    func cleanupTmp(_ x: Xfer) {
        if let d = x.tmpDir { _ = LocalFs.rmrf(d) }
        x.tmpDir = nil; x.tmpPath = nil
    }

    private func notifyXfer(_ x: Xfer, _ st: XState) {
        let ok = st == .done
        let kind = ok ? "ok" : st == .cancel ? "info" : "err"
        let verb = ok ? L("传输完成")
                 : st == .cancel ? L("传输已取消") : L("传输失败")
        let t = "\(verb) · \(x.name)"
        let b = "\(x.srcLabel) → \(x.dstLabel)"
        NotifyCenter.shared.post(kind: kind, title: t, body: b,
                                 tabIndex: sftpTabIndex)
        if v_tabFocused() {
            toast(t)
        } else {
            NotifyCenter.shared.system.post(
                title: t, body: b, tabIndex: sftpTabIndex,
                priorityHigh: !ok)
        }
    }
    private var sftpTabIndex: Int? {
        (window as? MainWindow)?.tabs.firstIndex { $0 === tab }
    }
    private func v_tabFocused() -> Bool {
        guard let w = window as? MainWindow else { return false }
        return w.selTab < w.tabs.count && w.tabs[w.selTab] === tab
    }

    func finishXfer(_ x: Xfer, _ st: XState, _ err: String?) {
        x.state = st
        x.req = 0
        x.sess = nil
        if let err { x.err = err }
        if st != .run {
            cleanupTmp(x)
            if x.lcop != nil { x.lcop = nil }
            x.xbtn?.isHidden = st != .fail
            if st == .fail { x.xbtn?.title = L("重试") }
            notifyXfer(x, st)
        }
        updateRow(x)
        xheadSync()
        if st == .done, let dp = x.dp, dp.state == .browse {
            dp.refresh()
        }
    }

    /// SFTP DONE/PROGRESS 分发（pane 事件泵调用）。
    func xferDispatch(_ p: SftpPane, _ r: hb_sftp_result) {
        guard let x = xferByReq(r.req, p.sess) else { return }
        let now = Date()
        if r.kind == HB_SFTP_R_PROGRESS {
            let dd = r.done
            x.acc += dd >= x.done ? dd - x.done : dd
            x.done = dd
            if x.emaT == nil { x.emaT = now; x.emaDone = dd }
            else if dd > x.emaDone {
                let dt = now.timeIntervalSince(x.emaT!)
                if dt >= 0.25 {
                    let inst = Double(dd - x.emaDone) / dt
                    x.ema = x.ema > 0 ? x.ema * 0.6 + inst * 0.4 : inst
                    x.emaT = now; x.emaDone = dd
                }
            }
            x.total = r.total
            updateRow(x)
            return
        }
        // DONE
        if x.kind == .relay && x.phase == 0 && r.status == HB_SFTP_OK {
            x.phase = 1
            x.done = 0; x.total = 0
            guard let ds = x.dp?.sess else {
                finishXfer(x, .fail, L("连接已断开")); return
            }
            let t = SftpKit.pathJoin(x.dstDir, x.name)
            x.req = hb_sftp_upload(ds.sess, x.tmpPath!, t,
                                   hb_sftp_conflict(x.policy))
            x.sess = ds
            updateRow(x)
            return
        }
        if r.skipped { x.skipped = true }
        if r.status == HB_SFTP_OK || r.skipped {
            finishXfer(x, .done, nil)
        } else if r.status == HB_SFTP_ERR_CANCELLED {
            finishXfer(x, .cancel, nil)
        } else {
            finishXfer(x, .fail,
                r.message.map { String(cString: $0) } ?? L("传输失败"))
        }
    }

    /// session 断 → 其上所有 run xfer fail。
    func xferFailSession(_ s: Session?, _ why: String) {
        guard let s else { return }
        for x in xers where x.state == .run {
            if x.sess === s || x.sp?.sess === s || x.dp?.sess === s {
                finishXfer(x, .fail, why)
            }
        }
    }

    // ---------------- local copy ----------------
    private func startLocal(_ x: Xfer) {
        let dst = SftpKit.pathJoin(x.dstDir, x.name)
        let op = LocalCopy(src: x.srcPath, dst: dst, policy: x.policy)
        x.lcop = op
        x.tick = Timer.scheduledTimer(withTimeInterval: 0.12,
                                      repeats: true) { [weak self, weak x] _ in
            guard let self, let x, x.state == .run else { return }
            self.lcTick(x)
        }
        op.run { [weak self, weak x] in
            guard let self, let x else { return }
            x.tick?.invalidate(); x.tick = nil
            if x.state != .run || op.cancelled {
                op.cleanupPartial()
            }
            if x.state != .run {
                // 已被外部置位（取消/断连）
            } else if let e = op.err {
                self.finishXfer(x, .fail, e)
            } else if op.cancelled {
                self.finishXfer(x, .cancel, nil)
            } else if op.skipped {
                x.skipped = true
                self.finishXfer(x, .done, nil)
            } else {
                self.finishXfer(x, .done, nil)
            }
        }
    }
    private func lcTick(_ x: Xfer) {
        guard let o = x.lcop else { return }
        let now = Date()
        x.acc += o.done >= x.done ? o.done - x.done : o.done
        x.done = o.done
        if x.emaT == nil { x.emaT = now; x.emaDone = o.done }
        else if o.done > x.emaDone {
            let dt = now.timeIntervalSince(x.emaT!)
            if dt >= 0.25 {
                let inst = Double(o.done - x.emaDone) / dt
                x.ema = x.ema > 0 ? x.ema * 0.6 + inst * 0.4 : inst
                x.emaT = now; x.emaDone = o.done
            }
        }
        x.total = o.total
        updateRow(x)
    }

    // ---------------- queue buttons ----------------
    @objc private func xferBtn(_ b: NSButton) {
        guard let x = xers.first(where: { $0.id == b.tag }) else { return }
        if x.state == .run {
            if x.req != 0, let s = x.sess {
                hb_sftp_cancel(s.sess, x.req)
            }
            if let o = x.lcop { o.cancel() }
            if x.kind != .local && !(x.req != 0 && x.sess != nil) {
                finishXfer(x, .cancel, nil)
            }
            if x.kind == .local { finishXfer(x, .cancel, nil) }
            return
        }
        if x.state == .fail {
            // 重试：同 policy 重入队；移除旧行
            let it = FsEntry(name: x.name, path: x.srcPath,
                             size: 0, mtime: 0, mode: 0,
                             isDir: false, isLink: false)
            let sp = x.sp, dp = x.dp
            xers.removeAll { $0 === x }
            x.row?.removeFromSuperview()
            xheadSync()
            guard let dp else { return }
            confQueue.append(ConfCtx(todo: [it], policyAll: x.policy,
                                     sp: sp, dp: dp))
            confNext()
        }
    }
    @objc private func clearDone() {
        for x in xers where x.state == .done || x.state == .cancel {
            x.row?.removeFromSuperview()
        }
        xers.removeAll { $0.state == .done || $0.state == .cancel }
        xheadSync()
    }

    // ---------------- 断开/释放 ----------------
    /// dev：取消（run）/重试（fail）指定 xfer —— 与 xferBtn 同路径。
    func devXfer(_ id: Int) {
        let b = NSButton()
        b.tag = id
        xferBtn(b)
    }
    func devClearDone() { clearDone() }

    func disconnectAll() {
        for p in panes {
            if p.ctl != nil || p.sess != nil {
                p.teardownConn()
                p.setState(.select)
            }
        }
    }
    func closeMenus() { for p in panes { p.closeActions() } }
    /// dispose 对齐：未完成删除立即执行；RUN xfer 取消。
    func teardown() {
        for p in panes { p.teardownConn() }
        for x in xers where x.state == .run {
            if x.req != 0, let s = x.sess { hb_sftp_cancel(s.sess, x.req) }
            x.lcop?.cancel()
        }
        xers.removeAll()
        cleanOpenCache()
        xwrapZeroH = xwrap.heightAnchor.constraint(equalToConstant: 0)
        xwrapZeroH.priority = .required
        xheadSync()
    }

    // ============================ chmod sheet ============================
    /// linux act_chmod：3×3 rwx 矩阵 + 八进制，双向同步。
    func chmodSheet(paths: [String], initial: UInt32, pane: SftpPane) {
        let a = NSAlert()
        a.messageText = L("修改权限")
        let sy = ChmodSync()
        let grid = NSGridView()
        grid.rowSpacing = 6
        grid.columnSpacing = 14
        let heads = [L("所有者"), L("组"), L("其他")]
        grid.addRow(with: [NSTextField(labelWithString: "")] +
            heads.map { h -> NSTextField in
                let l = NSTextField(labelWithString: h)
                l.textColor = .secondaryLabelColor
                return l
            })
        let colNames = ["r", "w", "x"]
        for row in 0..<3 {
            var cells: [NSView] = []
            let l = NSTextField(labelWithString: colNames[row])
            l.textColor = .secondaryLabelColor
            cells.append(l)
            for _ in 0..<3 {
                let cb = NSButton(checkboxWithTitle: "",
                                  target: nil, action: nil)
                cb.setAccessibilityIdentifier("chmod-cb")
                cb.target = sy
                cb.action = #selector(ChmodSync.cbToggled)
                sy.cbs.append(cb)
                cells.append(cb)
            }
            grid.addRow(with: cells)
        }
        let oct = NSTextField(frame: NSRect(x: 0, y: 0,
                                            width: 80, height: 22))
        oct.placeholderString = L("八进制，如 755")
        oct.setAccessibilityIdentifier("chmod-oct")
        oct.target = sy
        oct.action = #selector(ChmodSync.octChanged(_:))
        sy.oct = oct
        let box = NSStackView()
        box.orientation = .vertical
        box.spacing = 10
        box.edgeInsets = NSEdgeInsets(top: 4, left: 10,
                                      bottom: 4, right: 10)
        box.addArrangedSubview(grid)
        box.addArrangedSubview(oct)
        a.accessoryView = box
        a.addButton(withTitle: L("应用"))
        a.addButton(withTitle: L("取消"))
        sy.toMatrix(initial)
        DialogRegistry.shared.present(alert: a, kind: "sftp-chmod",
                                      in: window) { ok in
            _ = sy   // sheet 期间保活
            guard ok else { return }
            let m = sy.fromMatrix()
            for p in paths {
                pane.fs?.chmod(p, mode: m) { [weak pane] r in
                    pane?.opRefresh(r)
                }
            }
        }
    }


    // ---------------- dev state ----------------
    var devState: String {
        var s = ""
        for (i, p) in panes.enumerated() {
            s += "pane\(i): \(p.devState)\n"
        }
        s += "xers=\(xers.count)["
        s += xers.map {
            "\($0.id):\($0.name):\($0.state):\($0.done)/\($0.total)"
            + ($0.err.map { ":err=\($0)" } ?? "")
        }.joined(separator: ",")
        s += "]"
        return s
    }
}
extension hb_sftp_conflict {
    init(_ v: Int32) {
        self.init(rawValue: UInt32(bitPattern: v))
    }
}
private let HB_SFTP_OVERWRITE_I = Int32(HB_SFTP_OVERWRITE.rawValue)
extension XState: CustomStringConvertible {
    var description: String {
        switch self {
        case .run: return "run"
        case .done: return "done"
        case .fail: return "fail"
        case .cancel: return "cancel"
        }
    }
}

/// NSScrollView 的 document view 默认非翻转 → 内容吸底；
/// 翻转栈顶排（linux GtkBox 自上而下同序）。
private final class FlippedStack: NSStackView {
    override var isFlipped: Bool { true }
}

/// chmod sheet 的矩阵↔八进制同步（linux ChmodCtx 等价）。
private final class ChmodSync: NSObject {
    var cbs: [NSButton] = []
    weak var oct: NSTextField?
    private var updating = false
    func fromMatrix() -> UInt32 {
        var m: UInt32 = 0
        for col in 0..<3 { for row in 0..<3 {
            if cbs[row * 3 + col].state == .on {
                m |= 1 << (8 - (col * 3 + row))
            }
        } }
        return m
    }
    func toMatrix(_ m: UInt32) {
        updating = true
        for col in 0..<3 { for row in 0..<3 {
            cbs[row * 3 + col].state =
                (m & (1 << (8 - (col * 3 + row)))) != 0 ? .on : .off
        } }
        updating = false
        oct?.stringValue = String(m, radix: 8)
    }
    @objc func cbToggled() {
        guard !updating else { return }
        oct?.stringValue = String(fromMatrix(), radix: 8)
    }
    @objc func octChanged(_ tf: NSTextField) {
        guard !updating else { return }
        var v: UInt64 = 0
        var ok = !tf.stringValue.isEmpty
        for ch in tf.stringValue {
            guard let d = Int(String(ch)), d < 8 else { ok = false; break }
            v = v * 8 + UInt64(d)
        }
        if ok && v <= 0o777 { toMatrix(UInt32(v)) }
    }
}
