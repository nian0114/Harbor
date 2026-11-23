import AppKit
import IOKit.pwr_mgt
import HarborKit

/// M8 FR-16 macOS 合盖/休眠策略：
///  —— 默认：willSleep 前干净断开会话（hb_session_close → 日志正常
///     收尾，桌面走 rd.close），didWake 后对原存活页走已有重连
///     （restart/retry；桌面页靠自身 backoff；转发规则 wantRun 保留
///     由 pf 引擎自重连）。
///  —— keep_connected 开：有在线会话时持 PreventUserIdleSystemSleep
///     断言（IOPS 防空闲睡眠），无会话即释放；设备本地，不同步。
final class SleepPolicy {
    static let shared = SleepPolicy()
    private var obs: [NSObjectProtocol] = []
    private var assertID = IOPMAssertionID(0)
    private var sleeping = false
    /// willSleep 时活着的可重连页（weak；didWake 只重启这些）。
    private var toWake: [() -> TermTab?] = []

    func install() {
        let nc = NSWorkspace.shared.notificationCenter
        obs.append(nc.addObserver(
            forName: NSWorkspace.willSleepNotification,
            object: nil, queue: .main)
        { [weak self] _ in self?.willSleep() })
        obs.append(nc.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil, queue: .main)
        { [weak self] _ in self?.didWake() })
    }

    /// 保持连接（防止休眠断言）：keep_connected 且存在在线会话 →
    /// 持断言；否则释放。由 MenuBar 的周期 refresh 驱动。
    func refreshAssertion() {
        let v = Vault.shared
        let want = v.settingsBool("keep_connected", false) &&
            MenuBar.onlineCount((NSApp.delegate as? App)?.win) > 0
        if want && assertID == 0 {
            var id = IOPMAssertionID(0)
            if IOPMAssertionCreateWithName(
                kIOPMAssertPreventUserIdleSystemSleep as CFString,
                IOPMAssertionLevel(kIOPMAssertionLevelOn),
                "Harbor" as CFString, &id) == kIOReturnSuccess {
                assertID = id
            }
        } else if !want && assertID != 0 {
            IOPMAssertionRelease(assertID)
            assertID = 0
        }
    }
    var devAssertionHeld: Bool { assertID != 0 }

    /// willSleep：在线页干净断开并记下，供 didWake 重连。
    /// keep_connected 只挡空闲睡眠——真睡（合盖/强制）仍要断开。
    private func willSleep() {
        guard !sleeping else { return }
        sleeping = true
        toWake = []
        guard let win = (NSApp.delegate as? App)?.win else { return }
        for t in win.tabs where t.onlineCount > 0 {
            if let w = t as? WsTab {
                for l in w.ws.leavesList where l.wakeReconnectable {
                    if l.session?.alive == true {
                        toWake.append { [weak l] in l }
                    }
                }
            } else if t.wakeReconnectable {
                toWake.append { [weak t] in t }
            }
            t.disconnect()
        }
    }
    private func didWake() {
        guard sleeping else { return }
        sleeping = false
        for w in toWake { w()?.restart() }
        toWake = []
    }

    // ---------- dev hooks ----------
    func devSleep() { willSleep() }
    func devWake() { didWake() }
}
