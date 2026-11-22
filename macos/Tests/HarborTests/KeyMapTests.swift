import XCTest
import CoreGraphics
@testable import HarborKit

final class KeyMapTests: XCTestCase {
    func testSpecials() {
        XCTAssertEqual(KeyMap.specialKey(36), KeyMap.HB_KEY_ENTER)
        XCTAssertEqual(KeyMap.specialKey(126), KeyMap.HB_KEY_UP)
        XCTAssertEqual(KeyMap.specialKey(122), KeyMap.fkey(1))
        XCTAssertEqual(KeyMap.specialKey(111), KeyMap.fkey(12))
        XCTAssertNil(KeyMap.specialKey(0)) // 'a' → unichar
    }
    func testMods() {
        XCTAssertEqual(KeyMap.mods(1 << 18, optionIsMeta: true),
                       KeyMap.HB_MOD_CTRL)
        XCTAssertEqual(KeyMap.mods(1 << 19, optionIsMeta: true),
                       KeyMap.HB_MOD_ALT)
        XCTAssertEqual(KeyMap.mods(1 << 19, optionIsMeta: false), 0)
        XCTAssertEqual(KeyMap.mods((1 << 17) | (1 << 18), optionIsMeta: false),
                       KeyMap.HB_MOD_SHIFT | KeyMap.HB_MOD_CTRL)
    }
}
