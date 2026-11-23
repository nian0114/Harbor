import XCTest
import Foundation
@testable import HarborKit

/// M2b 纯逻辑：{{var}} 解析/渲染（core hb_snippet_* 封装）、
/// 日志 result/reason/duration 映射、known hashed 主机名展示。
final class M2bFormatTests: XCTestCase {

    // ---- SnipKit：{{var}} ----
    func testVarsSimple() {
        let vs = SnipKit.vars(of: "ls {{path}}")
        XCTAssertEqual(vs.count, 1)
        XCTAssertEqual(vs[0].name, "path")
        XCTAssertNil(vs[0].def)
    }
    func testVarsDefault() {
        let vs = SnipKit.vars(of: "tar -czf {{out=backup.tgz}} {{dir}}")
        XCTAssertEqual(vs.map(\.name), ["out", "dir"])
        XCTAssertEqual(vs[0].def, "backup.tgz")
        XCTAssertNil(vs[1].def)
    }
    func testVarsDedupe() {
        let vs = SnipKit.vars(of: "{{a}} {{a}} {{b}}")
        XCTAssertEqual(vs.map(\.name), ["a", "b"])
    }
    func testVarsNone() {
        XCTAssertTrue(SnipKit.vars(of: "uptime").isEmpty)
        XCTAssertTrue(SnipKit.vars(of: "echo { { x } }").isEmpty)
    }
    func testRenderSubstitute() {
        XCTAssertEqual(SnipKit.render("ls {{path}}",
                                      values: ["path": "/tmp/x"]),
                       "ls /tmp/x")
    }
    func testRenderDefaultFallback() {
        // 空值 → core 回退默认
        XCTAssertEqual(SnipKit.render("tar {{out=b.tgz}}",
                                      values: [:]),
                       "tar b.tgz")
        XCTAssertEqual(SnipKit.render("tar {{out=b.tgz}}",
                                      values: ["out": ""]),
                       "tar b.tgz")
    }
    func testRenderMulti() {
        XCTAssertEqual(
            SnipKit.render("scp {{f}} {{u}}@{{h}}:{{d=/tmp}}",
                           values: ["f": "a", "u": "root", "h": "h1"]),
            "scp a root@h1:/tmp")
    }
    func testScopeLabel() {
        XCTAssertEqual(SnipKit.scopeLabel("global") { _ in nil }, L("全局"))
        XCTAssertEqual(SnipKit.scopeLabel("host:abc") {
            $0 == "abc" ? "web1" : nil }, "web1")
        XCTAssertEqual(SnipKit.scopeLabel("host:x") { _ in nil }, "x")
        XCTAssertEqual(SnipKit.scopeLabel("group:prod") { _ in nil },
                       "prod")
    }

    // ---- LogFormat ----
    func testResultClasses() {
        XCTAssertEqual(LogFormat.result("", durationS: -1).0, L("进行中"))
        XCTAssertEqual(LogFormat.result("connecting", durationS: 3).0,
                       L("进行中"))
        XCTAssertEqual(LogFormat.result("ok", durationS: 3).0, L("成功"))
        XCTAssertEqual(LogFormat.result("cancelled", durationS: 3).0,
                       L("已取消"))
        XCTAssertEqual(LogFormat.result("auth", durationS: 3).0, L("失败"))
    }
    func testReasonMap() {
        XCTAssertEqual(LogFormat.reason("auth_failed", result: "x"),
                       L("认证失败"))
        XCTAssertEqual(LogFormat.reason("unreachable", result: "x"),
                       L("无法连接"))
        XCTAssertEqual(LogFormat.reason("hostkey_changed", result: "x"),
                       L("主机密钥变更"))
        XCTAssertEqual(LogFormat.reason("timeout", result: "x"), L("超时"))
        // reason 空 → 回退 result
        XCTAssertEqual(LogFormat.reason("", result: "cancelled"),
                       L("已取消"))
        // 未识别 → 原文
        XCTAssertEqual(LogFormat.reason("weird", result: "x"), "weird")
        XCTAssertEqual(LogFormat.reason("ok", result: "ok"), "")
    }
    func testDuration() {
        XCTAssertEqual(LogFormat.duration(-1), L("进行中"))
        XCTAssertEqual(LogFormat.duration(7), "7s")
        XCTAssertEqual(LogFormat.duration(75), "1m 15s")
    }

    // ---- KnownFormat ----
    func testHostDisp() {
        XCTAssertEqual(KnownFormat.hostDisp("example.com"), "example.com")
        let h = "|1|salt==|hash=="
        let d = KnownFormat.hostDisp(h)
        XCTAssertTrue(d.hasPrefix(L("（hashed）") + "|1|"))
        XCTAssertTrue(d.hasSuffix("…"))
        XCTAssertEqual(d, L("（hashed）") + String(h.prefix(12)) + "…")
    }
}
