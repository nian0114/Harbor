import Foundation

/// M6 账号同步（FR-11）纯逻辑层：无 AppKit/网络依赖，可直接单测。
/// 规则逐行对齐 linux/src/hb-sync.c 的同名函数。
public enum SyncKit {

    /// WS 断开重连表（对齐 Linux kWSRetry），循环使用最后一个值。
    public static let wsRetry: [TimeInterval] = [1, 2, 4, 8, 15, 30]

    /// 服务器地址策略（对齐 hb_sync_check_server）：
    /// 仅 https://；http:// 只允许回环（127.0.0.0/8、::1、localhost）。
    /// 返回规范化 "scheme://host[:port][/path]"，非法返回 nil。
    public static func checkServer(_ raw: String) -> String? {
        let t = raw.trimmingCharacters(in: .whitespaces)
        if t.isEmpty || t.count > 2000 { return nil }
        guard let c = URLComponents(string: t),
              let scheme = c.scheme?.lowercased(),
              scheme == "https" || scheme == "http",
              var host = c.host, !host.isEmpty
        else { return nil }
        // URLComponents 对 IPv6 host 可能带 [] —— 先剥离
        if host.hasPrefix("[") && host.hasSuffix("]") {
            host = String(host.dropFirst().dropLast())
        }
        if scheme == "http" && !isLoopbackHost(host) { return nil }
        var out = "\(scheme)://"
        if host.contains(":") { out += "[\(host)]" } else { out += host }
        if let p = c.port { out += ":\(p)" }
        let path = c.percentEncodedPath
        if !path.isEmpty && path != "/" { out += path }
        return out
    }

    /// 对齐 g_inet_address_get_is_loopback：IPv4 127/8、IPv6 ::1；
    /// + "localhost"（Linux 同样放行）。
    public static func isLoopbackHost(_ host: String) -> Bool {
        host.caseInsensitiveCompare("localhost") == .orderedSame
            || host.hasPrefix("127.") || host == "::1"
    }

    /// hb_norm_email：去空白 + 小写；非法返回 nil。
    public static func normEmail(_ raw: String) -> String? {
        let t = raw.trimmingCharacters(in: .whitespaces)
            .lowercased()
        return t.isEmpty ? nil : t
    }

    /// 对齐 Linux nfkc（g_utf8_normalize NFKC）。
    public static func nfkc(_ s: String) -> String {
        (s as NSString).precomposedStringWithCompatibilityMapping
    }

    /// 拆分 24 词恢复码。
    public static func splitWords(_ s: String) -> [String] {
        s.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" })
            .map(String.init)
    }

    /// 设备平台名（对齐 Linux "macos"）。
    public static let platform = "macos"

    /// 设备显示名（对齐 hb_device_name → g_get_host_name）。
    public static func deviceName() -> String {
        ProcessInfo.processInfo.hostName
    }

    /// 服务器错误 JSON {"error":{code,message}} 解析（对齐 post_json 的错误分支）。
    public static func parseErr(_ resp: String) -> (code: String, msg: String, retryAfter: Int)? {
        guard let d = resp.data(using: .utf8),
              let o = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any],
              let e = o["error"] as? [String: Any]
        else { return nil }
        var code = (e["code"] as? String) ?? "server_error"
        var msg = (e["message"] as? String) ?? L("服务器错误")
        if code == "totp_required" { msg = L("需要两步验证码") }
        if code == "totp_invalid" {
            code = "totp_invalid"; msg = L("两步验证码错误")
        }
        let retry = (e["retry_after"] as? Int)
            ?? Int((e["retry_after"] as? String) ?? "") ?? 0
        return (code, msg, retry)
    }

    /// 通用失败提示（对齐 post_json 各 HTTP 状态分支）。
    public static func httpErrMsg(_ status: Int) -> String {
        switch status {
        case 401, 403: return L("邮箱或密码错误")
        case 404: return L("账号不存在")
        case 409: return L("该邮箱已注册")
        case 429: return L("尝试过于频繁，请稍后再试")
        case 400...499: return L("请求被拒绝 (\(status))")
        default: return L("服务器错误 (\(status))")
        }
    }

    /// 限流提示（对齐 ngettext 分支）。
    public static func retryMsg(_ retryAfter: Int) -> String {
        L("尝试次数过多，请 \(retryAfter) 秒后再试")
    }

    /// 冲突字段是否敏感（对齐 conflict_card 的 secret 判定：
    /// 字段名以 $ 开头或含 secret/key/password）。
    public static func isSecretField(_ f: String) -> Bool {
        f.hasPrefix("$") || f.contains("secret")
            || f.contains("key") || f.contains("password")
    }

    /// 设备列表 JSON 解析（{"devices":[{id,name,platform,created,last_seen,current}]}）。
    public struct DeviceInfo {
        public let id: String
        public let name: String
        public let platform: String
        public let created: Int64
        public let lastSeen: Int64
        public let current: Bool
    }

    public static func parseDevices(_ resp: String) -> [DeviceInfo] {
        guard let d = resp.data(using: .utf8),
              let o = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any],
              let arr = o["devices"] as? [[String: Any]]
        else { return [] }
        return arr.map {
            DeviceInfo(id: ($0["id"] as? String) ?? "",
                       name: ($0["name"] as? String) ?? "",
                       platform: ($0["platform"] as? String) ?? "",
                       created: int64($0["created_at"]),
                       lastSeen: int64($0["last_seen"]),
                       current: ($0["current"] as? Bool) ?? false)
        }
    }

    private static func int64(_ v: Any?) -> Int64 {
        if let n = v as? Int64 { return n }
        if let n = v as? Int { return Int64(n) }
        if let n = v as? Double { return Int64(n) }
        if let s = v as? String { return Int64(s) ?? 0 }
        return 0
    }

    /// 冲突卡：从对象 JSON 提取字段的显示值（对齐 jmember_str）。
    public static func memberStr(_ objJson: String?, _ field: String) -> String? {
        guard let j = objJson, let d = j.data(using: .utf8),
              let o = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any],
              let v = o[field]
        else { return nil }
        switch v {
        case let s as String: return s
        case let b as Bool: return b ? "true" : "false"
        case let n as NSNumber:
            if CFGetTypeID(n) == CFBooleanGetTypeID() {
                return n.boolValue ? "true" : "false"
            }
            if n.doubleValue == n.doubleValue.rounded()
               && fabs(n.doubleValue) < 1e15 {
                return "\(n.int64Value)"
            }
            return "\(n.doubleValue)"
        default: return nil
        }
    }
}
