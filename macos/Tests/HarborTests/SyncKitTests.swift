import XCTest
@testable import HarborKit

/// M6 纯逻辑测试：服务器地址/回环策略、邮箱规范化、NFKC、
/// 恢复码拆分、错误 JSON/状态映射、WS 重连退避表、冲突字段判定、
/// 设备列表解析（对齐 linux/src/hb-sync.c + docs/api/sync-protocol.md）。
final class SyncKitTests: XCTestCase {

    // ---------- checkServer：仅 https 或回环 http ----------
    func testCheckServerHttpLoopback() {
        XCTAssertEqual(SyncKit.checkServer("http://127.0.0.1:8787"),
                       "http://127.0.0.1:8787")
        XCTAssertEqual(SyncKit.checkServer(" http://localhost:8787 "),
                       "http://localhost:8787")
        XCTAssertEqual(SyncKit.checkServer("http://[::1]:8787"),
                       "http://[::1]:8787")
    }
    func testCheckServerHttpNonLoopbackRejected() {
        // 非回环明文 http 必须拒绝（sync-protocol：HTTP 仅回环）
        XCTAssertNil(SyncKit.checkServer("http://192.168.1.5:8787"))
        XCTAssertNil(SyncKit.checkServer("http://example.com"))
        XCTAssertNil(SyncKit.checkServer("http://10.0.0.1:8787"))
    }
    func testCheckServerHttps() {
        XCTAssertEqual(SyncKit.checkServer("https://sync.example.com"),
                       "https://sync.example.com")
        XCTAssertEqual(SyncKit.checkServer("https://sync.example.com:8443"),
                       "https://sync.example.com:8443")
        // 非 "/" 路径保留
        XCTAssertEqual(
            SyncKit.checkServer("https://h.example.com/sync"),
            "https://h.example.com/sync")
        // "/" 路径丢弃
        XCTAssertEqual(SyncKit.checkServer("https://h.example.com/"),
                       "https://h.example.com")
    }
    func testCheckServerBad() {
        XCTAssertNil(SyncKit.checkServer(""))
        XCTAssertNil(SyncKit.checkServer("   "))
        XCTAssertNil(SyncKit.checkServer("ftp://x"))
        XCTAssertNil(SyncKit.checkServer("ws://127.0.0.1"))
        XCTAssertNil(SyncKit.checkServer("notaurl"))
        XCTAssertNil(SyncKit.checkServer("http://"))
    }

    // ---------- normEmail / nfkc ----------
    func testNormEmail() {
        XCTAssertEqual(SyncKit.normEmail("  A@B.com  "), "a@b.com")
        XCTAssertNil(SyncKit.normEmail("   "))
        XCTAssertNil(SyncKit.normEmail(""))
    }
    func testNfkc() {
        // 全角字符 → 半角（NFKC）
        XCTAssertEqual(SyncKit.nfkc("ＡＢＣ"), "ABC")
        XCTAssertEqual(SyncKit.nfkc("abc"), "abc")
    }

    // ---------- splitWords ----------
    func testSplitWords() {
        let w = SyncKit.splitWords(
            (0..<24).map { "w\($0)" }.joined(separator: " "))
        XCTAssertEqual(w.count, 24)
        XCTAssertEqual(w[0], "w0")
        XCTAssertEqual(w[23], "w23")
        XCTAssertEqual(SyncKit.splitWords("a  b\n c"), ["a", "b", "c"])
    }

    // ---------- WS 重连退避（对齐 linux kWSRetry）----------
    func testWsRetryTable() {
        XCTAssertEqual(SyncKit.wsRetry, [1, 2, 4, 8, 15, 30])
    }

    // ---------- 错误 JSON 解析（{"error":{code,message}}）----------
    func testParseErr() {
        let j = """
        {"error":{"code":"invalid_credentials","message":"bad"}}
        """
        let e = SyncKit.parseErr(j)
        XCTAssertEqual(e?.code, "invalid_credentials")
        XCTAssertEqual(e?.msg, "bad")
        let t = SyncKit.parseErr(
            "{\"error\":{\"code\":\"totp_required\"}}")
        XCTAssertEqual(t?.code, "totp_required")
        XCTAssertEqual(t?.msg, L("需要两步验证码"))
        XCTAssertNil(SyncKit.parseErr("{}"))
        XCTAssertNil(SyncKit.parseErr("not json"))
    }
    func testParseErrRetryAfter() {
        let e = SyncKit.parseErr(
            "{\"error\":{\"code\":\"rate_limited\"," +
            "\"retry_after\":30}}")
        XCTAssertEqual(e?.retryAfter, 30)
    }

    // ---------- HTTP 状态 → 提示 ----------
    func testHttpErrMsg() {
        XCTAssertEqual(SyncKit.httpErrMsg(401), L("邮箱或密码错误"))
        XCTAssertEqual(SyncKit.httpErrMsg(409), L("该邮箱已注册"))
        XCTAssertEqual(SyncKit.httpErrMsg(429), L("尝试过于频繁，请稍后再试"))
        XCTAssertEqual(SyncKit.httpErrMsg(500), L("服务器错误 (500)"))
    }
    func testRetryMsg() {
        XCTAssertEqual(SyncKit.retryMsg(30), L("尝试次数过多，请 30 秒后再试"))
    }

    // ---------- 冲突字段敏感判定 ----------
    func testIsSecretField() {
        XCTAssertTrue(SyncKit.isSecretField("$password"))
        XCTAssertTrue(SyncKit.isSecretField("secret_note"))
        XCTAssertTrue(SyncKit.isSecretField("private_key"))
        XCTAssertTrue(SyncKit.isSecretField("password"))
        XCTAssertFalse(SyncKit.isSecretField("name"))
        XCTAssertFalse(SyncKit.isSecretField("addr"))
    }

    // ---------- 设备列表解析 ----------
    func testParseDevices() {
        let j = """
        {"devices":[{"id":"d1","name":"m1","platform":"macos",
         "created_at":1000,"last_seen":2000,"current":true},
        {"id":"d2","name":"l1","platform":"linux",
         "created_at":1000,"last_seen":3000,"current":false}]}
        """
        let ds = SyncKit.parseDevices(j)
        XCTAssertEqual(ds.count, 2)
        XCTAssertEqual(ds[0].id, "d1")
        XCTAssertTrue(ds[0].current)
        XCTAssertEqual(ds[1].lastSeen, 3000)
        XCTAssertFalse(ds[1].current)
        XCTAssertEqual(SyncKit.parseDevices("{}").count, 0)
        XCTAssertEqual(SyncKit.parseDevices("x").count, 0)
    }

    // ---------- 冲突卡字段值（jmember_str 等价）----------
    func testMemberStr() {
        let j = "{\"name\":\"h1\",\"port\":22,\"on\":true,\"r\":1.5}"
        XCTAssertEqual(SyncKit.memberStr(j, "name"), "h1")
        XCTAssertEqual(SyncKit.memberStr(j, "port"), "22")
        XCTAssertEqual(SyncKit.memberStr(j, "on"), "true")
        XCTAssertEqual(SyncKit.memberStr(j, "r"), "1.5")
        XCTAssertNil(SyncKit.memberStr(j, "missing"))
        XCTAssertNil(SyncKit.memberStr(nil, "name"))
    }

    // ---------- 设备名/平台 ----------
    func testPlatform() {
        XCTAssertEqual(SyncKit.platform, "macos")
        XCTAssertFalse(SyncKit.deviceName().isEmpty)
    }
}
