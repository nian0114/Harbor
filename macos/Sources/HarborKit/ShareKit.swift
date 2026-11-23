import Foundation

/// FR-12 共享：纯逻辑（链接解析/配对码展示/结束原因/权限名）。
/// 协议格式 docs/api/share-protocol.md §3：
/// `harbor://share/<share_id>?s=<b64url(server_url)>#<b64url(link_secret)>`
/// fragment（link_secret）不出客户端 —— 这里只解析，不接触 secret 语义。
public enum ShareKit {

    // ---------------- 链接 ----------------

    /// 解析 harbor://share 链接。返回 nil = 不是合法共享链接。
    /// guest 加入时完整链接交给 core hb_share_peer_new_guest；UI 只需要
    /// share_id/server 做展示与预检。
    public static func parseLink(_ s: String)
        -> (shareId: String, server: String, secret: Bool)?
    {
        let p = "harbor://share/"
        guard s.hasPrefix(p) else { return nil }
        var rest = String(s.dropFirst(p.count))
        // fragment：链接 secret（b64url），不出客户端
        var hasSecret = false
        if let h = rest.firstIndex(of: "#") {
            hasSecret = !rest[rest.index(after: h)...].isEmpty
            rest = String(rest[..<h])
        }
        var server = ""
        var idPart = rest
        if let q = rest.firstIndex(of: "?") {
            idPart = String(rest[..<q])
            let qs = String(rest[rest.index(after: q)...])
            for pair in qs.split(separator: "&") {
                let kv = pair.split(separator: "=", maxSplits: 1)
                if kv.count == 2 && kv[0] == "s" {
                    server = b64urlDecode(String(kv[1])) ?? ""
                }
            }
        }
        guard !idPart.isEmpty else { return nil }
        return (idPart, server, hasSecret)
    }

    /// b64url（无 padding）→ 明文。仅用于 server_url 这类非密字段。
    public static func b64urlDecode(_ s: String) -> String? {
        var b = s.replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        let pad = (4 - b.count % 4) % 4
        b += String(repeating: "=", count: pad)
        guard let d = Data(base64Encoded: b) else { return nil }
        return String(data: d, encoding: .utf8)
    }

    /// 配对码展示："123 456"（输入原样 6 位数字）。
    public static func codeFmt(_ code: String) -> String {
        guard code.count >= 6 else { return code }
        return String(code.prefix(3)) + " " + String(
            code.suffix(from: code.index(code.startIndex,
                                         offsetBy: 3)).prefix(3))
    }

    /// 6 位数字配对码（UI 预检；真正校验在 CPace）。
    public static func codeValid(_ code: String) -> Bool {
        code.count == 6 && code.allSatisfy { $0.isNumber }
    }

    // ---------------- 结束原因 ----------------

    /// ended 原因 → 展示文本（对齐 hb-share-page.c show_ended）。
    public static func endReason(_ reason: String?) -> String {
        switch reason ?? "stopped" {
        case "stopped":    return L("所有者已停止共享")
        case "kicked":     return L("已被移除")
        case "owner_gone": return L("所有者已离线")
        case "expired":    return L("共享已过期")
        case "locked":     return L("尝试次数过多，请让所有者重新生成配对码")
        default:           return L("共享已结束")
        }
    }

    /// http(s):// → ws(s)://（对齐 linux hb-share.c ws_url()）。
    public static func wsURL(_ server: String, _ pathQ: String) -> String {
        if server.hasPrefix("https://") {
            return "wss://" + server.dropFirst(8) + pathQ
        }
        if server.hasPrefix("http://") {
            return "ws://" + server.dropFirst(7) + pathQ
        }
        return "wss://" + server + pathQ
    }

    /// 权限标签。
    public static func permName(_ collab: Bool) -> String {
        collab ? L("可协作") : L("只读")
    }
    public static func partPermName(_ collab: Bool) -> String {
        collab ? L("可输入") : L("只读")
    }
}
