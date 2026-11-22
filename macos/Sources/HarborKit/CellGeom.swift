import Foundation
import CoreGraphics

/// 终端网格几何（纯逻辑，可测试）。坐标系 = isFlipped 视图：
/// row 0 在顶部，row/col → 点/rect 都含内边距。宽字符占 2 格。
public struct CellGeom {
    public var cellW: CGFloat
    public var cellH: CGFloat
    public var padX: CGFloat
    public var padY: CGFloat
    /// 字体 ascent + 行内居中偏移；CTLine 基线 = rowTop + baseline。
    public var baseline: CGFloat

    public init(cellW: CGFloat, cellH: CGFloat,
                padX: CGFloat, padY: CGFloat, baseline: CGFloat) {
        self.cellW = cellW; self.cellH = cellH
        self.padX = padX; self.padY = padY; self.baseline = baseline
    }

    /// 视图尺寸 → 可见行列数
    public func rows(in viewH: CGFloat) -> Int {
        max(1, Int((viewH - 2*padY) / cellH))
    }
    public func cols(in viewW: CGFloat) -> Int {
        max(1, Int((viewW - 2*padX) / cellW))
    }
    /// 行 0 顶 = padY；每行严格递增 cellH
    public func rowTop(_ row: Int) -> CGFloat { padY + CGFloat(row) * cellH }
    public func rowRect(_ row: Int, cols: Int) -> CGRect {
        CGRect(x: padX, y: rowTop(row),
               width: CGFloat(cols) * cellW, height: cellH)
    }
    public func cellRect(row: Int, col: Int, width: Int = 1) -> CGRect {
        CGRect(x: padX + CGFloat(col) * cellW, y: rowTop(row),
               width: CGFloat(width) * cellW, height: cellH)
    }
    public func baselineY(row: Int) -> CGFloat {
        rowTop(row) + baseline
    }
    /// 点 → (row,col)；列按格宽截断
    public func cellAt(_ p: CGPoint) -> (row: Int, col: Int) {
        (Int((p.y - padY) / cellH), Int((p.x - padX) / cellW))
    }
}
