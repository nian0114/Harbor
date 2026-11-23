import AppKit
import HarborKit

/// M8：NSStatusItem —— linux hb-tray.c 的 macOS 对应物。
/// 常驻菜单栏：在线会话数 + 显示 Harbor / 全部断开 / 退出。
final class MenuBar: NSObject, NSMenuDelegate {
    static let shared = MenuBar()
    private var item: NSStatusItem?
    private var countItem: NSMenuItem?
    private var tick: Timer?

    func install() {
        guard item == nil else { return }
        let it = NSStatusBar.system
            .statusItem(withLength: NSStatusItem.variableLength)
        it.button?.image = MenuBar.statusImage()
        let m = NSMenu()
        m.delegate = self
        let cnt = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        cnt.isEnabled = false
        m.addItem(cnt)
        m.addItem(.separator())
        for (t, sel) in [
            (L("显示 Harbor"), #selector(showApp)),
            (L("全部断开"), #selector(disconnectAll)),
            (L("退出"), #selector(quitApp)),
        ] as [(String, Selector)] {
            let i = NSMenuItem(title: t, action: sel, keyEquivalent: "")
            i.target = self
            m.addItem(i)
        }
        it.menu = m
        item = it
        countItem = cnt
        refresh()
        // linux 由会话事件驱动托盘计数；这里 2s 轮询足够且无事件接点。
        tick = Timer.scheduledTimer(withTimeInterval: 2, repeats: true)
        { [weak self] _ in self?.refresh() }
    }

    /// linux tray_count_online：terminals + sftp + desktops + 运行中转发。
    static func onlineCount(_ win: MainWindow?) -> Int {
        var n = 0
        for t in win?.tabs ?? [] {
            n += t.onlineCount
        }
        return n + ForwardMgr.shared.runningCount()
    }
    func refresh() {
        let n = Self.onlineCount(
            (NSApp.delegate as? App)?.win)
        countItem?.title = String(format: L("%d 个会话在线"), n)
        SleepPolicy.shared.refreshAssertion()
    }
    func menuWillOpen(_ m: NSMenu) { refresh() }

    /// 模板状态图标：应用图标的单色版（IconGen 生成的 statusIcon
    /// 资源；缺失时退回手绘 >_ 轮廓）。
    static func statusImage() -> NSImage {
        if let img = Bundle.main
            .image(forResource: NSImage.Name("statusIcon")) {
            img.isTemplate = true
            return img
        }
        let s: CGFloat = 18
        let img = NSImage(size: NSSize(width: s, height: s))
        img.lockFocus()
        NSColor.black.set()
        let ch = NSBezierPath()
        ch.move(to: NSPoint(x: 3, y: 14))
        ch.line(to: NSPoint(x: 8.5, y: 9))
        ch.line(to: NSPoint(x: 3, y: 4))
        ch.lineWidth = 2.2
        ch.lineCapStyle = .round
        ch.lineJoinStyle = .round
        ch.stroke()
        NSBezierPath(roundedRect: NSRect(x: 10.5, y: 3, width: 5.5,
                                         height: 2.2),
                     xRadius: 1.1, yRadius: 1.1).fill()
        img.unlockFocus()
        img.isTemplate = true
        return img
    }

    @objc private func showApp() {
        (NSApp.delegate as? App)?.showMainWindow()
    }
    /// linux hb_window_disconnect_all + stop_all_forwards：
    /// 会话断开（tab 保留，日志正常收尾），转发停。
    @objc private func disconnectAll() {
        guard let app = NSApp.delegate as? App else { return }
        for t in app.win?.tabs ?? [] { t.disconnect() }
        ForwardMgr.shared.stopAll()
        refresh()
    }
    @objc private func quitApp() { NSApp.terminate(nil) }

    // ---------- dev ----------
    var devMenuItems: [String] {
        item?.menu?.items.map { $0.title } ?? []
    }
    /// dev：按 index 触发菜单项（与点击同路：performActionForItem）。
    func devPerformItem(_ i: Int) -> Bool {
        guard let m = item?.menu,
              i >= 0, i < m.items.count else { return false }
        m.performActionForItem(at: i)
        return true
    }
}
