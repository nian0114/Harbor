import Foundation

/// 工作区分屏树模型（对齐 linux hb-workspace.c 的 WsNode 树语义）：
///   叶 = 会话 pane；内节点 = 分屏容器。
///   horizontal=true  → 左右排布（"split right"，对应 GTK HORIZONTAL /
///   AppKit NSSplitView.isVertical=true）。
///   split：目标叶被内节点顶替（a=旧叶 start, b=新叶 end）。
///   removeLeaf：兄弟顶替父节点位置；焦点自动落到剩余首叶。
/// 纯模型无 AppKit 依赖，供 Harbor 渲染层与 HarborTests 单测共用。
public final class WsModel {
    public final class Node {
        public weak var parent: Node?
        public var a: Node?            // 内节点：start（左/上）子
        public var b: Node?            // 内节点：end（右/下）子
        public var horizontal = true
        public var frac: Float = 0.5   // a 侧所占比例
        public let leafId: UUID?       // 非 nil → 叶
        public var isLeaf: Bool { leafId != nil }
        public init(leafId: UUID?) { self.leafId = leafId }
    }

    public private(set) var root: Node
    public private(set) var focused: Node
    public private(set) var leafCount = 1
    public let maxPanes = 6

    public init(firstId: UUID) {
        root = Node(leafId: firstId)
        focused = root
    }

    /// 按布局顺序（中序 = 屏幕上从左到右/从上到下）列出全部叶。
    public var leaves: [Node] {
        var out: [Node] = []
        func walk(_ n: Node) {
            if n.isLeaf { out.append(n) }
            else { n.a.map(walk); n.b.map(walk) }
        }
        walk(root)
        return out
    }

    public func leaf(_ id: UUID) -> Node? {
        leaves.first { $0.leafId == id }
    }

    public var canSplit: Bool { leafCount < maxPanes }

    /// target 叶分裂：内节点顶替 target 的位置，a=target b=newLeaf。
    /// 返回新建的内节点；target 已满员返回 nil。
    @discardableResult
    public func split(target: Node, newLeaf: Node,
                      horizontal: Bool, frac: Float = 0.5) -> Node? {
        guard target.isLeaf, leafCount < maxPanes else { return nil }
        let inner = Node(leafId: nil)
        inner.horizontal = horizontal
        inner.frac = frac
        inner.parent = target.parent
        inner.a = target
        inner.b = newLeaf
        target.parent = inner
        newLeaf.parent = inner
        if let gp = inner.parent {
            if gp.a === target { gp.a = inner } else { gp.b = inner }
        } else {
            root = inner
        }
        leafCount += 1
        return inner
    }

    /// 摘除叶子：兄弟顶替父节点的位置（对齐 remove_leaf）。
    /// n 是根（唯一叶）→ 不删（由调用方决定关 tab）。返回被提升的兄弟，
    /// 或 nil（n 非叶/不存在）。
    @discardableResult
    public func removeLeaf(_ n: Node) -> Node? {
        guard n.isLeaf, let pn = n.parent else { return nil }
        let sib = (pn.a === n) ? pn.b : pn.a
        guard let sib else { return nil }
        let gp = pn.parent
        sib.parent = gp
        if let gp {
            if gp.a === pn { gp.a = sib } else { gp.b = sib }
        } else {
            root = sib
        }
        leafCount -= 1
        if focused === n, let first = leaves.first { focused = first }
        return sib
    }

    public func setFocus(_ n: Node) {
        if n.isLeaf { focused = n }
    }
}
