import Foundation

/// UI 文案统一入口：msgid = 中文源串；en 目录由 build-macos-app.sh
/// 把 po/en.po 转成 bundle 内 en.strings。运行时按 settings.language
/// 解析：en → 查表；zh/无表 → 原样返回中文。语言切换需重启生效
/// （linux 同：LANGUAGE 环境变量在进程启动时装配）。
public enum Lang {
    /// 已解析的翻译表；nil = 中文（msgid 直通）。
    public private(set) static var table: [String: String]?
    /// C 指针缓存：给 core hb_set_translator 的返回必须长存，
    /// strdup 出的表只增不删（表小，重启前不会重载）。
    private static var tableC: [String: UnsafePointer<CChar>] = [:]

    /// settings.language → 有效语言 "zh"|"en"。
    /// 跟随系统：preferredLanguages 含中文 → zh，其余 → en。
    public static func effective(_ setting: String?) -> String {
        switch setting {
        case "en": return "en"
        case "zh": return "zh"
        default:
            return Locale.preferredLanguages
                .contains { $0.hasPrefix("zh") } ? "zh" : "en"
        }
    }

    /// bundle 里的 en 表（Resources/en.strings，OpenStep plist）。
    public static func loadEn() -> [String: String]? {
        guard let u = Bundle.main.url(forResource: "en",
                                      withExtension: "strings"),
              let d = NSDictionary(contentsOf: u) as? [String: String],
              !d.isEmpty
        else { return nil }
        return d
    }

    /// 应用语言：生效语言为 en → 装表（含 C 指针缓存），否则置空。
    public static func apply(_ setting: String?) {
        table = effective(setting) == "en" ? loadEn() : nil
        var tc: [String: UnsafePointer<CChar>] = [:]
        for (k, v) in table ?? [:] {
            tc[k] = UnsafePointer(strdup(v))
        }
        tableC = tc
    }

    /// core hb_set_translator 的查表实现：命中 → 长存 C 串，否则 NULL。
    public static func cTranslate(_ msgid: UnsafePointer<CChar>?)
        -> UnsafePointer<CChar>? {
        guard let msgid else { return nil }
        return tableC[String(cString: msgid)]
    }
}

public func L(_ zh: String) -> String {
    Lang.table?[zh] ?? zh
}
