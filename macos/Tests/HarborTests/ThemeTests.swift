import XCTest
import CoreGraphics
@testable import HarborKit

final class ThemeTests: XCTestCase {
    func testPaletteSize() {
        XCTAssertEqual(Theme.palette256.count, 256)
    }
    func testAnsi() {
        XCTAssertEqual(Theme.palette256[1].r, 0xcc/255.0, accuracy: 0.001)
        XCTAssertEqual(Theme.palette256[15].r, 0xee/255.0, accuracy: 0.001)
    }
    func testCubeAndGray() {
        XCTAssertEqual(Theme.palette256[16].r, 0)
        XCTAssertEqual(Theme.palette256[231].b, 1.0, accuracy: 0.001)
        XCTAssertEqual(Theme.palette256[232].r, 8/255.0, accuracy: 0.001)
        XCTAssertEqual(Theme.palette256[255].r, 238/255.0, accuracy: 0.001)
    }
    func testHex() {
        let c = Theme.RGBA(0x2f7cf6)
        XCTAssertEqual(c.b, 0xf6/255.0, accuracy: 0.001)
    }
    /// FR-15：暗色 token 表与 linux/data/theme-dark.css 一一对应——
    /// 每个亮色 token 必须有暗色值，无多余键。
    func testDarkTokenParity() {
        XCTAssertEqual(Set(Theme.light.keys), Set(Theme.dark.keys))
    }
    /// ui_theme: mode=dark 直接走暗色表。
    func testModeDarkLookup() {
        Theme.mode = .dark
        defer { Theme.mode = .light }
        XCTAssertEqual(Theme.c("text").r, Theme.dark["text"]!.r,
                       accuracy: 0.001)
    }
}


