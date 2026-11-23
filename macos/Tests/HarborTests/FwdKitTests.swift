import XCTest
@testable import HarborKit

/// FR-06 纯逻辑测试：ssh 命令预览、重连退避、规则校验
///（对齐 linux hb-forward-form.c refresh_cmd / hb-forward-mgr.c
/// kBackoff / validate）。
final class FwdKitTests: XCTestCase {

    // ---------- 命令预览（linux refresh_cmd）----------
    func testPreviewLocal() {
        let c = FwdKit.cmdPreview(kind: .local,
            bindAddr: "127.0.0.1", bindPort: 13306,
            destAddr: "db.internal", destPort: 3306,
            user: "root", hostAddr: "srv", hostPort: 22,
            jumps: nil)
        XCTAssertEqual(c,
            "ssh -L 127.0.0.1:13306:db.internal:3306 root@srv")
    }
    func testPreviewRemote() {
        let c = FwdKit.cmdPreview(kind: .remote,
            bindAddr: "0.0.0.0", bindPort: 8443,
            destAddr: "127.0.0.1", destPort: 443,
            user: "u", hostAddr: "h", hostPort: 2222,
            jumps: nil)
        /* 非默认端口 → -p */
        XCTAssertEqual(c,
            "ssh -R 0.0.0.0:8443:127.0.0.1:443 -p 2222 u@h")
    }
    func testPreviewDynamic() {
        let c = FwdKit.cmdPreview(kind: .dynamic,
            bindAddr: "", bindPort: 1080,
            destAddr: "ignored", destPort: 9999,
            user: "u", hostAddr: "h", hostPort: 22,
            jumps: nil)
        /* 空 bind → 127.0.0.1；dynamic 无目标段 */
        XCTAssertEqual(c, "ssh -D 127.0.0.1:1080 u@h")
    }
    func testPreviewJumps() {
        let c = FwdKit.cmdPreview(kind: .local,
            bindAddr: "127.0.0.1", bindPort: 1,
            destAddr: "d", destPort: 2,
            user: "u", hostAddr: "h", hostPort: 22,
            jumps: "jump1,jump2")
        XCTAssertEqual(c,
            "ssh -L 127.0.0.1:1:d:2 -J jump1,jump2 u@h")
    }
    func testPreviewJumpsAndPort() {
        let c = FwdKit.cmdPreview(kind: .local,
            bindAddr: "127.0.0.1", bindPort: 1,
            destAddr: "d", destPort: 2,
            user: "u", hostAddr: "h", hostPort: 2200,
            jumps: "j")
        XCTAssertEqual(c,
            "ssh -L 127.0.0.1:1:d:2 -J j -p 2200 u@h")
    }
    func testPreviewNoHost() {
        let c = FwdKit.cmdPreview(kind: .local,
            bindAddr: "127.0.0.1", bindPort: 1,
            destAddr: "d", destPort: 2,
            user: nil, hostAddr: nil, hostPort: 22,
            jumps: nil)
        XCTAssertEqual(c, "ssh -L 127.0.0.1:1:d:2 user@host")
    }
    func testPreviewRemoteDestDefault() {
        /* remote 空目标 → 127.0.0.1 占位（linux 同） */
        let c = FwdKit.cmdPreview(kind: .remote,
            bindAddr: "0.0.0.0", bindPort: 8443,
            destAddr: "", destPort: 443,
            user: "u", hostAddr: "h", hostPort: 22,
            jumps: nil)
        XCTAssertEqual(c,
            "ssh -R 0.0.0.0:8443:127.0.0.1:443 u@h")
    }

    // ---------- 重连退避（linux kBackoff）----------
    func testBackoffSchedule() {
        XCTAssertEqual(FwdKit.backoffSecs(0), 1)
        XCTAssertEqual(FwdKit.backoffSecs(1), 2)
        XCTAssertEqual(FwdKit.backoffSecs(2), 5)
        XCTAssertEqual(FwdKit.backoffSecs(3), 10)
        XCTAssertEqual(FwdKit.backoffSecs(4), 30)
        /* 超出表长恒 30（linux min(idx, len-1)） */
        XCTAssertEqual(FwdKit.backoffSecs(5), 30)
        XCTAssertEqual(FwdKit.backoffSecs(99), 30)
    }

    // ---------- 规则校验（linux validate）----------
    private func valid(_ k: FwdKit.Kind = .local,
                       _ ba: String = "127.0.0.1",
                       _ bp: Int = 13306,
                       _ da: String = "db",
                       _ dp: Int = 3306,
                       via: Bool = true) -> String? {
        FwdKit.validate(kind: k, bindAddr: ba, bindPort: bp,
                        destAddr: da, destPort: dp, viaSet: via)
    }
    func testValidateOk() {
        XCTAssertNil(valid())
        XCTAssertNil(valid(.remote, "0.0.0.0", 8443, "127.0.0.1", 443))
        /* dynamic 不校验目标 */
        XCTAssertNil(valid(.dynamic, "127.0.0.1", 1080, "", 0))
    }
    func testValidateBindAddr() {
        XCTAssertNotNil(valid(.local, "not-an-ip", 1, "d", 2))
        XCTAssertNil(valid(.local, "localhost", 1, "d", 2))
        XCTAssertNil(valid(.local, "::1", 1, "d", 2))
        XCTAssertNil(valid(.local, "10.0.0.5", 1, "d", 2))
    }
    func testValidatePorts() {
        XCTAssertNotNil(valid(.local, "127.0.0.1", 0, "d", 2))
        XCTAssertNotNil(valid(.local, "127.0.0.1", 65536, "d", 2))
        XCTAssertNotNil(valid(.local, "127.0.0.1", 1, "d", 0))
        XCTAssertNotNil(valid(.local, "127.0.0.1", 1, "d", 65536))
    }
    func testValidateDest() {
        XCTAssertNotNil(valid(.local, "127.0.0.1", 1, "", 2))
        XCTAssertNil(valid(.local, "127.0.0.1", 1, "x", 2))
    }
    func testValidateVia() {
        XCTAssertNotNil(valid(.local, "127.0.0.1", 1, "d", 2,
                              via: false))
    }

    // ---------- 提示（linux refresh_hints，非阻止）----------
    func testHints() {
        /* remote 非回环 → GatewayPorts */
        XCTAssertTrue(FwdKit.hint(kind: .remote,
            bindAddr: "0.0.0.0", bindPort: 8443)!
            .contains("GatewayPorts"))
        XCTAssertNil(FwdKit.hint(kind: .remote,
            bindAddr: "127.0.0.1", bindPort: 8443))
        /* local/dynamic 非回环 → 局域网提示 */
        XCTAssertNotNil(FwdKit.hint(kind: .local,
            bindAddr: "0.0.0.0", bindPort: 8080))
        /* 低端口 → root 提示 */
        XCTAssertNotNil(FwdKit.hint(kind: .local,
            bindAddr: "127.0.0.1", bindPort: 80))
        XCTAssertNil(FwdKit.hint(kind: .local,
            bindAddr: "127.0.0.1", bindPort: 8080))
    }

    // ---------- 地址工具 ----------
    func testAddrHelpers() {
        XCTAssertTrue(FwdKit.addrOk("localhost"))
        XCTAssertTrue(FwdKit.addrOk("127.0.0.1"))
        XCTAssertTrue(FwdKit.addrOk("::1"))
        XCTAssertFalse(FwdKit.addrOk("host.name"))
        XCTAssertTrue(FwdKit.isLoopback(""))
        XCTAssertTrue(FwdKit.isLoopback("localhost"))
        XCTAssertTrue(FwdKit.isLoopback("127.0.0.9"))
        XCTAssertTrue(FwdKit.isLoopback("::1"))
        XCTAssertFalse(FwdKit.isLoopback("0.0.0.0"))
        XCTAssertTrue(FwdKit.isWildcard(""))
        XCTAssertTrue(FwdKit.isWildcard("0.0.0.0"))
        XCTAssertTrue(FwdKit.isWildcard("::"))
        XCTAssertFalse(FwdKit.isWildcard("127.0.0.1"))
    }

    // ---------- 展示串 ----------
    func testRowText() {
        XCTAssertEqual(
            FwdKit.bindText(kind: .local, addr: "", port: 8080),
            "127.0.0.1:8080")
        /* remote 配 0 → 监听时用服务端挑的实际端口 */
        XCTAssertEqual(
            FwdKit.bindText(kind: .remote, addr: "0.0.0.0", port: 0,
                            actual: 40001),
            "服务器 0.0.0.0:40001")
        XCTAssertEqual(
            FwdKit.destText(kind: .dynamic, addr: "", port: 0),
            "SOCKS5")
        XCTAssertEqual(
            FwdKit.destText(kind: .local, addr: "db", port: 3306),
            "db:3306")
        XCTAssertEqual(FwdKit.trafficText(running: false,
            up: 1, down: 2, active: 3), "—")
        let t = FwdKit.trafficText(running: true,
            up: 1024, down: 2048, active: 2)
        XCTAssertTrue(t.contains("2 连接"))
    }
    func testCfgKey() {
        let a = FwdKit.cfgKey(kind: "local", bindAddr: "127.0.0.1",
            bindPort: 1, destAddr: "d", destPort: 2, via: "h")
        XCTAssertEqual(a, "local|127.0.0.1|1|d|2|h")
        /* enabled/autostart 不在指纹内：改它们不触发重启 */
        XCTAssertEqual(a, FwdKit.cfgKey(kind: "local",
            bindAddr: "127.0.0.1", bindPort: 1, destAddr: "d",
            destPort: 2, via: "h"))
        XCTAssertNotEqual(a, FwdKit.cfgKey(kind: "local",
            bindAddr: "127.0.0.1", bindPort: 2, destAddr: "d",
            destPort: 2, via: "h"))
    }
}
