import AppKit
import CHarbor
import HarborKit

// ---------- AppDelegate + 菜单 ----------
final class App: NSObject, NSApplicationDelegate {
    var win: MainWindow!
    #if DEBUG
    var dev: DevServer?
    #endif
    func applicationDidFinishLaunching(_ n: Notification) {
        buildMenus()
        win = MainWindow()
        win.center()
        win.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        #if DEBUG
        if ProcessInfo.processInfo.environment["HARBOR_DEV_ACTIONS"] == "1" {
            dev = DevServer(win: win)
        }
        #endif
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ a: NSApplication)
        -> Bool { true }

    // 活动会话存在时，退出也要确认（windowShouldClose 只管点 X，terminate 走这里）
    func applicationShouldTerminate(_ a: NSApplication)
        -> NSApplication.TerminateReply {
        if win.tabs.contains(where: { $0.running }) {
            let al = NSAlert()
            al.messageText = L("仍有会话在运行")
            al.informativeText = L("退出 Harbor 将终止所有活动会话。")
            al.addButton(withTitle: L("退出"))
            al.addButton(withTitle: L("取消"))
            return al.runModal() == .alertFirstButtonReturn
                ? .terminateNow : .terminateCancel
        }
        return .terminateNow
    }

    private func item(_ t: String, _ a: Selector?, _ k: String,
                      _ m: NSEvent.ModifierFlags = .command,
                      id: String? = nil) -> NSMenuItem {
        let i = NSMenuItem(title: t, action: a, keyEquivalent: k)
        i.keyEquivalentModifierMask = m
        if let id { i.identifier = NSUserInterfaceItemIdentifier(id) }
        return i
    }
    func buildMenus() {
        let main = NSMenu()
        let app = NSMenuItem()
        main.addItem(app)
        let am = NSMenu(title: "Harbor")
        am.addItem(item(L("关于 Harbor"), #selector(NSApplication.orderFrontStandardAboutPanel(_:)), "", id: "about"))
        am.addItem(.separator())
        am.addItem(item(L("设置…"), #selector(showSettings), ",", id: "settings"))
        am.addItem(.separator())
        am.addItem(item(L("退出 Harbor"), #selector(NSApplication.terminate(_:)), "q", id: "quit"))
        app.submenu = am

        let file = NSMenuItem()
        main.addItem(file)
        let fm = NSMenu(title: L("文件"))
        fm.addItem(item(L("新建本地标签页"), #selector(newTab), "t", id: "new-local-tab"))
        fm.addItem(item(L("关闭标签页"), #selector(closeTab), "w", id: "close-tab"))
        file.submenu = fm

        let edit = NSMenuItem()
        main.addItem(edit)
        let em = NSMenu(title: L("编辑"))
        em.addItem(item(L("拷贝"), #selector(copySel), "c", id: "copy"))
        em.addItem(item(L("粘贴"), #selector(paste), "v", id: "paste"))
        em.addItem(item(L("全选"), #selector(selectAll), "a", id: "select-all"))
        em.addItem(.separator())
        em.addItem(item(L("查找…"), nil, "f", id: "find"))
        edit.submenu = em

        let view = NSMenuItem()
        main.addItem(view)
        let vm = NSMenu(title: L("显示"))
        vm.addItem(item(L("放大字体"), #selector(fontPlus), "+", id: "font-plus"))
        vm.addItem(item(L("缩小字体"), #selector(fontMinus), "-", id: "font-minus"))
        vm.addItem(item(L("实际大小"), #selector(fontReset), "0", id: "font-reset"))
        vm.addItem(.separator())
        vm.addItem(item(L("上一个标签页"), #selector(prevTab), "[",
                        [.command, .shift], id: "prev-tab"))
        vm.addItem(item(L("下一个标签页"), #selector(nextTab), "]",
                        [.command, .shift], id: "next-tab"))
        for i in 1...9 {
            vm.addItem(item(L("标签页 \(i)"), #selector(gotoTab(_:)),
                            "\(i)", id: "tab-\(i)"))
        }
        view.submenu = vm

        let win = NSMenuItem()
        main.addItem(win)
        let wm = NSMenu(title: L("窗口"))
        wm.addItem(item(L("最小化"), #selector(NSWindow.miniaturize(_:)), "m"))
        wm.addItem(item(L("缩放"), #selector(NSWindow.zoom(_:)), ""))
        win.submenu = wm

        let help = NSMenuItem()
        main.addItem(help)
        help.submenu = NSMenu(title: L("帮助"))

        NSApp.mainMenu = main
    }
    @objc func newTab() { win.newLocalTab() }
    @objc func closeTab() { win.closeTab(win.selTab) }
    @objc func copySel() { win.tabs[win.selTab].term?.copySelection() }
    @objc func paste() { win.tabs[win.selTab].term?.paste() }
    @objc func selectAll() { win.tabs[win.selTab].term?.selectAll(nil) }
    @objc func prevTab() { win.select((win.selTab + win.tabs.count - 1)
                                      % max(win.tabs.count, 1)) }
    @objc func nextTab() { win.select((win.selTab + 1)
                                      % max(win.tabs.count, 1)) }
    @objc func gotoTab(_ s: NSMenuItem) {
        if let i = Int(String(s.title.last!)) { win.select(i - 1) }
    }
    private var fontBase: CGFloat = 14
    @objc func fontPlus() { setFont(+1) }
    @objc func fontMinus() { setFont(-1) }
    @objc func fontReset() { setFont(0) }
    private func setFont(_ d: CGFloat) {
        let t = win.tabs[win.selTab].term
        if d == 0 { t?.fontSize = fontBase }
        else { t?.fontSize += d }
    }
    @objc func showSettings() {
        win.sidebar.select(7)
        win.showSection(7)
    }
}
