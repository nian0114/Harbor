import XCTest
import CoreGraphics
@testable import HarborKit

final class TermThemeTests: XCTestCase {
    func testIds() {
        XCTAssertEqual(TermTheme.all.count, 4)
        XCTAssertEqual(TermTheme.for("ubuntu").id, "ubuntu")
        XCTAssertEqual(TermTheme.for("nope").id, "harbor")
        XCTAssertEqual(TermTheme.for(nil).id, "harbor")
    }
    func testAnsi() {
        let t = TermTheme.for("ubuntu")
        XCTAssertEqual(t.ansi.count, 16)
        XCTAssertEqual(t.ansi[1], Theme.RGBA(0xcc0000))
    }
    func testIndexed() {
        let t = TermTheme.for("harbor")
        // idx<16 → theme ansi；≥16 → 256 板
        XCTAssertEqual(t.color(2, idx: 1, r:0,g:0,b:0, def: t.fg),
                       t.ansi[1])
        XCTAssertEqual(t.color(2, idx: 200, r:0,g:0,b:0, def: t.fg),
                       Theme.palette256[200])
    }
}
