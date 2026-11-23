import XCTest
@testable import HarborKit

/// FR-07 纯逻辑测试：几何（drawRect/mapPoint）、画质映射、
/// 键表（macVK→x11kc/keysym）、滚轮聚合与方向、剪贴板判定。
final class RdKitTests: XCTestCase {

    // ---------- 画质预设（hb_rd_quality） ----------
    func testQualityOf() {
        XCTAssertEqual(RdKit.qualityOf("fast"), 0)
        XCTAssertEqual(RdKit.qualityOf("balanced"), 1)
        XCTAssertEqual(RdKit.qualityOf("high"), 2)
        XCTAssertEqual(RdKit.qualityOf(""), 1)      // 默认均衡
        XCTAssertEqual(RdKit.qualityOf("bogus"), 1)
    }
    func testQualityRoundTrip() {
        for q in 0..<3 {
            XCTAssertEqual(RdKit.qualityOf(RdKit.qualityStr(q)), q)
            XCTAssertFalse(RdKit.qualityLabel(q).isEmpty)
        }
    }

    // ---------- 几何：drawRect（缩放适应/原始大小） ----------
    func testDrawRectFit() {
        // 800x600 可用 / 1600x1200 fb → 0.5 缩放、无 letterbox
        let r = RdKit.drawRect(availW: 800, availH: 600,
                               fbW: 1600, fbH: 1200, orig: false)
        XCTAssertEqual(r.w, 800, accuracy: 0.01)
        XCTAssertEqual(r.h, 600, accuracy: 0.01)
        XCTAssertEqual(r.x, 0, accuracy: 0.01)
        XCTAssertEqual(r.y, 0, accuracy: 0.01)
    }
    func testDrawRectLetterbox() {
        // 宽可用区、窄 fb → 左右 letterbox
        let r = RdKit.drawRect(availW: 1000, availH: 500,
                               fbW: 800, fbH: 600, orig: false)
        XCTAssertEqual(r.h, 500, accuracy: 0.01)
        XCTAssertEqual(r.w, 500.0 * 800 / 600, accuracy: 0.01)
        XCTAssertEqual(r.x, (1000 - r.w) / 2, accuracy: 0.01)
        XCTAssertEqual(r.y, 0, accuracy: 0.01)
        XCTAssertGreaterThan(r.x, 0)
    }
    func testDrawRectOrigCentered() {
        // 原始大小且画布大于 fb → 居中 1:1
        let r = RdKit.drawRect(availW: 1400, availH: 900,
                               fbW: 800, fbH: 600, orig: true)
        XCTAssertEqual(r.w, 800); XCTAssertEqual(r.h, 600)
        XCTAssertEqual(r.x, 300); XCTAssertEqual(r.y, 150)
    }
    func testDrawRectOrigOversize() {
        // 原始大小但 fb 超出可用区 → 起点 0（滚动条接管）
        let r = RdKit.drawRect(availW: 400, availH: 300,
                               fbW: 800, fbH: 600, orig: true)
        XCTAssertEqual(r.x, 0); XCTAssertEqual(r.y, 0)
        XCTAssertEqual(r.w, 800); XCTAssertEqual(r.h, 600)
    }
    func testDrawRectZeroFb() {
        let r = RdKit.drawRect(availW: 800, availH: 600,
                               fbW: 0, fbH: 0, orig: false)
        XCTAssertEqual(r.w, 0); XCTAssertEqual(r.h, 0)
    }

    // ---------- 坐标映射：控件 → fb ----------
    func testMapPointFit() {
        let r = (x: 100.0, y: 50.0, w: 800.0, h: 450.0)
        let m = RdKit.mapPoint(x: 500, y: 275, rect: r,
                               fbW: 1600, fbH: 900)!
        XCTAssertEqual(m.fx, 800, accuracy: 0.5)   // 中心 → fb 中心
        XCTAssertEqual(m.fy, 450, accuracy: 0.5)
    }
    func testMapPointCorners() {
        let r = (x: 0.0, y: 0.0, w: 800.0, h: 600.0)
        let tl = RdKit.mapPoint(x: 0, y: 0, rect: r, fbW: 1600, fbH: 1200)!
        XCTAssertEqual(tl.fx, 0); XCTAssertEqual(tl.fy, 0)
        let br = RdKit.mapPoint(x: 799.9, y: 599.9, rect: r,
                                fbW: 1600, fbH: 1200)!
        XCTAssertLessThan(br.fx, 1600)
        XCTAssertGreaterThan(br.fx, 1590)
    }
    func testMapPointLetterboxReject() {
        // letterbox 区（左右黑边）拒绝输入
        let r = (x: 200.0, y: 0.0, w: 600.0, h: 600.0)
        XCTAssertNil(RdKit.mapPoint(x: 50, y: 300, rect: r,
                                    fbW: 800, fbH: 800))
        XCTAssertNil(RdKit.mapPoint(x: 850, y: 300, rect: r,
                                    fbW: 800, fbH: 800))
        XCTAssertNil(RdKit.mapPoint(x: 200, y: 600, rect: r,
                                    fbW: 800, fbH: 800))
    }

    // ---------- 键表：macVK → x11kc / keysym ----------
    func testKeyLetters() {
        let a = RdKit.keyInfo(0x00)!          // macVK A
        XCTAssertEqual(a.x11kc, 30 + 8)       // evdev KEY_A + 8
        XCTAssertEqual(a.keysym, 0)           // 字符键取产生字符
    }
    func testKeyModifiers() {
        XCTAssertEqual(RdKit.keyInfo(0x38)!.keysym, 0xFFE1) // LShift
        XCTAssertEqual(RdKit.keyInfo(0x3B)!.keysym, 0xFFE3) // LCtrl
        XCTAssertEqual(RdKit.keyInfo(0x3A)!.keysym, 0xFFE9) // LAlt
        XCTAssertEqual(RdKit.keyInfo(0x37)!.x11kc, 125 + 8) // LCmd→Meta
        XCTAssertEqual(RdKit.keyInfo(0x36)!.keysym, 0xFFEC) // RCmd→Super_R
        XCTAssertEqual(RdKit.keyInfo(0x37)!.keysym, 0xFFEB) // ⌘=Super_L
    }
    func testKeyNav() {
        XCTAssertEqual(RdKit.keyInfo(0x24)!.keysym, 0xFF0D) // Return
        XCTAssertEqual(RdKit.keyInfo(0x35)!.keysym, 0xFF1B) // Esc
        XCTAssertEqual(RdKit.keyInfo(0x7B)!.keysym, 0xFF51) // Left
        XCTAssertEqual(RdKit.keyInfo(0x7A)!.x11kc, 59 + 8)  // F1
        XCTAssertEqual(RdKit.keyInfo(0x6F)!.keysym, 0xFFC9) // F12
        XCTAssertNil(RdKit.keyInfo(0xFFFF))                 // 未知
    }
    func testCharKeysym() {
        XCTAssertEqual(RdKit.charKeysym(0x61), 0x61)        // 'a'
        XCTAssertEqual(RdKit.charKeysym(0x41), 0x41)        // 'A'
        XCTAssertEqual(RdKit.charKeysym(0xE9), 0xE9)        // 'é' Latin-1
        XCTAssertEqual(RdKit.charKeysym(0x4E2D),
                       0x01000000 | 0x4E2D)                 // '中'
        XCTAssertEqual(RdKit.charKeysym(0x01), 0)           // 控制码
    }
    func testCadWin() {
        // linux send_cad：Ctrl(37) Alt(64) Del(119)
        XCTAssertEqual(RdKit.cad.map { $0.kc }, [37, 64, 119])
        XCTAssertEqual(RdKit.cad.map { $0.keysym },
                       [0xFFE3, 0xFFE9, 0xFFFF])
        XCTAssertEqual(RdKit.winKey.kc, 133)
        XCTAssertEqual(RdKit.winKey.keysym, 0xFFEB)
    }

    // ---------- 鼠标 / 滚轮 ----------
    func testButtonBit() {
        XCTAssertEqual(RdKit.buttonBit(0), 1)   // left
        XCTAssertEqual(RdKit.buttonBit(1), 4)   // right
        XCTAssertEqual(RdKit.buttonBit(2), 2)   // middle
        XCTAssertEqual(RdKit.buttonBit(9), 0)
    }
    func testScrollSteps() {
        // 触控板像素增量按 16px/行聚合，余数保留
        var s = RdKit.scrollSteps(acc: 0, delta: 10, unit: 16)
        XCTAssertEqual(s.steps, 0); XCTAssertEqual(s.rest, 10)
        s = RdKit.scrollSteps(acc: s.rest, delta: 10, unit: 16)
        XCTAssertEqual(s.steps, 1); XCTAssertEqual(s.rest, 4, accuracy: 0.01)
        s = RdKit.scrollSteps(acc: 0, delta: -40, unit: 16)
        XCTAssertEqual(s.steps, -2)
    }
    func testWheelMask() {
        XCTAssertEqual(RdKit.wheelMask(dy: 1), 1 << 3)   // 上
        XCTAssertEqual(RdKit.wheelMask(dy: -1), 1 << 4)  // 下
        XCTAssertEqual(RdKit.hwheelMask(dx: -1), 1 << 6) // 右
        XCTAssertEqual(RdKit.hwheelMask(dx: 1), 1 << 5)  // 左
    }

    // ---------- 回环判定（隧道 dest 语义） ----------
    func testIsLoopback() {
        XCTAssertTrue(RdKit.isLoopback(""))
        XCTAssertTrue(RdKit.isLoopback("localhost"))
        XCTAssertTrue(RdKit.isLoopback("LOCALHOST"))
        XCTAssertTrue(RdKit.isLoopback("127.0.0.1"))
        XCTAssertTrue(RdKit.isLoopback("::1"))
        XCTAssertFalse(RdKit.isLoopback("10.0.0.1"))
        XCTAssertFalse(RdKit.isLoopback("example.com"))
    }
}
