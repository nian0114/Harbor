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
    func free_() {
        source?.cancel()
        source = nil
        sess.map { hb_session_free($0) }
        sess = nil
    }
    var alive: Bool { sess != nil && hb_session_alive(sess) }
    deinit { free_() }
}
