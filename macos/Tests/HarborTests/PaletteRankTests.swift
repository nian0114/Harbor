import XCTest
import Foundation
@testable import HarborKit

/// PaletteRank 模糊排名纯逻辑测试（对齐 linux score_item/apply_filter）。
final class PaletteRankTests: XCTestCase {
    private func item(_ kind: PalItem.Kind = .host, _ title: String,
                      kw: String = "", ord: Int = 0,
                      id: String = "") -> PalItem {
        PalItem(kind: kind, title: title, kw: kw,
                id: id.isEmpty ? title : id, ord: ord)
    }

    // ---- score ----
    func testScorePrefix() {
        XCTAssertEqual(PaletteRank.score(q: "web", title: "web-1", kw: ""),
                       0)
    }
    func testScoreInfix() {
        XCTAssertEqual(PaletteRank.score(q: "prod", title: "my prod box",
                                         kw: ""), 1)
    }
    func testScoreKwOnly() {
        XCTAssertEqual(PaletteRank.score(q: "10.0.0", title: "web",
                                         kw: "web 10.0.0.1 root"), 2)
    }
    func testScoreFuzzy() {
        // "wb1" 散落于 "web-1" → fuzzy(3)
        XCTAssertEqual(PaletteRank.score(q: "wb1", title: "web-1", kw: ""),
                       3)
    }
    func testScoreMiss() {
        XCTAssertEqual(PaletteRank.score(q: "zzz", title: "web", kw: "x"),
                       -1)
    }
    func testScoreEmptyQuery() {
        XCTAssertEqual(PaletteRank.score(q: "", title: "x", kw: ""), 0)
    }

    // ---- 子序列 ----
    func testSubseq() {
        XCTAssertTrue(PaletteRank.isSubseq("wb", "web box"))
        XCTAssertFalse(PaletteRank.isSubseq("bw", "web box"))
        XCTAssertTrue(PaletteRank.isSubseq("", "anything"))
    }

    // ---- filter：score 升序、同分按 ord 稳定、上限 9 ----
    func testFilterOrdering() {
        let items = [
            item(.cmd, "zz fuzzy", kw: "", ord: 0, id: "c1"),
            item(.host, "prod-web", ord: 1),
            item(.host, "web", ord: 2),
            item(.host, "other", kw: "has web inside", ord: 3),
        ]
        // "web"：title 前缀(0) > title 子串(1) > kw 子串(2)
        let r = PaletteRank.filter(items, query: "web")
        XCTAssertEqual(r.map { $0.id },
                       ["web", "prod-web", "other"])
        let kinds = r.map { $0.score }
        XCTAssertEqual(kinds, kinds.sorted())
    }
    func testFilterStable() {
        // 同 score → ord 升序
        let items = [
            item(.host, "web-b", ord: 5),
            item(.host, "web-a", ord: 1),
        ]
        let r = PaletteRank.filter(items, query: "web")
        XCTAssertEqual(r.map { $0.ord }, [1, 5])
    }
    func testFilterLimit9() {
        let items = (0..<20).map {
            item(.host, "web-\($0)", ord: $0)
        }
        XCTAssertEqual(PaletteRank.filter(items, query: "web").count, 9)
    }
    func testFilterCaseInsensitive() {
        let items = [item(.host, "Prod-WEB", ord: 0)]
        XCTAssertEqual(PaletteRank.filter(items, query: "web").count, 1)
    }

    // ---- snippet scope ----
    func testScopeMatch() {
        XCTAssertTrue(PaletteRank.snippetScopeMatch(
            nil, hostId: "h1", group: "g1"))
        XCTAssertTrue(PaletteRank.snippetScopeMatch(
            "global", hostId: nil, group: nil))
        XCTAssertTrue(PaletteRank.snippetScopeMatch(
            "全局", hostId: nil, group: nil))
        XCTAssertTrue(PaletteRank.snippetScopeMatch(
            "host:h1", hostId: "h1", group: nil))
        XCTAssertFalse(PaletteRank.snippetScopeMatch(
            "host:h1", hostId: "h2", group: nil))
        XCTAssertFalse(PaletteRank.snippetScopeMatch(
            "host:h1", hostId: nil, group: nil))
        XCTAssertTrue(PaletteRank.snippetScopeMatch(
            "group:g1", hostId: nil, group: "g1"))
        XCTAssertFalse(PaletteRank.snippetScopeMatch(
            "group:g1", hostId: nil, group: nil))
        XCTAssertFalse(PaletteRank.snippetScopeMatch(
            "group:g1", hostId: nil, group: "g2"))
        // 未知 scope 前缀 → 放行（对齐 linux 默认 TRUE）
        XCTAssertTrue(PaletteRank.snippetScopeMatch(
            "weird", hostId: nil, group: nil))
    }
}
