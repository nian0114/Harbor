import XCTest
import Foundation
import CHarbor
@testable import HarborKit

/// M2b core-贯通测试：临时 vault 上的 known_hosts 导入（去重/
/// hashed/[host]:port/多类型）与日志 CSV（BOM/RFC4180/中文）。
final class M2bVaultTests: XCTestCase {
    private var vault: OpaquePointer?
    private var dir: String = ""

    override func setUp() {
        super.setUp()
        dir = NSTemporaryDirectory()
            + "hb-m2b-\(UUID().uuidString)/vault.db"
        var dek = [UInt8](repeating: 0xA5, count: 32)
        var v: OpaquePointer?
        let rc = dek.withUnsafeBufferPointer { buf -> hb_err in
            dir.withCString { p in
                hb_vault_open(p, buf.baseAddress!, &v) }
        }
        XCTAssertEqual(rc, HB_OK)
        vault = v
    }
    override func tearDown() {
        vault.map { hb_vault_close($0) }
        try? FileManager.default.removeItem(
            atPath: (dir as NSString).deletingLastPathComponent)
        super.tearDown()
    }

    // ---- FR-09 known_hosts 导入 ----
    func testKnownImportDedupeAndForms() throws {
        // 生成行：dupes + [host]:port + 多 key 类型 + hashed。
        // key blob 须为合法 base64（core 解码后算 SHA256 指纹）。
        func k(_ tag: UInt8) -> String {
            Data((0..<32).map { UInt8((Int(tag) + $0) & 0xff) })
                .base64EncodedString()
        }
        var lines: [String] = []
        lines.append("h1.example ssh-ed25519 \(k(1))")
        lines.append("h1.example ssh-ed25519 \(k(1))")   // dup
        lines.append("[h2.example]:2222 ssh-rsa \(k(2))")
        lines.append("h3.example ecdsa-sha2-nistp256 \(k(3))")
        lines.append("|1|c2FsdA==|aGFzaA== ssh-ed25519 \(k(4))")
        lines.append("bogus")                            // 字段不足
        lines.append("h9.example ssh-ed25519 x") // 非法 b64
        var r = hb_import_result()
        defer { hb_import_result_free(&r) }
        let ok = lines.joined(separator: "\n").withCString {
            hb_known_import_text(vault, $0, &r)
        }
        // 无效行是否被拒由 core 决定；统计有效 key 行数
        XCTAssertEqual(ok, HB_OK)
        XCTAssertEqual(r.created, 4)   // 4 条唯一（dup 第 2 行跳过）
        XCTAssertEqual(r.skipped, 1)
        XCTAssertGreaterThanOrEqual(r.nwarn, 1)   // 无效行告警
        // 再导入一遍：全部应 skipped（幂等去重）
        var r2 = hb_import_result()
        defer { hb_import_result_free(&r2) }
        _ = lines.joined(separator: "\n").withCString {
            hb_known_import_text(vault, $0, &r2)
        }
        XCTAssertEqual(r2.created, 0)
        XCTAssertEqual(r2.skipped, 5)   // 5 条合法行全部判重（h1 计两次）
        // 导出能还原 hashed/[host]:port 行
        var out: UnsafeMutablePointer<CChar>?
        XCTAssertEqual(hb_known_export_text(vault, &out), HB_OK)
        let txt = String(cString: out!)
        free(out)
        XCTAssertTrue(txt.contains("[h2.example]:2222"))
        XCTAssertTrue(txt.contains("|1|"))
    }

    // ---- FR-10 日志 CSV ----
    func testLogCsvBomAndEscaping() throws {
        func add(_ user: String, _ host: String, _ proto: String,
                 _ result: String, _ reason: String?, _ dur: Int64,
                 saved: Bool) {
            var l = hb_log()
            user.withCString { u in host.withCString { h in
                proto.withCString { p in result.withCString { r in
                    l.user = u; l.host = h; l.proto = p; l.result = r
                    l.reason = nil; l.saved = false; l.duration_s = -1
                    var id: Int64 = 0
                    XCTAssertEqual(hb_vault_log_add(vault, &l, &id), HB_OK)
                    if let rsn = reason {
                        rsn.withCString {
                            _ = hb_vault_log_finish(vault, id, r,
                                                    $0, dur) }
                    } else {
                        _ = hb_vault_log_finish(vault, id, r, nil, dur)
                    }
                    if saved {
                        _ = hb_vault_log_set_saved(vault, id, true)
                    }
                }}}}
        }
        add("管理员", "db,prod", "ssh", "ok", nil, 5, saved: true)
        add("u\"q", "bad \"host\"", "ssh", "auth", "auth_failed",
            12, saved: false)
        var out: UnsafeMutablePointer<CChar>?
        XCTAssertEqual(hb_vault_log_export_csv(vault, &out), HB_OK)
        let csv = String(cString: out!)
        free(out)
        // BOM
        let b = [UInt8](csv.utf8)
        XCTAssertEqual(Array(b.prefix(3)), [0xEF, 0xBB, 0xBF])
        XCTAssertTrue(csv.contains(
            "Date,User,Host,Protocol,Result,Reason,Duration,Saved"))
        XCTAssertTrue(csv.contains("\"db,prod\""))
        XCTAssertTrue(csv.contains("\"u\"\"q\""))
        XCTAssertTrue(csv.contains("管理员"))
        XCTAssertTrue(csv.contains("auth_failed"))
    }

    // ---- FR-08 history：dedupe + 空白忽略 ----
    func testHistoryDedupe() {
        "ls".withCString { _ = hb_vault_history_add(vault, nil, $0) }
        "ls".withCString { _ = hb_vault_history_add(vault, nil, $0) }
        "pwd".withCString { _ = hb_vault_history_add(vault, nil, $0) }
        "  ".withCString { _ = hb_vault_history_add(vault, nil, $0) }
        var n = 0
        let cb: hb_history_cb = { _, ud in
            ud!.assumingMemoryBound(to: Int.self).pointee += 1
            return true }
        _ = hb_vault_history_foreach(vault, nil, nil, cb, &n)
        XCTAssertEqual(n, 2)   // 连续重复 + 空白都未入库
    }
}
