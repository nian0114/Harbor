import Foundation

/// M1a 误造的 macOS 私有设置键 → 跨平台共享键（见 docs/dev/data-model.md）。
/// vault 打开时一次性迁移：新键缺失才拷贝，旧键一律删除。
public enum SettingsMigration {
    public static let termKeys: [(old: String, new: String)] = [
        ("term_font", "font_family"),
        ("term_font_size", "font_size"),
        ("term_line_height", "line_height"),
        ("term_theme", "theme"),
        ("term_cursor", "cursor_style"),
        ("term_scrollback", "scrollback"),
        ("term_option_meta", "option_meta"),
    ]

    /// 纯逻辑版（测试用）：对设置字典做同样的迁移。
    public static func migrateTermKeys(
        _ d: [String: String]
    ) -> [String: String] {
        var d = d
        for (o, n) in termKeys where d[n] == nil {
            if let v = d[o] { d[n] = v }
        }
        for (o, _) in termKeys { d.removeValue(forKey: o) }
        return d
    }
}
