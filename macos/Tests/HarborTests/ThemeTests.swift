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
}
