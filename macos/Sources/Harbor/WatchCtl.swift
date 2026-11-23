import AppKit
import CHarbor
import HarborKit
import Darwin

/// 完成监控（FR-13）：hb_session_set_watch 装配 + 命中路由 +
/// 设置对话框（信号/规则/免打扰）+ 规则测试器 + OSC 133 安装器。
/// 全部对齐 linux/src/hb-watch-ui.c。

// ---- settings 读写（对齐 watch_settings/set_save_*） ----
enum WatchSettings {
    static var obj: OpaquePointer? {
        guard let v = Vault.shared.vault else { return nil }
        var o: OpaquePointer?
        guard hb_vault_settings(v, &o) == HB_OK else { return nil }
        return o
    }
    static func bool_(_ key: String, _ def: Bool) -> Bool {
        guard let o = obj else { return def }
        defer { hb_obj_free(o) }
        return hb_obj_get_bool(o, key, def)
    }
    static func int(_ key: String, _ def: Int64) -> Int64 {
        guard let o = obj else { return def }
        defer { hb_obj_free(o) }
        return hb_obj_get_int(o, key, def)
    }
    static func str(_ key: String) -> String? {
        guard let o = obj else { return nil }
        defer { hb_obj_free(o) }
        return hb_obj_get_str(o, key)?.str
    }
    static func strv(_ key: String) -> [String] {
        guard let o = obj else { return [] }
        defer { hb_obj_free(o) }
        var out: UnsafeMutablePointer<UnsafePointer<CChar>?>?
        let n = hb_obj_get_strv(o, key, &out)
        var r: [String] = []
        for i in 0..<n {
            if let p = out?[i] { r.append(String(cString: p)) }
        }
        free(out)
        return r
    }
    static func save(_ key: String, bool v: Bool) {
        guard let o = obj else { return }
        hb_obj_set_bool(o, key, v)
        Vault.shared.put(o); hb_obj_free(o)
    }
    static func save(_ key: String, str v: String) {
        guard let o = obj else { return }
        hb_obj_set_str(o, key, v)
        Vault.shared.put(o); hb_obj_free(o)
    }
    static func save(_ key: String, strv v: [String]) {
        guard let o = obj else { return }
        let ptrs: [UnsafePointer<CChar>?] = v.map {
            UnsafePointer(strdup($0))
        }
        ptrs.withUnsafeBufferPointer { b in
            hb_obj_set_strv(o, key, b.baseAddress, v.count)
        }
        for p in ptrs { free(UnsafeMutablePointer(mutating: p)) }
        Vault.shared.put(o); hb_obj_free(o)
    }
}

// ---- 规则解析："name: re" | "!name: re"（POSIX ERE+ICASE 预编译校验） ----
struct WatchRule {
    var name: String
    var okRe: String?    // 成功正则
    var errRe: String?   // 失败正则
}
enum WatchRules {
    /// 单行解析（跳过逻辑由调用方管）；ERE 编译不过 → nil。
    static func parseLine(_ raw: String) -> WatchRule? {
        var line = raw.trimmingCharacters(in: .whitespaces)
        if line.isEmpty || line.hasPrefix("#") { return nil }
        var isErr = false
        if line.hasPrefix("!") {
            isErr = true
            line = String(line.dropFirst())
                .trimmingCharacters(in: .whitespaces)
        }
        guard let ci = line.firstIndex(of: ":") else { return nil }
        let name = String(line[..<ci])
            .trimmingCharacters(in: .whitespaces)
        let re = String(line[line.index(after: ci)...])
            .trimmingCharacters(in: .whitespaces)
        if name.isEmpty || re.isEmpty { return nil }
        var rx = regex_t()
        guard regcomp(&rx, re,
                      Int32(REG_EXTENDED | REG_ICASE | REG_NOSUB)) == 0
        else { return nil }
        regfree(&rx)
        return WatchRule(name: name,
                         okRe: isErr ? nil : re,
                         errRe: isErr ? re : nil)
    }
    /// 文本 → 规则数组；(rules, badLine 1-based 或 0)
    static func parseText(_ text: String) -> ([WatchRule], Int) {
        var out: [WatchRule] = []
        var bad = 0
        for (i, l) in text.components(separatedBy: "\n").enumerated() {
            let s = l.trimmingCharacters(in: .whitespaces)
            if s.isEmpty || s.hasPrefix("#") { continue }
            guard let r = parseLine(l) else { bad = i + 1; break }
            out.append(r)
        }
        if bad != 0 { return ([], bad) }
        return (out, 0)
    }
    /// 合并全局 + per-host 规则（对齐 hb_watch_rules_merged）。
    static func merged(host: OpaquePointer?) -> [WatchRule] {
        var out: [WatchRule] = []
        for l in WatchSettings.strv("watch_rules") {
            if let r = parseLine(l) { out.append(r) }
        }
        if let host {
            var sv: UnsafeMutablePointer<UnsafePointer<CChar>?>?
            let n = hb_obj_get_strv(host, "watch_rules", &sv)
            for i in 0..<n {
                if let p = sv?[i],
                   let r = parseLine(String(cString: p)) { out.append(r) }
            }
            free(sv)
        }
        return out
    }
}

/// 装配 hb_watch_cfg 并挂到 session（对齐 hb_watch_cfg_build/apply）。
enum WatchCtl {
    static let SIG_EOF: UInt32 = 1
    static let SIG_OSC133: UInt32 = 2
    static let SIG_RULES: UInt32 = 4
    static let SIG_IDLE: UInt32 = 8
    static let SIG_TERMNOTIFY: UInt32 = 16
    static let defaultSignals: UInt32 =
        SIG_EOF | SIG_OSC133 | SIG_RULES | SIG_TERMNOTIFY

    /// signals 默认值：引擎默认 + settings.watch_idle → 加 IDLE 位。
    static func leafSignals() -> UInt32 {
        var s = defaultSignals
        if WatchSettings.bool_("watch_idle", false) { s |= SIG_IDLE }
        return s
    }
    /// 默认开关：SSH → watch_default_ssh(true)；Local → _local(false)。
    static func defaultOn(isSSH: Bool) -> Bool {
        WatchSettings.bool_(isSSH ? "watch_default_ssh"
                                  : "watch_default_local", isSSH)
    }

    /// 构建 cfg（调用方用完须 hb_watch_cfg_free_swift）。
    static func buildCfg(hostId: String?,
                         signals: UInt32) -> UnsafeMutablePointer<hb_watch_cfg>? {
        var host: OpaquePointer?
        if let hostId { host = Vault.shared.get(hostId) }
        defer { host.map { hb_obj_free($0) } }
        let rules = WatchRules.merged(host: host)
        let c = UnsafeMutablePointer<hb_watch_cfg>.allocate(capacity: 1)
        hb_watch_cfg_defaults(c)
        c.pointee.signals = signals
        c.pointee.min_duration_ms =
            Int32(WatchSettings.int("watch_min_duration_s", 5)) * 1000
        c.pointee.idle_active_ms =
            Int32(WatchSettings.int("watch_idle_n", 10)) * 1000
        c.pointee.idle_quiet_ms =
            Int32(WatchSettings.int("watch_idle_m", 5)) * 1000
        if !rules.isEmpty {
            let arr = UnsafeMutablePointer<hb_watch_rule>
                .allocate(capacity: rules.count)
            var names: [UnsafeMutablePointer<CChar>?] = []
            var oks: [UnsafeMutablePointer<CChar>?] = []
            var errs: [UnsafeMutablePointer<CChar>?] = []
            for (i, r) in rules.enumerated() {
                names.append(strdup(r.name))
                oks.append(r.okRe.map { strdup($0) } ?? nil)
                errs.append(r.errRe.map { strdup($0) } ?? nil)
                arr[i] = hb_watch_rule(name: names[i],
                                       ok_re: oks[i], err_re: errs[i])
            }
            c.pointee.rules = UnsafePointer(arr)
            c.pointee.nrules = rules.count
        }
        return c
    }
    /// buildCfg 的对称释放（深拷贝发生在 set_watch 内，所以可立即释放）。
    static func freeCfg(_ c: UnsafeMutablePointer<hb_watch_cfg>) {
        if let arr = c.pointee.rules {
            for i in 0..<c.pointee.nrules {
                let r = arr[i]
                free(UnsafeMutablePointer(mutating: r.name))
                free(UnsafeMutablePointer(mutating: r.ok_re))
                free(UnsafeMutablePointer(mutating: r.err_re))
            }
            arr.deallocate()
        }
        c.deallocate()
    }
    /// on=false → 摘引擎。
    static func apply(_ sess: OpaquePointer?, on: Bool,
                      hostId: String?, signals: UInt32) {
        guard let sess else { return }
        if !on { hb_session_set_watch(sess, nil); return }
        guard let c = buildCfg(hostId: hostId, signals: signals)
        else { return }
        hb_session_set_watch(sess, c)
        freeCfg(c)
    }
}

// ---- 命中路由（对齐 hb_watch_route） ----
enum WatchRoute {
    /// "HH:MM-HH:MM"（可跨零点；空=关）→ 当前处于免打扰窗口。
    static func inQuietWindow(_ spec: String?) -> Bool {
        guard let spec, !spec.isEmpty else { return false }
        let parts = spec.split(separator: "-")
        guard parts.count == 2 else { return false }
        func hm(_ s: Substring) -> Int? {
            let p = s.split(separator: ":")
            guard p.count == 2, let h = Int(p[0]), let m = Int(p[1]),
                  (0..<24).contains(h), (0..<60).contains(m)
            else { return nil }
            return h * 60 + m
        }
        guard let s = hm(parts[0]), let e = hm(parts[1]), s != e
        else { return false }
        let cal = Calendar.current
        let now = cal.component(.hour, from: Date()) * 60
                + cal.component(.minute, from: Date())
        return s < e ? (now >= s && now < e) : (now >= s || now < e)
    }

    static func fmtDuration(_ ms: Int64) -> String {
        if ms < 0 { return "" }
        if ms < 60000 {
            return String(format: L("耗时 %.1fs"), Double(ms) / 1000)
        }
        return String(format: L("耗时 %dm%02ds"),
                      Int(ms / 60000), Int((ms % 60000) / 1000))
    }

    /// leaf：命中的 pane；hit：HB_SESS_EV_WATCH 的 watch 载荷。
    static func route(leaf: TermTab, hit: hb_watch_hit) {
        // 未选中 tab 的 leaf.view 已出窗 —— 经 ws.tab 反查，不依赖视图层级
        let win = (leaf.view.window as? MainWindow)
                  ?? NSApp.windows.compactMap({ $0 as? MainWindow }).first
        let wsTab = leaf.wsLeaf?.tab
        let tabIdx = win?.tabs.firstIndex {
            $0 === wsTab || $0 === leaf
        } ?? -1
        let host = leaf.wsTitle
        let termnotify = hit.signal == HB_WATCH_SIG_TERMNOTIFY
        let isBEL = termnotify && (hit.rule?.str ?? "") == "BEL"
        let hostOnly = WatchSettings.str("notify_body") == "host"
        let fdnd = WatchSettings.bool_("watch_focus_dnd", true)
        let snd = WatchSettings.bool_("watch_sound", false)
        let dnd = WatchSettings.str("watch_dnd")

        // ---- 分类 + 文案 ----
        let kind: String
        if termnotify { kind = isBEL ? "ok" : "info" }
        else { kind = hit.ok ? "ok" : "err" }
        let title: String
        var body = ""
        if hostOnly {
            title = termnotify
                ? "\(L("终端通知")) · \(host)"
                : "\(hit.ok ? L("会话任务完成") : L("任务失败")) · \(host)"
            body = String(format: L("%@ 上的任务%@"), host,
                          termnotify ? L("有新通知")
                          : (hit.ok ? L("已完成") : L("失败")))
        } else if termnotify {
            let tt = (hit.title?.str).flatMap { $0.isEmpty ? nil : $0 }
                     ?? L("终端通知")
            title = "\(tt) · \(host)"
            body = hit.line?.str ?? ""
        } else {
            title = "\(hit.ok ? L("会话任务完成") : L("任务失败")) · \(host)"
            var b = (hit.command?.str).flatMap { $0.isEmpty ? nil : $0 }
                    ?? hit.rule?.str ?? ""
            if !hit.ok && hit.exit_code >= 0 {
                b += " · exit \(hit.exit_code)"
            }
            let dur = fmtDuration(hit.duration_ms)
            if !dur.isEmpty { b += " · \(dur)" }
            b += L(" — 可以进行下一步了")
            body = b
        }

        // ---- 通知中心 ----
        NotifyCenter.shared.post(kind: kind, title: title, body: body,
                                 tabIndex: tabIdx >= 0 ? tabIdx : nil)
        NotificationCenter.default.post(name: .init("hb-notif-changed"),
                                        object: nil)

        // ---- 角标（tab + pane-head） ----
        let dot = termnotify ? (isBEL ? "ok" : "pending")
                             : (hit.ok ? "ok" : "err")
        leaf.watchBadge = dot
        leaf.onChange?()
        let focused = win?.isKeyWindow == true &&
                      tabIdx == win?.selTab
        if focused {
            // 正在看的 tab：4s 后清角标（对齐 linux dot_clear_cb）
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                [weak leaf] in
                if leaf?.watchBadge == dot {
                    leaf?.watchBadge = nil
                    leaf?.onChange?()
                }
            }
        }

        // ---- 系统通知（M8 接缝；当前 no-op） ----
        if !((focused && fdnd) || inQuietWindow(dnd)) {
            NotifyCenter.shared.system.post(
                title: title, body: body,
                tabIndex: tabIdx >= 0 ? tabIdx : nil,
                priorityHigh: !hit.ok)
        }
        if snd { NSSound.beep() }
    }
}
