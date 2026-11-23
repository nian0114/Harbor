import Foundation
import CHarbor
import Dispatch

/// 一个远程桌面会话：event_fd → DispatchSourceRead 主队列消费
///（镜像 Session 之于 hb_session；hb_rd 的内部 worker 独占驱动
/// libvncclient/FreeRDP，主线程只取事件与取帧）。
final class RdSession {
    private(set) var rd: OpaquePointer!
    private var source: DispatchSourceRead?
    var onEvent: ((hb_rd_event) -> Void)?
    var exited = false

    /// opts 字段为借用指针；core 在 hb_rd_new 里深拷贝。
    init(opts: UnsafeMutablePointer<hb_rd_opts>) {
        rd = hb_rd_new(opts)
        guard rd != nil else { return }
        let fd = hb_rd_event_fd(rd)
        let src = DispatchSource.makeReadSource(fileDescriptor: fd,
                                                queue: .main)
        src.setEventHandler { [weak self] in self?.drain() }
        src.resume()
        source = src
    }

    func drain() {
        guard rd != nil else { return }
        var ev = hb_rd_event()
        while rd != nil && hb_rd_next_event(rd, &ev) {
            if ev.type == HB_RD_EV_EXIT { exited = true }
            onEvent?(ev)
            hb_rd_event_clear(&ev)
        }
    }

    /// 锁帧缓冲；返回后尽快调用 unlock（锁内 worker 的像素拷贝会阻塞）。
    func lockFb() -> (px: UnsafePointer<UInt8>, w: Int, h: Int,
                      stride: Int)? {
        guard rd != nil else { return nil }
        var px: UnsafePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0, stride: Int32 = 0
        guard hb_rd_fb_lock(rd, &px, &w, &h, &stride), let px
        else { return nil }
        return (px, Int(w), Int(h), Int(stride))
    }
    func unlockFb() { if rd != nil { hb_rd_fb_unlock(rd) } }

    func certReply(_ accept: Bool) {
        if rd != nil { hb_rd_cert_reply(rd, accept) }
    }
    func promptReply(user: String?, password: String?, domain: String?) {
        guard rd != nil else { return }
        /* core 在调用内深拷贝（xstrdup/secret_strdup）——strdup 后即释放 */
        let u = user.map { strdup($0)! }
        let p = password.map { strdup($0)! }
        let d = domain.map { strdup($0)! }
        hb_rd_prompt_reply(rd,
                           u.map { UnsafePointer($0) },
                           p.map { UnsafePointer($0) },
                           d.map { UnsafePointer($0) })
        if let p { memset(p, 0, strlen(p)); free(p) }
        free(u); free(d)
    }
    func pointer(_ x: Int, _ y: Int, _ buttons: UInt32) {
        if rd != nil { hb_rd_pointer(rd, Int32(x), Int32(y), buttons) }
    }
    func key(_ keysym: UInt32, _ kc: UInt32, _ down: Bool) {
        if rd != nil { hb_rd_key(rd, keysym, kc, down) }
    }
    func clipboard(_ utf8: String) {
        guard rd != nil else { return }
        utf8.withCString { hb_rd_clipboard(rd, $0) }
    }
    func requestSize(_ w: Int, _ h: Int) {
        if rd != nil { hb_rd_request_size(rd, Int32(w), Int32(h)) }
    }
    func setQuality(_ q: Int) {
        guard rd != nil else { return }
        hb_rd_set_quality(rd, hb_rd_quality(UInt32(q)))
    }
    func close() { if rd != nil { hb_rd_close(rd) } }

    /// 主线程摘除事件源——cancel 后不再有 drain 入队，之后
    /// free_() 可在后台安全做 hb_rd_free（对齐 GTK g_source_remove
    /// 先于 worker join 的顺序）。
    func stopSource() {
        source?.cancel()
        source = nil
    }
    /// 收尾（hb_rd_free 有界等待 worker 汇合——调用方在后台线程跑，
    /// 见 DesktopTab.kill）。
    func free_() {
        stopSource()
        rd.map { hb_rd_free($0) }
        rd = nil
    }

    deinit { free_() }
}
