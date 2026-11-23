import AppKit
import CHarbor
import HarborKit

/// 会话侧「助手栏」右侧面板 —— 对齐 prototype workspaceView 的
/// assist 栏：标题「Snippets 快捷发送」+ 每个 snippet 一个 tbtn，
/// 点击经 onSend 走共享发送流（变量/broadcast 与 Snippets 页一致）。
final class AssistPanel: NSView {
    var onSend: ((String) -> Void)?
    private let stack = NSStackView()
    private var items: [OpaquePointer] = []

    override init(frame f: NSRect) {
        super.init(frame: f)
        wantsLayer = true
        layer?.backgroundColor = Theme.subtle.color.cgColor
        layer?.borderWidth = 1
        layer?.borderColor = Theme.line.color.cgColor
        let head = NSTextField(labelWithString: L("Snippets 快捷发送"))
        head.font = .systemFont(ofSize: 11.5)
        head.textColor = Theme.termDim.color
        head.translatesAutoresizingMaskIntoConstraints = false
        addSubview(head)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        let scroll = NSScrollView()
        scroll.documentView = stack
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        addSubview(scroll)
        NSLayoutConstraint.activate([
            head.topAnchor.constraint(equalTo: topAnchor, constant: 10),
            head.leadingAnchor.constraint(equalTo: leadingAnchor,
                                          constant: 10),
            scroll.topAnchor.constraint(equalTo: head.bottomAnchor,
                                        constant: 8),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            stack.leadingAnchor.constraint(equalTo: scroll.leadingAnchor,
                                           constant: 8),
            stack.trailingAnchor.constraint(equalTo: scroll.trailingAnchor,
                                            constant: -8),
            stack.topAnchor.constraint(equalTo: scroll.topAnchor),
            stack.widthAnchor.constraint(
                equalTo: scroll.widthAnchor, constant: -16),
        ])
        reload()
    }
    required init?(coder: NSCoder) { fatalError() }

    func reload() {
        for o in items { hb_obj_free(o) }
        items = Vault.shared.list("snippet")
        stack.arrangedSubviews.forEach { stack.removeArrangedSubview($0); $0.removeFromSuperview() }
        for o in items {
            let id = String(cString: hb_obj_id(o))
            let title = hb_obj_get_str(o, "title").str
            let b = NSButton()
            b.bezelStyle = .rounded
            b.font = .systemFont(ofSize: 11.5)
            b.alignment = .left
            b.attributedTitle = NSAttributedString(
                string: "{ } \(title.isEmpty ? "snippet" : title)",
                attributes: [.font: NSFont.systemFont(ofSize: 11.5)])
            b.setAccessibilityIdentifier("assist-snip-" + id)
            b.tag = items.firstIndex(where: {
                String(cString: hb_obj_id($0)) == id }) ?? 0
            b.target = self
            b.action = #selector(send(_:))
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(equalToConstant: 188).isActive = true
            stack.addArrangedSubview(b)
        }
        if items.isEmpty {
            let l = NSTextField(labelWithString: L("暂无 Snippet"))
            l.font = .systemFont(ofSize: 11)
            l.textColor = Theme.termDim.color
            stack.addArrangedSubview(l)
        }
    }

    @objc private func send(_ b: NSButton) {
        guard b.tag >= 0, b.tag < items.count else { return }
        onSend?(String(cString: hb_obj_id(items[b.tag])))
    }

    deinit { for o in items { hb_obj_free(o) } }
}
