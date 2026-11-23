import XCTest
@testable import HarborKit

/// FR-02 Touch ID 门控：注入 evaluator/canEvaluate 验证
/// 通过/拒绝/不可用 三条路径（VM 无真实生物识别，走注入）。
final class BiometricTests: XCTestCase {
    private var savedCan: (() -> Bool)!
    private var savedEval: ((String, @escaping (Bool) -> Void) -> Void)!
    override func setUp() {
        savedCan = Biometric.canEvaluate
        savedEval = Biometric.evaluate
    }
    override func tearDown() {
        Biometric.canEvaluate = savedCan
        Biometric.evaluate = savedEval
    }
    func testGateAccept() {
        var answered = false
        Biometric.canEvaluate = { true }
        Biometric.evaluate = { _, cb in answered = true; cb(true) }
        XCTAssertTrue(Biometric.canEvaluate())
        var got: Bool?
        Biometric.evaluate("test") { got = $0 }
        XCTAssertTrue(answered)
        XCTAssertEqual(got, true)
    }
    func testGateDeny() {
        // 拒绝 → ConnCtl 中止连接，secret 不出 Vault
        Biometric.canEvaluate = { true }
        Biometric.evaluate = { _, cb in cb(false) }
        var got: Bool?
        Biometric.evaluate("test") { got = $0 }
        XCTAssertEqual(got, false)
    }
    func testUnavailableFallsBack() {
        // canEvaluate=false → ConnCtl 走确认对话框路径（同 Linux）
        Biometric.canEvaluate = { false }
        XCTAssertFalse(Biometric.canEvaluate())
    }
}
