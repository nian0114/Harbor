import XCTest
@testable import HarborKit

final class SmokeTests: XCTestCase {
    func testPackageBuilds() { XCTAssertTrue(true) }
}

final class DEKPlanTests: XCTestCase {
    // 成功读链
    func testChainHit() {
        XCTAssertEqual(DEKPlan.decide(chainStatus: 0, chainHasKey: true,
                                    fileKey: false, dbExists: true,
                                    fileMode: false), .useKeychain)
    }
    // 链空 + 文件有 → 文件
    func testNotFoundFileFallback() {
        XCTAssertEqual(DEKPlan.decide(chainStatus: -25300, chainHasKey: false,
                                    fileKey: true, dbExists: true,
                                    fileMode: false), .useFile)
    }
    // 链空 + 无文件 + 无 db → 全新
    func testFreshInstall() {
        XCTAssertEqual(DEKPlan.decide(chainStatus: -25300, chainHasKey: false,
                                    fileKey: false, dbExists: false,
                                    fileMode: false), .createNew)
    }
    // 链空 + 无文件 + 有 db → 报错，禁止重建 key
    func testOrphanDb() {
        XCTAssertEqual(DEKPlan.decide(chainStatus: -25300, chainHasKey: false,
                                    fileKey: false, dbExists: true,
                                    fileMode: false), .failKeyUnavailable)
    }
    // 非 notFound 的任意状态 → 报错（ACL 拒绝/不可交互…）
    func testChainError() {
        for st: Int32 in [-25308, -25293, -128, -34018, 1] {
            XCTAssertEqual(DEKPlan.decide(chainStatus: st, chainHasKey: false,
                                        fileKey: false, dbExists: false,
                                        fileMode: false), .failKeychain(st))
        }
    }
    // 链错时即使文件/db 都没有也绝不新建
    func testChainErrorNeverCreates() {
        XCTAssertEqual(DEKPlan.decide(chainStatus: -25308, chainHasKey: false,
                                    fileKey: false, dbExists: false,
                                    fileMode: false), .failKeychain(-25308))
    }
    // success 但数据损坏：有文件走文件，无文件按新建
    func testChainBadData() {
        XCTAssertEqual(DEKPlan.decide(chainStatus: 0, chainHasKey: false,
                                    fileKey: true, dbExists: true,
                                    fileMode: false), .useFile)
        XCTAssertEqual(DEKPlan.decide(chainStatus: 0, chainHasKey: false,
                                    fileKey: false, dbExists: true,
                                    fileMode: false), .failKeyUnavailable)
        XCTAssertEqual(DEKPlan.decide(chainStatus: 0, chainHasKey: false,
                                    fileKey: false, dbExists: false,
                                    fileMode: false), .createNew)
    }
    // fileMode（DEBUG HARBOR_DEK_FILE）
    func testFileMode() {
        XCTAssertEqual(DEKPlan.decide(chainStatus: -25308, chainHasKey: false,
                                    fileKey: true, dbExists: true,
                                    fileMode: true), .useFile)
        XCTAssertEqual(DEKPlan.decide(chainStatus: -25308, chainHasKey: false,
                                    fileKey: false, dbExists: true,
                                    fileMode: true), .failKeyUnavailable)
        XCTAssertEqual(DEKPlan.decide(chainStatus: -25308, chainHasKey: false,
                                    fileKey: false, dbExists: false,
                                    fileMode: true), .createNew)
    }
    func testDescribe() {
        XCTAssertTrue(DEKPlan.describe(-25308).contains("errSecInteractionNotAllowed"))
        XCTAssertTrue(DEKPlan.describe(42).contains("42"))
    }
}

final class HostModelTests: XCTestCase {
    func testFuzzy() {
        XCTAssertTrue(HostRec.fuzzy("hw", "Hello World"))
        XCTAssertTrue(HostRec.fuzzy("", "anything"))
        XCTAssertFalse(HostRec.fuzzy("hx", "Hello"))
        XCTAssertTrue(HostRec.fuzzy("zx", "zhostx"))
    }
    func testMatch() {
        var h = HostRec(id: "1")
        h.name = "web-01"; h.address = "10.0.0.5"; h.group = "prod"
        h.tags = ["nginx", "edge"]
        XCTAssertTrue(h.match("web"))
        XCTAssertTrue(h.match("10.0"))
        XCTAssertTrue(h.match("nginx"))
        XCTAssertTrue(h.match("prod"))
        XCTAssertFalse(h.match("db"))
    }
}
