import Foundation

/// 端口转发纯逻辑（对齐 linux hb-forward-mgr.c / hb-forward-form.c /
/// hb-forward-view.c 的 helper）——无 UI 依赖，供页面与测试复用。
public enum FwdKit {

    public enum Kind: String {
        case local, remote, dynamic
        /// form 下拉标签（linux kKindLbl）。
        public var label: String {
            switch self {
            case .local:   return "Local (-L)"
            case .remote:  return "Remote (-R)"
            case .dynamic: return "Dynamic SOCKS5 (-D)"
            }
        }
        /// 表格类型列（linux fmt_kind）。
        public var tag: String {
            switch self {
            case .local:   return "Local"
            case .remote:  return "Remote"
            case .dynamic: return "Dynamic"
            }
        }
        /// 表单类型说明（linux subs[]）。
        public var explain: String {
            switch self {
            case .local:
                return L("本机 绑定地址:端口 → 经 SSH 服务器访问 目标地址:端口")
            case .remote:
                return L("服务器 绑定地址:端口 → 经本机访问 目标地址:端口（需要 sshd 允许）")
            case .dynamic:
                return L("本机 SOCKS5 代理端口 → 目标由客户端逐连接指定")
            }
        }
    }

    // ---------- 重连退避（linux kBackoff）----------
    public static let backoff = [1, 2, 5, 10, 30]
    /// idx 超出表长后恒 30（linux 同）。
    public static func backoffSecs(_ idx: Int) -> Int {
        backoff[min(max(0, idx), backoff.count - 1)]
    }

    // ---------- 地址校验（linux addr_ok/is_loopback/is_wildcard）----------
    /// 数字 IPv4/IPv6 或 localhost（hb_pf_probe_bind 同规则）。
    public static func addrOk(_ s: String) -> Bool {
        if s == "localhost" { return true }
        var a4 = in_addr(); var a6 = in6_addr()
        return s.withCString { inet_pton(AF_INET, $0, &a4) == 1 ||
                                  inet_pton(AF_INET6, $0, &a6) == 1 }
    }
    public static func isLoopback(_ s: String) -> Bool {
        if s.isEmpty || s == "localhost" || s == "::1" { return true }
        return s.hasPrefix("127.")
    }
    public static func isWildcard(_ s: String) -> Bool {
        s.isEmpty || s == "0.0.0.0" || s == "::"
    }

    // ---------- 展示串 ----------
    /// 监听列：local/dynamic "addr:port"；remote "服务器 addr:port"
    /// （actual 为实际绑定端口，配 0 时显示服务端挑的端口）。
    public static func bindText(kind: Kind, addr: String, port: Int,
                                actual: Int = 0) -> String {
        let a = addr.isEmpty ? "127.0.0.1" : addr
        let p = actual > 0 ? actual : port
        return kind == .remote ? L("服务器 %@:%d").fmt(a, p)
                               : "\(a):\(p)"
    }
    /// 目标列：dynamic "SOCKS5"，其余 "addr:port"。
    public static func destText(kind: Kind, addr: String, port: Int)
        -> String {
        if kind == .dynamic { return "SOCKS5" }
        return "\(addr.isEmpty ? "?" : addr):\(port)"
    }
    /// 通知/删除确认里的规则描述（linux rule_desc）。
    public static func ruleDesc(kind: Kind, bindAddr: String, bindPort: Int,
                                destAddr: String, destPort: Int) -> String {
        let bind = "\(bindAddr.isEmpty ? "127.0.0.1" : bindAddr):\(bindPort)"
        switch kind {
        case .remote:
            return L("服务器 %@ → %@:%d").fmt(bind,
                destAddr.isEmpty ? "127.0.0.1" : destAddr, destPort)
        case .dynamic:
            return "\(bind) → SOCKS5"
        case .local:
            return "\(bind) → \(destAddr.isEmpty ? "?" : destAddr):\(destPort)"
        }
    }
    /// 流量列（linux fmt_traffic；running=false → "—"）。
    public static func trafficText(running: Bool, up: UInt64, down: UInt64,
                                   active: UInt32) -> String {
        if !running { return "—" }
        return L("↑ %@ ↓ %@ · %d 连接").fmt(
            SftpKit.fmtSize(up), SftpKit.fmtSize(down), Int(active))
    }
    /// 配置指纹：编辑重启判断（linux cfg_string，不含 enabled/autostart）。
    public static func cfgKey(kind: String, bindAddr: String, bindPort: Int,
                              destAddr: String, destPort: Int,
                              via: String) -> String {
        "\(kind)|\(bindAddr)|\(bindPort)|\(destAddr)|\(destPort)|\(via)"
    }

    // ---------- 等效 ssh 命令预览（linux refresh_cmd）----------
    /// host 为 nil → "user@host" 占位；jumps 名称逗号串或 nil。
    public static func cmdPreview(kind: Kind, bindAddr: String,
                                  bindPort: Int, destAddr: String,
                                  destPort: Int, user: String?,
                                  hostAddr: String?, hostPort: Int,
                                  jumps: String?) -> String {
        var s = "ssh "
        let ba = bindAddr.isEmpty ? "127.0.0.1" : bindAddr
        switch kind {
        case .local:
            s += String(format: "-L %@:%d:%@:%d", ba, bindPort,
                        destAddr.isEmpty ? L("目标地址") : destAddr, destPort)
        case .remote:
            s += String(format: "-R %@:%d:%@:%d", ba, bindPort,
                        destAddr.isEmpty ? "127.0.0.1" : destAddr, destPort)
        case .dynamic:
            s += String(format: "-D %@:%d", ba, bindPort)
        }
        if let jumps, !jumps.isEmpty { s += " -J \(jumps)" }
        if let hostAddr {
            if hostPort != 22 { s += String(format: " -p %d", hostPort) }
            /* 空 user → 本机用户名（linux g_get_user_name 同） */
            s += " \(user?.isEmpty == false ? user! : NSUserName())@" +
                 "\(hostAddr)"
        } else {
            s += " user@host"
        }
        return s
    }

    // ---------- 表单校验（linux validate + refresh_hints）----------
    /// 返回行内错误文案或 nil（保存阻止项）。
    public static func validate(kind: Kind, bindAddr: String, bindPort: Int,
                                destAddr: String, destPort: Int,
                                viaSet: Bool) -> String? {
        if !addrOk(bindAddr.isEmpty ? "127.0.0.1" : bindAddr) {
            return L("绑定地址必须是数字 IP 或 localhost")
        }
        if bindPort < 1 || bindPort > 65535 {
            return L("绑定端口必须在 1–65535")
        }
        if kind != .dynamic {
            if destAddr.isEmpty { return L("目标地址不能为空") }
            if destPort < 1 || destPort > 65535 {
                return L("目标端口必须在 1–65535")
            }
        }
        if !viaSet { return L("请选择经由 SSH 主机") }
        return nil
    }
    /// 非阻止提示（linux refresh_hints）：非回环/低端口/GatewayPorts。
    public static func hint(kind: Kind, bindAddr: String, bindPort: Int)
        -> String? {
        let a = bindAddr.isEmpty ? "127.0.0.1" : bindAddr
        if kind == .remote {
            if !isLoopback(a) {
                return L("需要服务器 sshd_config 设置 GatewayPorts " +
                         "yes/clientspecified，否则仅绑定回环")
            }
        } else {
            if !isLoopback(a) { return L("将对局域网开放此端口") }
            if bindPort > 0 && bindPort < 1024 {
                return L("绑定 1024 以下端口需要 root 权限")
            }
        }
        return nil
    }
}

public extension String {
    /// "…%@…%d…".fmt(a, b)：页面拼接小工具。
    func fmt(_ a: CVarArg...) -> String {
        String(format: self, arguments: a)
    }
}
