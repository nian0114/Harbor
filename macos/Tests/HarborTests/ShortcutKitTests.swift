import XCTest
@testable import HarborKit

final class ShortcutKitTests: XCTestCase {
    func testParse() {
        let p = ShortcutTable.parse("cmd+shift+k")!
        XCTAssertEqual(p.key, "k")
        XCTAssertEqual(p.mods, SC_M_CMD | SC_M_SHIFT)
        XCTAssertNil(ShortcutTable.parse("cmd+bogus"))
        // "" = 合法禁用
        let d = ShortcutTable.parse("")!
        XCTAssertEqual(d.key, "")
        XCTAssertEqual(d.mods, 0)
    }
    func testAccelRoundTrip() {
        let a = ShortcutTable.accel(key: "tab", mods: SC_M_CTRL)
        XCTAssertEqual(ShortcutTable.parse(a)!.key, "tab")
        XCTAssertEqual(ShortcutTable.parse(a)!.mods, SC_M_CTRL)
    }
    func testEffectiveOverride() {
        // 找 palette 行号并覆盖
        let i = ShortcutTable.rows.firstIndex { $0.id == "palette" }!
        let eff = ShortcutTable.effective(
            ["palette@\(i)=ctrl+shift+p"])
        XCTAssertEqual(eff[i].key, "p")
        XCTAssertEqual(eff[i].mods, SC_M_CTRL | SC_M_SHIFT)
        // 脏覆盖回落默认
        let eff2 = ShortcutTable.effective(["palette@\(i)=bogus"])
        XCTAssertEqual(eff2[i].key, "k")
    }
    func testConflictAndDisable() {
        let i = ShortcutTable.rows.firstIndex { $0.id == "palette" }!
        let eff = ShortcutTable.rows
        XCTAssertEqual(ShortcutTable.conflict(eff, skip: 999,
            key: "k", mods: SC_M_CMD), i)
        // 禁用行不冲突
        XCTAssertNil(ShortcutTable.conflict(eff, skip: 999,
            key: "", mods: 0))
    }
    func testSetOverride() {
        var ov: [String] = []
        let i = ShortcutTable.rows.firstIndex { $0.id == "palette" }!
        ShortcutTable.setOverride(&ov, idx: i, accel: "cmd+p")
        XCTAssertEqual(ov, ["palette@\(i)=cmd+p"])
        ShortcutTable.setOverride(&ov, idx: i, accel: "cmd+j")
        XCTAssertEqual(ov, ["palette@\(i)=cmd+j"])
        ShortcutTable.setOverride(&ov, idx: i, accel: nil)
        XCTAssertTrue(ov.isEmpty)
    }
    func testTerminalRisk() {
        // 无修饰 = 吞普通输入
        XCTAssertTrue(ShortcutTable.terminalRisk(key: "j", mods: 0))
        // 纯 ⌃+字母 = shell 控制键
        XCTAssertTrue(ShortcutTable.terminalRisk(key: "c",
                                                 mods: SC_M_CTRL))
        // ⌃Tab 不算字母 → 不警告
        XCTAssertFalse(ShortcutTable.terminalRisk(key: "tab",
                                                  mods: SC_M_CTRL))
        // ⌘ 组合正常
        XCTAssertFalse(ShortcutTable.terminalRisk(key: "t",
                                                  mods: SC_M_CMD))
        XCTAssertFalse(ShortcutTable.terminalRisk(
            key: "c", mods: SC_M_CTRL | SC_M_CMD))
    }
    func testLabel() {
        XCTAssertEqual(ShortcutTable.label(key: "k",
                                           mods: SC_M_CMD), "⌘K")
        XCTAssertEqual(ShortcutTable.label(key: "tab",
                                           mods: SC_M_CTRL), "⌃⇥")
        XCTAssertFalse(ShortcutTable.label(key: "",
                                           mods: 0).isEmpty)
    }
    func testKeyName() {
        XCTAssertEqual(ShortcutTable.keyName("K"), "k")
        XCTAssertEqual(ShortcutTable.keyName("F5"), "f5")
        XCTAssertNil(ShortcutTable.keyName("bogus"))
        XCTAssertNotNil(ShortcutTable.keyName("]"))
    }
    func testTabNNotEditable() {
        let i = ShortcutTable.rows.firstIndex { $0.id == "tab-n" }!
        XCTAssertFalse(ShortcutTable.editable(i))
        XCTAssertTrue(ShortcutTable.editable(0))
    }
}
