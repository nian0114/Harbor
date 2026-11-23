import XCTest
@testable import HarborKit

final class LangTests: XCTestCase {
    override func tearDown() { Lang.apply("") }
    func testEffective() {
        XCTAssertEqual(Lang.effective("en"), "en")
        XCTAssertEqual(Lang.effective("zh"), "zh")
        // 跟随系统：本测试环境的 preferredLanguages 不含中文 → en 或 zh
        // 视系统而定，只断言二值性
        let sys = Lang.effective("")
        XCTAssertTrue(sys == "en" || sys == "zh")
    }
    func testLPassthrough() {
        Lang.apply("zh")
        XCTAssertEqual(L("主机"), "主机")
        XCTAssertEqual(L("hello"), "hello")
    }
    func testTableApply() {
        Lang.apply("en")   // 测试 runner 无 bundle 表 → nil → 直通
        XCTAssertEqual(L("主机"), "主机")
    }
    func testCTranslator() {
        // 无表 → NULL（core 回退 msgid）
        Lang.apply("")
        let r = "中文".withCString { Lang.cTranslate($0) }
        XCTAssertNil(r)
    }
}
