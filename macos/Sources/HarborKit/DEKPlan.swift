import Foundation

/// Vault DEK 获取决策（纯逻辑，可测试）。
/// 输入 = Keychain 读取状态 × vault.key 是否存在 × vault.db 是否存在。
public enum DEKAction: Equatable {
    case useKeychain          // Keychain 里已有可用 DEK
    case useFile              // 用 0600 vault.key 里的 DEK
    case createNew            // 全新安装：生成 DEK 并尝试持久化
    case failKeychain(Int32)  // Keychain 读取出错：禁止新建/覆盖任何东西
    case failKeyUnavailable   // vault.db 存在但拿不到 key：禁止重新生成
}

public enum DEKPlan {
    /// chainStatus: SecItemCopyMatching 的 OSStatus（errSecSuccess/-25300…）。
    /// chainHasKey: 成功且数据长度 == 32。
    /// fileKey/dbExists: vault.key / vault.db 是否存在。
    /// fileMode: DEBUG 的 HARBOR_DEK_FILE=1。
    public static func decide(chainStatus: Int32,
                              chainHasKey: Bool,
                              fileKey: Bool,
                              dbExists: Bool,
                              fileMode: Bool) -> DEKAction {
        if fileMode {
            if fileKey { return .useFile }
            return dbExists ? .failKeyUnavailable : .createNew
        }
        // errSecSuccess 且数据完整
        if chainStatus == 0 && chainHasKey { return .useKeychain }
        // errSecItemNotFound(-25300) 或 success-但-数据不可用：视为“链里没有”
        if chainStatus == 0 || chainStatus == -25300 {
            if fileKey { return .useFile }
            return dbExists ? .failKeyUnavailable : .createNew
        }
        // 其他一切状态（拒绝交互/ACL 拒绝/取消/损坏…）都先报错
        return .failKeychain(chainStatus)
    }

    /// OSStatus → 用户可读的一行解释。
    public static func describe(_ status: Int32) -> String {
        switch status {
        case -25300: return L("Keychain 中没有保存的密钥（errSecItemNotFound）")
        case -25299: return L("Keychain 条目已存在（errSecDuplicateItem）")
        case -25308: return L("当前会话不允许 Keychain 交互（errSecInteractionNotAllowed）")
        case -25293: return L("Keychain 认证失败（errSecAuthFailed）")
        case -128:   return L("用户取消了 Keychain 授权（errSecUserCanceled）")
        case -25303: return L("Keychain 条目属性不可读（errSecDecode）")
        case -34018: return L("缺少 Keychain 访问权限（errSecMissingEntitlement）")
        default:     return L("Keychain 返回错误 OSStatus \(status)")
        }
    }
}
