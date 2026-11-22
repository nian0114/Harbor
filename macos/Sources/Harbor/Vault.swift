import Foundation
import Security
import CHarbor

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
        var dek = Array(loadDEK())
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
    }

    // ---- DEK ----
    private var dekAccount: String { "vault-dek:" + dataDir.path }
    private func keychainGet() -> Data? {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "dev.harbor.Harbor.vault",
            kSecAttrAccount as String: dekAccount,
            kSecReturnData as String: true,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &item) == errSecSuccess,
              let d = item as? Data, d.count == 32 else { return nil }
        return d
    }
    private func keychainSet(_ d: Data) -> Bool {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "dev.harbor.Harbor.vault",
            kSecAttrAccount as String: dekAccount,
        ]
        SecItemDelete(q as CFDictionary)
        var add = q
        add[kSecValueData as String] = d
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }
    private var keyFile: URL {
        dataDir.appendingPathComponent("vault.key") }
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
    private func loadDEK() -> Data {
        #if DEBUG
        if ProcessInfo.processInfo.environment["HARBOR_DEK_FILE"] == "1" {
            // ad-hoc 重签导致 keychain ACL 每次都弹窗：DEBUG 下允许绕开。
            if let d = fileDEK() { dekFallback = true; return d }
            let d = genDEK(); _ = fileDEKSet(d); dekFallback = true; return d
        }
        #endif
        if let d = keychainGet() { return d }
        if let d = fileDEK() { dekFallback = true; return d }
        let d = genDEK()
        if keychainSet(d) { return d }
        if fileDEKSet(d) { dekFallback = true; return d }
        return d
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
}
