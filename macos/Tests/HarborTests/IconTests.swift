import XCTest
import CoreGraphics
@testable import HarborKit

final class IconTests: XCTestCase {
    func testRect() {
        let ic = IconPath(svgFragment:
            "<rect x=\"3\" y=\"4\" width=\"18\" height=\"7\" rx=\"2\"/>")
        XCTAssertEqual(ic.subs.count, 1)
        XCTAssertFalse(ic.subs[0].fill)
        let b = ic.subs[0].path.boundingBox
        XCTAssertEqual(b.width, 18, accuracy: 0.1)
        XCTAssertEqual(b.height, 7, accuracy: 0.1)
    }
    func testCircleFill() {
        let ic = IconPath(svgFragment:
            "<circle cx=\"8\" cy=\"8\" r=\"1.4\" fill=\"currentColor\"/>")
        XCTAssertTrue(ic.subs[0].fill)
    }
    func testPathD() {
        let ic = IconPath(svgFragment: "<path d=\"M7 7.5h.01M7 16.5h.01\"/>")
        XCTAssertEqual(ic.subs.count, 1)
        let ic2 = IconPath(svgFragment:
            "<path d=\"M4 4h7.4v7.4H4zM12.6 4H20v7.4h-7.4z\"/>")
        let b = ic2.subs[0].path.boundingBox
        XCTAssertEqual(b.width, 16, accuracy: 0.01)
    }
    func testAllIconsParse() {
        for (name, frag) in Icons.table {
            XCTAssertFalse(IconPath(svgFragment: frag).subs.isEmpty, name)
        }
    }
}
