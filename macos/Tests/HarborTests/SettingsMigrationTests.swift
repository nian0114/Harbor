import XCTest
@testable import HarborKit

final class SettingsMigrationTests: XCTestCase {
    func testCopiesOldToNew() {
        let out = SettingsMigration.migrateTermKeys([
            "term_theme": "ubuntu", "term_font": "Menlo",
            "term_option_meta": "1", "term_cursor": "2",
        ])
        XCTAssertEqual(out["theme"], "ubuntu")
        XCTAssertEqual(out["font_family"], "Menlo")
        XCTAssertEqual(out["option_meta"], "1")
        XCTAssertEqual(out["cursor_style"], "2")
        XCTAssertNil(out["term_theme"])
        XCTAssertNil(out["term_font"])
        XCTAssertNil(out["term_option_meta"])
        XCTAssertNil(out["term_cursor"])
    }
    func testNewKeyWins() {
        // 新键已存在：保留新值，仍删除旧键
        let out = SettingsMigration.migrateTermKeys([
            "term_theme": "ubuntu", "theme": "solarized",
        ])
        XCTAssertEqual(out["theme"], "solarized")
        XCTAssertNil(out["term_theme"])
    }
    func testAllPairsCovered() {
        XCTAssertEqual(SettingsMigration.termKeys.count, 7)
        XCTAssertEqual(Set(SettingsMigration.termKeys.map(\.new)).count, 7)
    }
    func testNoOldKeysIsNoop() {
        let d = ["theme": "harbor", "ui_theme": "dark"]
        XCTAssertEqual(SettingsMigration.migrateTermKeys(d), d)
    }
}
