import AppKit
import CHarbor
import HarborKit

// ---------- 侧栏分区（Linux 九区顺序）----------
struct Section { let id: String; let icon: String; let title: String }
let kSections: [Section] = [
    .init(id: "hosts",      icon: "server",   title: L("主机")),
    .init(id: "keychain",   icon: "key",      title: L("密钥")),
    .init(id: "fwd",        icon: "fwd",      title: L("端口转发")),
    .init(id: "snippets",   icon: "list",     title: L("命令片段")),
    .init(id: "knownhosts", icon: "fingerprint", title: L("已知主机")),
    .init(id: "logs",       icon: "clock",    title: L("日志")),
    .init(id: "shared",     icon: "broadcast", title: L("共享会话")),
    .init(id: "appearance", icon: "palette",  title: L("外观与终端")),
    .init(id: "account",    icon: "user",     title: L("账户")),
]

// ---------- 图标渲染：IconPath → tinted NSImage ----------
func hbIcon(_ name: String, size: CGFloat, tint: NSColor) -> NSImage {
    let img = NSImage(size: NSSize(width: size, height: size))
    img.lockFocus()
    let ctx = NSGraphicsContext.current!.cgContext
    ctx.translateBy(x: 0, y: size)
    ctx.scaleBy(x: size/24, y: -size/24)
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
    img.unlockFocus()
    img.isTemplate = false
    return img
}

let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
