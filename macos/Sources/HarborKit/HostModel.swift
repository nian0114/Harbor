import Foundation

/// 主机对象字段（与 linux hb-store.c / hb-host-form.c 一致）。
public struct HostRec {
    public var id: String
    public var name = ""
    public var proto = "ssh"
    public var address = ""
    public var port = 22
    public var user = ""
    public var group = ""
    public var tags: [String] = []
    public var color = ""
    public var os = ""
    public var auth = "auto"
    public var keyId = ""
    public var identityFile = ""
    public var jumps: [String] = []
    public var keepalive = -1     // -1=默认30 0=关
    public var snippetId = ""
    public var lastConnected: Int64 = 0

    public init(id: String) { self.id = id }

    public var label: String { name.isEmpty ? address : name }

    /// 模糊匹配：query 的字符按序出现在 hay 中（大小写不敏感）。
    public static func fuzzy(_ q: String, _ hay: String) -> Bool {
        if q.isEmpty { return true }
        var it = hay.lowercased().makeIterator()
        for qc in q.lowercased() {
            var ok = false
            while let hc = it.next() {
                if hc == qc { ok = true; break }
            }
            if !ok { return false }
        }
        return true
    }
    /// 在 name/address/tags/group 上做 fuzzy；返回匹配强度（命中字段数）。
    public func match(_ q: String) -> Bool {
        HostRec.fuzzy(q, name) || HostRec.fuzzy(q, address)
            || tags.contains { HostRec.fuzzy(q, $0) }
            || HostRec.fuzzy(q, group)
    }
}
