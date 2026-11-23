import XCTest
import Foundation
@testable import HarborKit

/// WsModel 分屏树纯逻辑测试（对齐 linux hb-workspace.c 的节点语义）。
final class WsModelTests: XCTestCase {
    private func ids(_ m: WsModel) -> [UUID] {
        m.leaves.map { $0.leafId! }
    }

    func testSingleLeaf() {
        let id = UUID()
        let m = WsModel(firstId: id)
        XCTAssertEqual(m.leafCount, 1)
        XCTAssertEqual(ids(m), [id])
        XCTAssertTrue(m.focused.isLeaf)
        XCTAssertEqual(m.focused.leafId, id)
        XCTAssertTrue(m.root.isLeaf)
    }

    /// split：目标叶被内节点顶替，a=旧 b=新；水平 = 左右。
    func testSplitRight() {
        let a = UUID(), b = UUID()
        let m = WsModel(firstId: a)
        let target = m.leaf(a)!
        let inner = m.split(target: target, newLeaf: .init(leafId: b),
                            horizontal: true, frac: 0.5)
        XCTAssertNotNil(inner)
        XCTAssertEqual(m.leafCount, 2)
        XCTAssertFalse(m.root.isLeaf)
        XCTAssertTrue(m.root.horizontal)
        XCTAssertEqual(m.root.a?.leafId, a)
        XCTAssertEqual(m.root.b?.leafId, b)
        XCTAssertEqual(ids(m), [a, b])   // 中序：a 左 b 右
    }

    /// 嵌套分裂：在右侧叶上再竖分 → 左右 + 右下。
    func testNestedSplit() {
        let a = UUID(), b = UUID(), c = UUID()
        let m = WsModel(firstId: a)
        m.split(target: m.leaf(a)!, newLeaf: .init(leafId: b),
                horizontal: true)
        let inner2 = m.split(target: m.leaf(b)!, newLeaf: .init(leafId: c),
                             horizontal: false)
        XCTAssertNotNil(inner2)
        XCTAssertEqual(m.leafCount, 3)
        // root.a=a, root.b=inner2(b over c)
        XCTAssertEqual(m.root.a?.leafId, a)
        XCTAssertFalse(m.root.b!.isLeaf)
        XCTAssertFalse(m.root.b!.horizontal)
        XCTAssertEqual(m.root.b!.a?.leafId, b)
        XCTAssertEqual(m.root.b!.b?.leafId, c)
        XCTAssertEqual(ids(m), [a, b, c])
    }

    /// 满员拒绝分裂（maxPanes=6，对齐 linux 最多 6 分屏）。
    func testSplitCap() {
        let m = WsModel(firstId: UUID())
        var prev = m.leaves.first!
        for _ in 1..<6 {
            let nl = WsModel.Node(leafId: UUID())
            XCTAssertNotNil(m.split(target: prev, newLeaf: nl,
                                    horizontal: true))
            prev = nl
        }
        XCTAssertEqual(m.leafCount, 6)
        XCTAssertFalse(m.canSplit)
        XCTAssertNil(m.split(target: prev,
                             newLeaf: .init(leafId: UUID()),
                             horizontal: true))
    }

    /// 非叶不能分裂。
    func testSplitNonLeafFails() {
        let a = UUID(), b = UUID()
        let m = WsModel(firstId: a)
        m.split(target: m.leaf(a)!, newLeaf: .init(leafId: b),
                horizontal: true)
        XCTAssertNil(m.split(target: m.root,
                             newLeaf: .init(leafId: UUID()),
                             horizontal: true))
    }

    /// 摘除：兄弟顶替父节点位置（remove_leaf 提升）。
    func testRemovePromotesSibling() {
        let a = UUID(), b = UUID(), c = UUID()
        let m = WsModel(firstId: a)
        m.split(target: m.leaf(a)!, newLeaf: .init(leafId: b),
                horizontal: true)
        m.split(target: m.leaf(b)!, newLeaf: .init(leafId: c),
                horizontal: false)
        // 删 c → inner2 消失，b 顶替回 root.b
        let sib = m.removeLeaf(m.leaf(c)!)
        XCTAssertEqual(sib?.leafId, b)
        XCTAssertEqual(m.leafCount, 2)
        XCTAssertEqual(m.root.b?.leafId, b)
        XCTAssertEqual(ids(m), [a, b])
        // 删 b → a 成唯一叶 = root
        XCTAssertEqual(m.removeLeaf(m.leaf(b)!)?.leafId, a)
        XCTAssertTrue(m.root.isLeaf)
        XCTAssertEqual(m.root.leafId, a)
        XCTAssertEqual(m.leafCount, 1)
    }

    /// 根（唯一叶）不能删 —— 关 tab 语义由调用方管。
    func testRemoveRootFails() {
        let a = UUID()
        let m = WsModel(firstId: a)
        XCTAssertNil(m.removeLeaf(m.leaf(a)!))
        XCTAssertEqual(m.leafCount, 1)
    }

    /// 焦点叶被删 → 焦点落到剩余首叶（对齐 remove_leaf 的 focus 逻辑）。
    func testRemoveFocusedMovesFocus() {
        let a = UUID(), b = UUID()
        let m = WsModel(firstId: a)
        m.split(target: m.leaf(a)!, newLeaf: .init(leafId: b),
                horizontal: true)
        m.setFocus(m.leaf(b)!)
        XCTAssertEqual(m.focused.leafId, b)
        m.removeLeaf(m.leaf(b)!)
        XCTAssertEqual(m.focused.leafId, a)
    }

    /// setFocus 只认叶。
    func testSetFocusOnlyLeaf() {
        let a = UUID(), b = UUID()
        let m = WsModel(firstId: a)
        m.split(target: m.leaf(a)!, newLeaf: .init(leafId: b),
                horizontal: true)
        m.setFocus(m.root)
        XCTAssertEqual(m.focused.leafId, a)   // 内节点不可聚焦
        m.setFocus(m.leaf(b)!)
        XCTAssertEqual(m.focused.leafId, b)
    }
}
