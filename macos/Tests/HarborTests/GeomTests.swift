import XCTest
import CoreGraphics
@testable import HarborKit

final class GeomTests: XCTestCase {
    let g = CellGeom(cellW: 8.5, cellH: 18, padX: 12, padY: 10,
                     baseline: 14)
    func testRowsIncrease() {
        XCTAssertLessThan(g.rowTop(0), g.rowTop(1))
        XCTAssertEqual(g.rowTop(2) - g.rowTop(1), 18)
        XCTAssertEqual(g.rowTop(0), 10)
    }
    func testCellRect() {
        let r = g.cellRect(row: 0, col: 0)
        XCTAssertEqual(r.minX, 12)
        let w = g.cellRect(row: 3, col: 5, width: 2)
        XCTAssertEqual(w.width, 17)
        XCTAssertEqual(w.minX, 12 + 5*8.5)
        XCTAssertEqual(w.minY, 10 + 3*18)
    }
    func testBaseline() {
        XCTAssertEqual(g.baselineY(row: 4), 10 + 4*18 + 14)
    }
    func testCellAt() {
        let (r, c) = g.cellAt(CGPoint(x: 12 + 3*8.5 + 1, y: 10 + 2*18 + 1))
        XCTAssertEqual(r, 2); XCTAssertEqual(c, 3)
    }
    func testFit() {
        XCTAssertEqual(g.rows(in: 10*2 + 4*18), 4)
        XCTAssertEqual(g.cols(in: 12*2 + 10*8.5), 10)
    }
}
