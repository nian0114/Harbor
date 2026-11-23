import Foundation

/// SFTP 视图纯逻辑（对齐 linux hb-sftp-view.c 的 helper / core sftp.c
/// ren_variant）——无 UI 依赖，供 SwiftUI/AppKit 页面与测试复用。
public enum SftpKit {

    /// path_join：dir + '/' + name；dir 以 '/' 结尾不重复。
    public static func pathJoin(_ dir: String, _ name: String) -> String {
        if dir.hasSuffix("/") { return dir + name }
        return dir + "/" + name
    }

    /// parent_dir："/a/b" → "/a"；"/a" → "/"；"/" → "/"。
    public static func parentDir(_ p: String) -> String {
        var s = p
        while s.count > 1 && s.hasSuffix("/") { s.removeLast() }
        guard let i = s.lastIndex(of: "/") else { return "/" }
        return i == s.startIndex ? "/" : String(s[..<i])
    }

    /// 面包屑："/a/b" → [("/", "/"), ("a","/a"), ("b","/a/b")]。
    public static func crumbs(_ cwd: String) -> [(label: String, path: String)] {
        var out: [(String, String)] = [("/", "/")]
        var cur = ""
        for seg in cwd.split(separator: "/") {
            cur += "/" + seg
            out.append((String(seg), cur))
        }
        return out
    }

    /// RENAME 冲突变体名（core ren_variant / linux lc_rename_free 同规）：
    /// 普通文件在最后的 '.' 前插 "(n)"（big.bin → big (1).bin）；
    /// 目录、点文件（'.' 在首字符）、无扩展名一律尾部追加
    /// （sub → sub (1)，.bashrc → .bashrc (1)）。
    public static func renVariant(_ name: String, isDir: Bool, _ n: Int)
        -> String {
        let dot = isDir ? nil : name.lastIndex(of: ".")
        let eff = (dot == name.startIndex) ? nil : dot
        if let d = eff {
            return "\(name[..<d]) (\(n))\(name[d...])"
        }
        return "\(name) (\(n))"
    }

    /// 找一个空闲的 "name (N)" 名（已存在集合判断由调用方给）。
    public static func firstFreeName(_ name: String, isDir: Bool,
                                     exists: (String) -> Bool) -> String? {
        for n in 1..<1000 {
            let c = renVariant(name, isDir: isDir, n)
            if !exists(c) { return c }
        }
        return nil
    }

    /// stat/lstat 统一走 fstatat：`stat` 与 Darwin 结构体同名，
    /// 直接调用易被解析成类型。follow=true 跟随符号链接。
    public static func statPath(_ path: String,
                                follow: Bool = true) -> stat? {
        var st = stat()
        let rc = path.withCString {
            fstatat(AT_FDCWD, $0, &st,
                    follow ? 0 : AT_SYMLINK_NOFOLLOW)
        }
        return rc == 0 ? st : nil
    }
    public static func modeIsDir(_ m: mode_t) -> Bool {
        (m & S_IFMT) == S_IFDIR
    }
    public static func modeIsReg(_ m: mode_t) -> Bool {
        (m & S_IFMT) == S_IFREG
    }
    public static func modeIsLnk(_ m: mode_t) -> Bool {
        (m & S_IFMT) == S_IFLNK
    }

    /// mode & 0777 → "rwxr-x--x"（linux fac_bind_mode）。
    public static func modeString(_ mode: UInt32) -> String {
        let m = mode & 0o777
        let rwx = Array("rwx")
        var s = ""
        for k in 0..<9 {
            s.append((m & UInt32(1 << (8 - k))) != 0
                     ? rwx[k % 3] : "-")
        }
        return s
    }

    /// 传输方向标签。
    public static func fmtSize(_ b: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(b),
                                  countStyle: .file)
    }
    public static func fmtSpeed(_ bps: Double) -> String {
        fmtSize(UInt64(max(0, bps))) + "/s"
    }

    /// 列表 mtime 列：linux "%m-%d %H:%M"。
    public static func fmtMtime(_ t: Int64) -> String {
        let f = DateFormatter()
        f.dateFormat = "MM-dd HH:mm"
        return f.string(from: Date(timeIntervalSince1970: TimeInterval(t)))
    }

    /// 目录优先比较器（linux dirfirst_cmp + 列排序组合；kind 列同
    /// linux 按 name_cmp）。
    public enum SortKey { case name, mtime, size, kind, mode }
    public static func sortEntries<T>(_ arr: [T], key: SortKey,
                                      ascending: Bool, isDir: (T) -> Bool,
                                      name: (T) -> String,
                                      value: (T) -> Int64) -> [T] {
        arr.sorted { a, b in
            let da = isDir(a), db = isDir(b)
            if da != db { return da }
            let c: Bool
            switch key {
            case .name, .kind:
                c = name(a).localizedCaseInsensitiveCompare(name(b)) == .orderedAscending
            default:
                c = value(a) == value(b)
                    ? name(a).localizedCaseInsensitiveCompare(name(b)) == .orderedAscending
                    : value(a) < value(b)
            }
            return ascending ? c : !c
        }
    }
}
