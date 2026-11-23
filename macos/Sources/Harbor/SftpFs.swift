import AppKit
import CHarbor
import HarborKit

/// 文件项（对齐 linux HbFileItem / hb_sftp_entry 的浏览侧投影）。
struct FsEntry {
    let name: String
    let path: String          // 全路径（cwd join name）
    let size: UInt64
    let mtime: Int64
    let mode: UInt32
    let isDir: Bool
    let isLink: Bool
}

/// hb_fs_result 等价物；字段回调后不再借用（已拷贝）。
struct FsRes {
    var ok = false
    var status: Int32 = 0
    var message: String?
    var entries: [FsEntry] = []
    var path: String?
}

/// hb_fs 等价物：local/remote 统一异步文件操作，回调均在主线程。
class Fs {
    func list(_ dir: String, _ cb: @escaping (FsRes) -> Void) { fatalError() }
    func stat(_ path: String, _ cb: @escaping (FsRes) -> Void) { fatalError() }
    func realpath(_ path: String?, _ cb: @escaping (FsRes) -> Void) { fatalError() }
    func mkdir(_ path: String, mode: UInt32,
               _ cb: @escaping (FsRes) -> Void) { fatalError() }
    func rename(_ from: String, _ to: String, overwrite: Bool,
                _ cb: @escaping (FsRes) -> Void) { fatalError() }
    func remove(_ path: String, recursive: Bool,
                _ cb: @escaping (FsRes) -> Void) { fatalError() }
    func chmod(_ path: String, mode: UInt32,
               _ cb: @escaping (FsRes) -> Void) { fatalError() }
    /// hb_fs_free：之后不再回调（in-flight op 丢弃）。
    func invalidate() {}
}

// ============================ 本地 POSIX 后端 ============================

final class LocalFs: Fs {
    private let q = DispatchQueue(label: "hb.fs.local")
    private var dead = false
    override func invalidate() { dead = true }

    private func submit(_ work: @escaping () -> FsRes,
                        _ cb: @escaping (FsRes) -> Void) {
        q.async { [weak self] in
            let r = work()
            DispatchQueue.main.async {
                guard self?.dead != true else { return }
                cb(r)
            }
        }
    }

    private static func entry(_ path: String, _ st: stat) -> FsEntry {
        let isDir = (st.st_mode & S_IFMT) == S_IFDIR
        let isLink = (st.st_mode & S_IFMT) == S_IFLNK
        return FsEntry(name: URL(fileURLWithPath: path).lastPathComponent,
                       path: path, size: UInt64(st.st_size),
                       mtime: Int64(st.st_mtimespec.tv_sec),
                       mode: UInt32(st.st_mode), isDir: isDir, isLink: isLink)
    }
    private static func err(_ path: String) -> FsRes {
        var r = FsRes()
        r.status = Int32(HB_SFTP_ERR_LOCAL.rawValue)
        r.message = String(cString: strerror(errno))
        return r
    }

    override func list(_ dir: String, _ cb: @escaping (FsRes) -> Void) {
        submit({
            var r = FsRes()
            guard let names = try? FileManager.default
                .contentsOfDirectory(atPath: dir) else { return Self.err(dir) }
            for n in names {
                let p = SftpKit.pathJoin(dir, n)
                var st = Darwin.stat()
                guard lstat(p, &st) == 0 else { continue }
                var e = Self.entry(p, st)
                e = FsEntry(name: n, path: e.path, size: e.size,
                            mtime: e.mtime, mode: e.mode,
                            isDir: e.isDir, isLink: e.isLink)
                r.entries.append(e)
            }
            r.ok = true
            return r
        }, cb)
    }
    override func stat(_ path: String, _ cb: @escaping (FsRes) -> Void) {
        submit({
            var r = FsRes()
            var st = Darwin.stat()
            guard lstat(path, &st) == 0 else {
                r.status = Int32(HB_SFTP_ERR_NOENT.rawValue)
                r.message = String(cString: strerror(errno))
                return r
            }
            r.entries = [Self.entry(path, st)]
            r.ok = true
            return r
        }, cb)
    }
    override func realpath(_ path: String?,
                           _ cb: @escaping (FsRes) -> Void) {
        submit({
            var r = FsRes()
            let p = (path?.isEmpty == false) ? path! : NSHomeDirectory()
            var buf = [CChar](repeating: 0, count: Int(PATH_MAX))
            if Darwin.realpath(p, &buf) != nil {
                r.path = String(cString: buf)
                r.ok = true
            } else { r = Self.err(p) }
            return r
        }, cb)
    }
    override func mkdir(_ path: String, mode: UInt32,
                        _ cb: @escaping (FsRes) -> Void) {
        submit({
            var r = FsRes()
            if Darwin.mkdir(path, mode_t(mode & 0o777)) == 0 { r.ok = true }
            else { r = Self.err(path) }
            return r
        }, cb)
    }
    override func rename(_ from: String, _ to: String, overwrite: Bool,
                         _ cb: @escaping (FsRes) -> Void) {
        submit({
            var r = FsRes()
            var st = Darwin.stat()
            if !overwrite && lstat(to, &st) == 0 {
                r.status = Int32(HB_SFTP_ERR_EXISTS.rawValue)
                r.message = L("目标已存在")
                return r
            }
            if Darwin.rename(from, to) == 0 { r.ok = true }
            else { r = Self.err(from) }
            return r
        }, cb)
    }
    override func remove(_ path: String, recursive: Bool,
                         _ cb: @escaping (FsRes) -> Void) {
        submit({
            var r = FsRes()
            var st = Darwin.stat()
            guard lstat(path, &st) == 0 else {
                r.status = Int32(HB_SFTP_ERR_NOENT.rawValue)
                r.message = String(cString: strerror(errno))
                return r
            }
            let isDir = (st.st_mode & S_IFMT) == S_IFDIR
            if isDir && !recursive {
                r.ok = (Darwin.rmdir(path) == 0)
                if !r.ok { r = Self.err(path) }
                return r
            }
            if isDir {
                // 递归删：lstat 语义（符号链接按链接删，不跟随）
                let ok = Self.rmrf(path)
                if ok { r.ok = true } else { r = Self.err(path) }
            } else {
                r.ok = (unlink(path) == 0)
                if !r.ok { r = Self.err(path) }
            }
            return r
        }, cb)
    }
    /// lc_rm_rf 等价：目录递归、链接按链接删除。
    static func rmrf(_ path: String) -> Bool {
        var st = Darwin.stat()
        guard lstat(path, &st) == 0 else { return true }
        if (st.st_mode & S_IFMT) == S_IFDIR {
            if let names = try? FileManager.default
                .contentsOfDirectory(atPath: path) {
                for n in names {
                    _ = rmrf(SftpKit.pathJoin(path, n))
                }
            }
            return Darwin.rmdir(path) == 0
        }
        return unlink(path) == 0
    }
    override func chmod(_ path: String, mode: UInt32,
                        _ cb: @escaping (FsRes) -> Void) {
        submit({
            var r = FsRes()
            if Darwin.chmod(path, mode_t(mode & 0o777)) == 0 { r.ok = true }
            else { r = Self.err(path) }
            return r
        }, cb)
    }
}

// ============================ 远端 SFTP 后端 ============================

/// hb_fs remote：owner 把每个 HB_SESS_EV_SFTP 事件喂给 handleEvent。
final class RemoteFs: Fs {
    private let sess: Session
    private var pend: [UInt32: (FsRes) -> Void] = [:]
    private var dead = false
    init(sess: Session) { self.sess = sess }
    override func invalidate() { dead = true; pend.removeAll() }

    /// sftp 请求提交；req==0 → 立即回 CONN 错（对齐 remote_submit）。
    private func submit(_ req: UInt32, _ cb: @escaping (FsRes) -> Void) {
        if req == 0 || dead {
            var r = FsRes()
            r.status = Int32(HB_SFTP_ERR_CONN.rawValue)
            r.message = L("连接已断开")
            if !dead { cb(r) }
            return
        }
        pend[req] = cb
    }

    /// owner 喂入会话事件；true = 已消费。
    @discardableResult
    func handleEvent(_ ev: UnsafePointer<hb_sess_event>) -> Bool {
        guard ev.pointee.type == HB_SESS_EV_SFTP,
              let s = ev.pointee.sftp else { return false }
        let r = s.pointee
        guard let cb = pend.removeValue(forKey: r.req) else { return false }
        if r.kind == HB_SFTP_R_DONE && !dead {
            cb(Self.res(r, cwd: nil))
        }
        return true
    }

    /// hb_sftp_result → FsRes（拷贝 entries）。
    static func res(_ r: hb_sftp_result, cwd: String?) -> FsRes {
        var out = FsRes()
        out.ok = r.status == HB_SFTP_OK
        out.status = Int32(bitPattern: r.status.rawValue)
        out.message = r.message.map { String(cString: $0) }
        out.path = r.path.map { String(cString: $0) }
        if r.n > 0, let es = r.entries {
            for i in 0..<Int(r.n) {
                let e = es[i]
                let nm = e.name.map { String(cString: $0) } ?? ""
                out.entries.append(FsEntry(
                    name: nm,
                    path: cwd.map { SftpKit.pathJoin($0, nm) } ?? nm,
                    size: e.size, mtime: e.mtime,
                    mode: e.mode, isDir: e.is_dir, isLink: e.is_link))
            }
        }
        return out
    }

    override func list(_ dir: String, _ cb: @escaping (FsRes) -> Void) {
        submit(hb_sftp_list(sess.sess, dir)) { r in
            // list 的 entry.path 由 pane 端 join cwd（见 pane_refresh）
            cb(r)
        }
    }
    override func stat(_ path: String, _ cb: @escaping (FsRes) -> Void) {
        submit(hb_sftp_stat(sess.sess, path), cb)
    }
    override func realpath(_ path: String?,
                           _ cb: @escaping (FsRes) -> Void) {
        submit(hb_sftp_realpath(sess.sess,
                                (path?.isEmpty == false) ? path! : "."), cb)
    }
    override func mkdir(_ path: String, mode: UInt32,
                        _ cb: @escaping (FsRes) -> Void) {
        submit(hb_sftp_mkdir(sess.sess, path, mode), cb)
    }
    override func rename(_ from: String, _ to: String, overwrite: Bool,
                         _ cb: @escaping (FsRes) -> Void) {
        submit(hb_sftp_rename(sess.sess, from, to, overwrite), cb)
    }
    override func remove(_ path: String, recursive: Bool,
                         _ cb: @escaping (FsRes) -> Void) {
        submit(hb_sftp_remove(sess.sess, path, recursive), cb)
    }
    override func chmod(_ path: String, mode: UInt32,
                        _ cb: @escaping (FsRes) -> Void) {
        submit(hb_sftp_chmod(sess.sess, path, mode), cb)
    }
}
