import Foundation

/// 命令面板条目 + 模糊排名 —— 纯逻辑，可测。
/// 对齐 linux/src/hb-palette.c：score 0=title 前缀, 1=title 子串,
/// 2=kw 子串, 3=fuzzy 子序列；按 (score, ord) 稳定升序，取前 9。
public struct PalItem: Equatable {
    public enum Kind: Int, Equatable {
        case host, section, snippet, cmd
    }
    public var kind: Kind
    public var icon: String     // 非 host 项图标名
    public var avatar: String   // host 的 os/proto key
    public var title: String
    public var sub: String
    public var kw: String       // 小写化关键词（name/addr/user/group/tags…）
    public var id: String       // host id / section id / snippet id / cmd key
    public var ord: Int
    public var score: Int = 0

    public init(kind: Kind, icon: String = "", avatar: String = "",
                title: String, sub: String = "", kw: String = "",
                id: String, ord: Int) {
        self.kind = kind; self.icon = icon; self.avatar = avatar
        self.title = title; self.sub = sub; self.kw = kw
        self.id = id; self.ord = ord
    }
}

public enum PaletteRank {
    public static let limit = 9

    /// 子序列匹配：q 的字符是否按序散落于 s（Unicode 粒度）。
    public static func isSubseq(_ q: String, _ s: String) -> Bool {
        var it = s.makeIterator()
        var cur = it.next()
        for qc in q {
            while true {
                guard let sc = cur else { return false }
                cur = it.next()
                if sc == qc { break }
            }
        }
        return true
    }

    /// 0=title 前缀, 1=title 子串, 2=kw 子串, 3=fuzzy; -1 不匹配。
    /// q/title/kw 传入前须 lowercased()。
    public static func score(q: String, title: String, kw: String) -> Int {
        if q.isEmpty { return 0 }
        if title.hasPrefix(q) { return 0 }
        if title.contains(q) { return 1 }
        if kw.contains(q) { return 2 }
        return isSubseq(q, title + " " + kw) ? 3 : -1
    }

    /// 过滤 + 排名：小写化 q，打分丢弃 -1，(score, ord) 升序，前 9。
    public static func filter(_ items: [PalItem],
                              query raw: String) -> [PalItem] {
        let q = raw.lowercased()
            .trimmingCharacters(in: .whitespaces)
        var out: [PalItem] = []
        out.reserveCapacity(min(items.count, limit))
        for var it in items {
            let s = score(q: q,
                          title: it.title.lowercased(),
                          kw: it.kw.lowercased())
            if s < 0 { continue }
            it.score = s
            out.append(it)
            if out.count == items.count { break }
        }
        out.sort { a, b in
            a.score != b.score ? a.score < b.score : a.ord < b.ord
        }
        return Array(out.prefix(limit))
    }

    /// snippet scope 匹配（对齐 hb_snippet_scope_match）：
    /// 空/"global"/"全局" → 总是；"host:<id>" / "group:<g>" 精确。
    public static func snippetScopeMatch(_ scope: String?,
                                         hostId: String?,
                                         group: String?) -> Bool {
        guard let s = scope, !s.isEmpty,
              s != "global", s != "全局" else { return true }
        if s.hasPrefix("host:") {
            return hostId == String(s.dropFirst(5))
        }
        if s.hasPrefix("group:") {
            return !(group?.isEmpty ?? true) &&
                   group == String(s.dropFirst(6))
        }
        return true
    }
}
