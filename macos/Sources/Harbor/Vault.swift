import Foundation
import AppKit
import Security
import CHarbor
import HarborKit

/// 数据目录 + vault + DEK（macOS Keychain generic password，
/// fallback 0600 vault.key）。
final class Vault {
    static let shared = Vault()
    let dataDir: URL
    private(set) var vault: OpaquePointer?
    private(set) var dekFallback = false

    private init() {
        let env = ProcessInfo.processInfo.environment
        if let d = env["HARBOR_DATA_DIR"], !d.isEmpty {
            dataDir = URL(fileURLWithPath: d)
        } else {
            dataDir = FileManager.default.urls(
                for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("Harbor")
        }
        try? FileManager.default.createDirectory(
            at: dataDir, withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700])
        var dekData = obtainDEK()
        var dek = [UInt8](dekData)
        dekData.resetBytes(in: 0..<dekData.count)
        var v: OpaquePointer?
        let db = dataDir.appendingPathComponent("vault.db").path
        let r = dek.withUnsafeMutableBytes { ptr -> hb_err in
            hb_vault_open(db, ptr.baseAddress!.assumingMemoryBound(to: UInt8.self), &v)
        }
        _ = memset_s(&dek, 32, 0, 32)
        if r != HB_OK || v == nil {
            NSLog("hb_vault_open failed: %d", r.rawValue)
        }
        vault = v
        // 上次运行被 kill/崩溃留下的“进行中”日志行 → 标记为中断（同 linux startup）
        if let v { _ = hb_vault_log_sweep(v) }
        // 变更回调（同 Linux hb-store "changed" 信号）：log/history/对象写入
        // → 通知各页 reload。回调可能发自会话线程，投递前切回主队列。
        if let v {
            hb_vault_set_change_cb(v, { _, tp, idp, _ in
                let t = tp.map { String(cString: $0) } ?? ""
                let i = idp.map { String(cString: $0) } ?? ""
                DispatchQueue.main.async {
                    NotificationCenter.default.post(
                        name: .hbVaultChanged, object: nil,
                        userInfo: ["type": t, "id": i])
                }
            }, nil)
        }
    }

    // ---- DEK ----
    private var dekAccount: String { "vault-dek:" + dataDir.path }
    private var keyFile: URL {
        dataDir.appendingPathComponent("vault.key") }
    private var dbFile: URL {
        dataDir.appendingPathComponent("vault.db") }

    /// (status, key-data)。status != errSecSuccess 时 data 为 nil。
    private func keychainRead() -> (OSStatus, Data?) {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "dev.harbor.Harbor.vault",
            kSecAttrAccount as String: dekAccount,
            kSecReturnData as String: true,
        ]
        var item: CFTypeRef?
        let st = SecItemCopyMatching(q as CFDictionary, &item)
        guard st == errSecSuccess,
              let d = item as? Data, d.count == 32
        else { return (st, nil) }
        return (st, d)
    }
    /// SecItemAdd 写入；errSecDuplicateItem 时重读，绝不 SecItemDelete。
    private func keychainAdd(_ d: Data) -> Bool {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "dev.harbor.Harbor.vault",
            kSecAttrAccount as String: dekAccount,
            kSecAttrAccessible as String:
                kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
            kSecValueData as String: d,
        ]
        let st = SecItemAdd(q as CFDictionary, nil)
        if st == errSecSuccess { return true }
        if st == errSecDuplicateItem {
            // 并发写入：以链中现有条目为准（读取路径，无删除）
            let (rst, rd) = keychainRead()
            return rst == errSecSuccess && rd != nil
        }
        return false
    }
    private func fileDEK() -> Data? {
        (try? Data(contentsOf: keyFile)).flatMap { $0.count == 32 ? $0 : nil }
    }
    private func fileDEKSet(_ d: Data) -> Bool {
        do {
            try d.write(to: keyFile, options: .atomic)
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o600], ofItemAtPath: keyFile.path)
            return true
        } catch { return false }
    }
    private func genDEK() -> Data {
        var b = Data(count: 32)
        _ = b.withUnsafeMutableBytes {
            SecRandomCopyBytes(kSecRandomDefault, 32, $0.baseAddress!) }
        return b
    }

    /// 阻塞告警：解释状态，给 重试/退出；重试返回 true 继续循环。
    private func dekAlert(_ text: String) -> Bool {
        let a = NSAlert()
        a.messageText = L("无法读取或保存保险库密钥")
        a.informativeText = text
        a.alertStyle = .critical
        a.addButton(withTitle: L("重试"))
        a.addButton(withTitle: L("退出"))
        return a.runModal() == .alertFirstButtonReturn
    }

    /// 按 DEKPlan 决策取/建 DEK；失败路径要么重试要么退出，
    /// 绝不销毁已有数据或丢下退出即丢的密钥。
    private func obtainDEK() -> Data {
        while true {
            #if DEBUG
            let fileMode = ProcessInfo.processInfo
                .environment["HARBOR_DEK_FILE"] == "1"
            let (st, kd) = fileMode ? (errSecItemNotFound, nil) : keychainRead()
            #else
            let fileMode = false
            let (st, kd) = keychainRead()
            #endif
            let action = DEKPlan.decide(
                chainStatus: st, chainHasKey: kd != nil,
                fileKey: fileDEK() != nil,
                dbExists: FileManager.default.fileExists(atPath: dbFile.path),
                fileMode: fileMode)
            switch action {
            case .useKeychain:
                if let kd { return kd }
                continue   // 决策与读数不一致，重读
            case .useFile:
                if let d = fileDEK() { dekFallback = true; return d }
                continue
            case .createNew:
                let d = genDEK()
                if !fileMode, keychainAdd(d) { return d }
                if !fileMode, let (_, kd2) = Optional(keychainRead()),
                    kd2 != nil { return kd2! }
                if fileDEKSet(d) { dekFallback = true; return d }
                // 两边都写不进：退出即丢，拒绝运行
                if dekAlert(L("Keychain 和密钥文件都无法写入，"
                              + "保险库密钥无法持久保存。")) { continue }
                NSApp.terminate(nil); exit(1)
            case .failKeychain(let status):
                if dekAlert(DEKPlan.describe(status)) { continue }
                NSApp.terminate(nil); exit(1)
            case .failKeyUnavailable:
                if dekAlert(L("找到 vault.db 但读不到解密密钥"
                              + "（Keychain 和 vault.key 均无）。"
                              + "为避免数据损毁，已停止启动。")) { continue }
                NSApp.terminate(nil); exit(1)
            }
        }
    }

    // ---- 设置（vault settings 单例对象）----
    func setting(_ k: String) -> String? {
        guard let v = vault else { return nil }
        var o: OpaquePointer?
        guard hb_vault_settings(v, &o) == HB_OK, let o else { return nil }
        defer { hb_obj_free(o) }
        guard let s = hb_obj_get_str(o, k) else { return nil }
        return String(cString: s)
    }
    func setSetting(_ k: String, _ val: String) {
        guard let v = vault else { return }
        var o: OpaquePointer?
        guard hb_vault_settings(v, &o) == HB_OK, let o else { return }
        hb_obj_set_str(o, k, val)
        _ = hb_vault_put(v, o)
        hb_obj_free(o)
    }
    func settingsBool(_ k: String, _ def: Bool) -> Bool {
        guard let v = vault else { return def }
        var o: OpaquePointer?
        guard hb_vault_settings(v, &o) == HB_OK, let o else { return def }
        defer { hb_obj_free(o) }
        return hb_obj_get_bool(o, k, def)
    }
    func setSettingsBool(_ k: String, _ val: Bool) {
        guard let v = vault else { return }
        var o: OpaquePointer?
        guard hb_vault_settings(v, &o) == HB_OK, let o else { return }
        hb_obj_set_bool(o, k, val)
        _ = hb_vault_put(v, o)
        hb_obj_free(o)
    }
    func settingsInt(_ k: String, _ def: Int64) -> Int64 {
        guard let v = vault else { return def }
        var o: OpaquePointer?
        guard hb_vault_settings(v, &o) == HB_OK, let o else { return def }
        defer { hb_obj_free(o) }
        return hb_obj_get_int(o, k, def)
    }
    func setSettingsInt(_ k: String, _ val: Int64) {
        guard let v = vault else { return }
        var o: OpaquePointer?
        guard hb_vault_settings(v, &o) == HB_OK, let o else { return }
        hb_obj_set_int(o, k, val)
        _ = hb_vault_put(v, o)
        hb_obj_free(o)
    }
}

// ---------- vault 对象便捷层 ----------
extension Vault {
    /// 类型列表（不含 tombstone）。调用方持有 Swift 侧对象。
    func list(_ type: String) -> [OpaquePointer] {
        guard let v = vault else { return [] }
        var out: UnsafeMutablePointer<OpaquePointer?>?
        var n = 0
        guard hb_vault_list(v, type, &out, &n) == HB_OK else { return [] }
        var r: [OpaquePointer] = []
        for i in 0..<n { if let o = out?[i] { r.append(o) } }
        // 数组本身 malloc；元素归调用方 hb_obj_free（list_free 会把对象也释放）。
        free(out)
        return r
    }
    func get(_ id: String) -> OpaquePointer? {
        guard let v = vault else { return nil }
        var o: OpaquePointer?
        guard hb_vault_get(v, id, &o) == HB_OK else { return nil }
        return o
    }
    func put(_ o: OpaquePointer) {
        guard let v = vault else { return }
        _ = hb_vault_put(v, o)
    }
    func newObj(_ type: String) -> OpaquePointer? { hb_obj_new(type) }
    func delete(_ id: String) { vault.map { _ = hb_vault_delete($0, id) } }
    func undelete(_ id: String) {
        vault.map { _ = hb_vault_undelete($0, id) } }
    func secret(_ id: String, _ field: String) -> String? {
        guard let v = vault else { return nil }
        guard let s = hb_vault_get_secret(v, id, field) else { return nil }
        defer { hb_secret_free(s) }
        return String(cString: s)
    }
    func setSecret(_ id: String, _ field: String, _ value: String) {
        vault.map { _ = hb_vault_set_secret($0, id, field, value) }
    }
    /// 直接取得 secret 的 C 字符串（调用方负责 wipe + hb_secret_free）。
    func secretC(_ id: String, _ field: String) -> UnsafeMutablePointer<CChar>? {
        guard let v = vault else { return nil }
        return hb_vault_get_secret(v, id, field)
    }
}

// 可选 CChar 指针 → Swift String
extension Optional where Wrapped == UnsafePointer<CChar> {
    var str: String { map { String(cString: $0) } ?? "" }
}
extension Optional where Wrapped == UnsafeMutablePointer<CChar> {
    var str: String { map { String(cString: $0) } ?? "" }
}
extension UnsafePointer where Pointee == CChar {
    var str: String {
        guard bitPattern != 0 else { return "" }
        return String(cString: self)
    }
    private var bitPattern: UInt { UInt(bitPattern: self) }
}
extension UnsafeMutablePointer where Pointee == CChar {
    var str: String {
        guard UInt(bitPattern: self) != 0 else { return "" }
        return String(cString: self)
    }
}

extension Notification.Name {
    static let hbVaultChanged = Notification.Name("hb-vault-changed")
}
