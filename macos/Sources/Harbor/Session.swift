import Foundation
import CHarbor
import Dispatch

/// 一个本地终端会话：event_fd → DispatchSourceRead 主队列消费。
final class Session {
    private(set) var sess: OpaquePointer!
    var vt: OpaquePointer? { sess.map { hb_session_vt($0) } ?? nil }
    private var source: DispatchSourceRead?
    var onEvent: ((hb_sess_event) -> Void)?
    var exited = false

    init(shell: String? = nil, rows: Int32, cols: Int32,
         scrollback: Int32, reuse: OpaquePointer? = nil) {
        var o = hb_pty_opts()
        let shellC = (shell ?? ProcessInfo.processInfo.environment["SHELL"]
                      ?? NSUserName()).withCString { strdup($0) }
        o.shell = UnsafePointer(shellC)
        o.command = nil
        let cwd = NSHomeDirectory().withCString { strdup($0) }
        o.cwd = UnsafePointer(cwd)
        o.env_extra = nil
        o.rows = rows; o.cols = cols
        if let r = reuse {
            sess = hb_session_new_local_vt(&o, r, scrollback)
        } else {
            sess = hb_session_new_local(&o, scrollback)
        }
        free(shellC); free(cwd)
        attachFd()
    }

    /// SSH 会话（shell/sftp/forward）：opts 由 ConnCtl 组装；
    /// core 在返回前深拷贝，opts 内的借用指针随后即可擦除。
    init(sshOpts o: UnsafeMutablePointer<hb_ssh_opts>,
         rows: Int32, cols: Int32, scrollback: Int32) {
        if o.pointee.mode == HB_SSH_MODE_SFTP {
            sess = hb_session_new_sftp(o)
        } else {
            sess = hb_session_new_ssh(o, rows, cols, scrollback)
        }
        attachFd()
    }

    /// FORWARD 会话：规则随会话创建（local/dynamic 的监听器在
    /// 调用线程同步绑定；失败 sess==nil，forwardErr = 中文消息）。
    private(set) var forwardErr: String?
    init(forwardOpts o: UnsafeMutablePointer<hb_ssh_opts>,
         rule: UnsafeMutablePointer<hb_pf_rule>) {
        var err: UnsafeMutablePointer<CChar>?
        sess = hb_session_new_forward(o, rule, nil, &err)
        forwardErr = err.map { String(cString: $0) }
        if let err { free(err) }
        attachFd()
    }

    private func attachFd() {
        guard sess != nil else { return }
        let fd = hb_session_event_fd(sess)
        let src = DispatchSource.makeReadSource(fileDescriptor: fd,
                                                queue: .main)
        src.setEventHandler { [weak self] in self?.drain() }
        src.setCancelHandler { }
        src.resume()
        source = src
    }

    func drain() {
        guard sess != nil else { return }
        var ev = hb_sess_event()
        while hb_session_next_event(sess, &ev) {
            if ev.type == HB_SESS_EV_EXIT { exited = true }
            onEvent?(ev)
            hb_sess_event_clear(&ev)
        }
    }

    func stealVt() -> OpaquePointer? {
        sess.map { hb_session_steal_vt($0) }
    }
    func close() { sess.map { hb_session_close($0) } }
    /// 主线程摘除事件源（先于后台 free_，保证无在途 drain 碰 sess）。
    func stopSource() {
        source?.cancel()
        source = nil
    }
    func free_() {
        stopSource()
        // FR-12：session 先于引擎死 → 摘 tap 前停止其共享
        ShareService.shared.stopSession(self)
        sess.map { hb_session_free($0) }
        sess = nil
    }
    var alive: Bool { sess != nil && hb_session_alive(sess) }
    deinit { free_() }
}
