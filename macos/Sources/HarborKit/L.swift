import Foundation

/// UI 文案统一入口：msgid = 中文源串。英文目录在 M9 接入；
/// 现在 NSLocalizedString 无命中即原样返回中文。
public func L(_ zh: String) -> String {
    NSLocalizedString(zh, bundle: .main, comment: "")
}
