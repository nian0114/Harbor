import Foundation
import CHarbor

/// FR-08 Snippet `{{var}}` 解析/渲染 —— 薄封装 core hb_snippet_*，
/// 供 UI 与单测共用（core 是唯一实现，保证与 Linux 逐字节一致）。
public enum SnipKit {
    /// (name, default?)，按出现顺序去重（与 core 一致）。
    public static func vars(of body: String) -> [(name: String, def: String?)] {
        var names: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?
        var defs: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?
        let n = body.withCString { hb_snippet_vars($0, &names, &defs) }
        var out: [(String, String?)] = []
        for i in 0..<n {
            out.append((names?[i].map { String(cString: $0) } ?? "",
                        defs?[i].map { String(cString: $0) }))
        }
        if let names { hb_strfreev(names, n) }
        if let defs { hb_strfreev(defs, n) }
        return out
    }
    /// values 里缺省/空串 → 该变量传 NULL，core 回退默认值。
    public static func render(_ body: String,
                              values: [String: String]) -> String {
        let vs = vars(of: body)
        if vs.isEmpty { return body }
        var names: [UnsafePointer<CChar>?] = vs.map {
            UnsafePointer(strdup($0.name))
        }
        var vals: [UnsafePointer<CChar>?] = vs.map { v in
            let s = values[v.name] ?? ""
            return s.isEmpty ? nil : UnsafePointer(strdup(s))
        }
        defer {
            for p in names { free(UnsafeMutablePointer(mutating: p)) }
            for p in vals { free(UnsafeMutablePointer(mutating: p)) }
        }
        guard let r = body.withCString({
            hb_snippet_render($0, &names, &vals, vs.count) })
        else { return body }
        defer { free(r) }
        return String(cString: r)
    }
    /// scope 展示标签（对齐 Linux：全局 / 主机 / 分组名）。
    public static func scopeLabel(_ scope: String, hostName: (String) -> String?) -> String {
        if scope.hasPrefix("host:") {
            let id = String(scope.dropFirst(5))
            return hostName(id) ?? id
        }
        if scope.hasPrefix("group:") { return String(scope.dropFirst(6)) }
        return L("全局")
    }
}

/// FR-10 日志展示映射 —— 对齐 linux/src/hb-logs-view.c 的
/// result_disp/reason_disp/dur_disp。
public enum LogFormat {
    /// (文本, css 类名等价物)：run/on/off/err。
    public static func result(_ r: String, durationS: Int64) -> (String, String) {
        if durationS < 0 || r == "connecting" { return (L("进行中"), "run") }
        if r == "ok" { return (L("成功"), "on") }
        if r == "cancelled" { return (L("已取消"), "off") }
        if r == "interrupted" { return (L("中断"), "wait") }
        return (L("失败"), "err")
    }
    private static let reasonMap: [(String, String)] = [
        ("auth_failed", L("认证失败")),
        ("unreachable", L("无法连接")),
        ("resolve", L("无法连接")),
        ("refused", L("无法连接")),
        ("hostkey_changed", L("主机密钥变更")),
        ("hostkey_rejected", L("主机密钥被拒绝")),
        ("timeout", L("超时")),
        ("cancelled", L("已取消")),
        ("interrupted", L("中断")),
        ("process exited", L("进程已退出")),
        ("key confirm denied", L("已取消")),
        ("ok", ""),
        ("connecting", L("进行中")),
    ]
    public static func reason(_ reason: String, result: String) -> String {
        let r = !reason.isEmpty ? reason : result
        if r.isEmpty { return "" }
        for (k, zh) in reasonMap where k == r { return zh }
        return r
    }
    public static func duration(_ s: Int64) -> String {
        if s < 0 { return L("进行中") }
        if s < 60 { return "\(s)s" }
        return "\(s / 60)m \(s % 60)s"
    }
    /// ms 时间戳 → "yyyy-MM-dd HH:mm"（本地时区，对齐 Linux strftime）。
    public static func date(_ ms: Int64) -> String {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return f.string(from: Date(timeIntervalSince1970:
                                    TimeInterval(ms) / 1000))
    }
    /// ms → "yyyy-MM-dd"（known added 列）。
    public static func dateOnly(_ ms: Int64) -> String {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd"
        return f.string(from: Date(timeIntervalSince1970:
                                    TimeInterval(ms) / 1000))
    }
}

/// FR-09 known_hosts 主机名展示 —— 对齐 Linux host_disp：
/// hashed `|1|` 主机显示 "（hashed）|1|前12字符…"。
public enum KnownFormat {
    public static func hostDisp(_ host: String) -> String {
        if host.hasPrefix("|1|") {
            return L("（hashed）") + String(host.prefix(12)) + "…"
        }
        return host
    }
}
