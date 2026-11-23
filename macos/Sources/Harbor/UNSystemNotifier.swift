import AppKit
import UserNotifications
import HarborKit

/// M8 FR-13：UNUserNotificationCenter 系统通知实现。
/// 文案、仅主机名隐私、focus-DND/免打扰全部已在 WatchRoute 侧算好，
/// 这里只负责投递与点击路由（对齐 linux send_notification）。
final class UNSystemNotifier: NSObject, SystemNotifier,
                              UNUserNotificationCenterDelegate {
    /// 成功/失败两个 category（linux 普通通知 vs 高优先失败音）。
    static let catOK = "hb-ok"
    static let catErr = "hb-err"
    /// dev 可观测投递记录（格式同 RecordingSystemNotifier）。
    private(set) var calls: [String] = []
    /// 最近一次投递的 userInfo（dev notif-click 模拟响应用）。
    private(set) var lastUserInfo: [String: Int]?
    private var askedAuth = false

    override init() {
        super.init()
        let c = UNUserNotificationCenter.current()
        c.delegate = self
        c.setNotificationCategories([
            UNNotificationCategory(identifier: Self.catOK, actions: [],
                                   intentIdentifiers: []),
            UNNotificationCategory(identifier: Self.catErr, actions: [],
                                   intentIdentifiers: []),
        ])
    }

    /// 首次需要时才弹系统授权框（PRD：不预申请）。
    private func ensureAuth(_ done: @escaping () -> Void) {
        guard !askedAuth else { done(); return }
        askedAuth = true
        UNUserNotificationCenter.current()
            .requestAuthorization(options: [.alert, .sound]) { _, _ in
                DispatchQueue.main.async { done() }
            }
    }

    func post(title: String, body: String, tabIndex: Int?,
              priorityHigh: Bool) {
        calls.append("\(priorityHigh ? "high" : "norm")|\(title)|\(body)")
        if calls.count > 200 { calls.removeFirst(calls.count - 200) }
        var info: [String: Int] = [:]
        if let ti = tabIndex, ti >= 0 { info["tab"] = ti }
        lastUserInfo = info
        ensureAuth { [info] in
            let c = UNMutableNotificationContent()
            c.title = title
            c.body = body
            c.categoryIdentifier =
                priorityHigh ? Self.catErr : Self.catOK
            c.userInfo = info
            if priorityHigh { c.sound = .default }
            let r = UNNotificationRequest(
                identifier: UUID().uuidString, content: c, trigger: nil)
            UNUserNotificationCenter.current().add(r) { _ in }
        }
    }

    /// 点击系统通知 → 激活并选中对应 tab（userInfo["tab"]，与内联
    /// 通知的 notif-jump 同路）。
    private func route(_ info: [AnyHashable: Any]) {
        NSApp.activate(ignoringOtherApps: true)
        guard let app = NSApp.delegate as? App,
              let ti = info["tab"] as? Int, ti >= 0 else { return }
        app.showMainWindow()
        app.win?.select(ti)
    }
    func userNotificationCenter(
        _ c: UNUserNotificationCenter,
        didReceive resp: UNNotificationResponse,
        withCompletionHandler done: @escaping () -> Void) {
        route(resp.notification.request.content.userInfo)
        done()
    }
    /// 前台也展示（linux 同样不打断——focus-DND 已在 WatchRoute 侧挡过）。
    func userNotificationCenter(
        _ c: UNUserNotificationCenter,
        willPresent n: UNNotification,
        withCompletionHandler done: @escaping
            (UNNotificationPresentationOptions) -> Void) {
        done([.banner, .sound])
    }

    // ---------- dev hooks（仅 DevServer 调用） ----------
    var devAuthStatus: String {
        var st = "unknown"
        let sem = DispatchSemaphore(value: 0)
        UNUserNotificationCenter.current()
            .getNotificationSettings { s in
                switch s.authorizationStatus {
                case .notDetermined: st = "notDetermined"
                case .authorized: st = "authorized"
                case .denied: st = "denied"
                case .provisional: st = "provisional"
                case .ephemeral: st = "ephemeral"
                @unknown default: st = "unknown"
                }
                sem.signal()
            }
        _ = sem.wait(timeout: .now() + 5)
        return st
    }
    func devDelivered(_ done: @escaping (String) -> Void) {
        UNUserNotificationCenter.current()
            .getDeliveredNotifications { list in
                let s = list.map {
                    let r = $0.request.content
                    let ti = (r.userInfo["tab"] as? Int) ?? -1
                    return "\(r.categoryIdentifier)|\(r.title)|\(r.body)|tab=\(ti)"
                }.joined(separator: " ;; ")
                DispatchQueue.main.async { done(s) }
            }
    }
    /// dev：用真实通知的 userInfo 走一遍点击路由。
    func devSimulateClick(_ tab: Int) {
        route(["tab": tab])
    }
}
