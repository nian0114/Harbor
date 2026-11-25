import AppKit
import CHarbor
import HarborKit

// ---------- 侧栏分区（Linux 九区顺序）----------
struct Section { let id: String; let icon: String; let title: String }
// 与 linux/src/hb-vault-page.c 同 msgid：同一概念同一翻译源。
var kSections: [Section] { [
    .init(id: "hosts",      icon: "server",   title: "Hosts"),
    .init(id: "keychain",   icon: "key",      title: "Keychain"),
    .init(id: "fwd",        icon: "fwd",      title: "Port Forwarding"),
    .init(id: "snippets",   icon: "list",     title: "Snippets"),
    .init(id: "knownhosts", icon: "fingerprint", title: "Known Hosts"),
    .init(id: "logs",       icon: "clock",    title: "Logs"),
    .init(id: "shared",     icon: "broadcast", title: L("共享会话")),
    .init(id: "appearance", icon: "palette",  title: L("外观与终端")),
    .init(id: "account",    icon: "user",     title: L("账号与同步")),
] }

// ---------- 图标渲染：IconPath → tinted NSImage ----------
func hbIcon(_ name: String, size: CGFloat, tint: NSColor) -> NSImage {
    // 惰性绘制：tint 多为 Theme 动态色，必须按绘制时的
    // effectiveAppearance 解析——lockFocus 会把浅色态的解析结果
    // 固化进位图，切深色后图标近不可见。
    return NSImage(size: NSSize(width: size, height: size),
                   flipped: false) { rect in
        guard let ctx = NSGraphicsContext.current?.cgContext
        else { return false }
        ctx.translateBy(x: 0, y: rect.height)
        ctx.scaleBy(x: rect.height/24, y: -rect.height/24)
        let ip = Icons.icon(name)
        tint.set()
        for s in ip.subs {
            if s.fill {
                ctx.addPath(s.path); ctx.fillPath()
            } else {
                ctx.addPath(s.path)
                ctx.setLineWidth(1.6); ctx.setLineCap(.round)
                ctx.setLineJoin(.round); ctx.strokePath()
            }
        }
        return true
    }
}

let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
