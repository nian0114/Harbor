import Foundation

/// 图标数据 —— 与 linux/src/hb-icons.c ICON_PATHS 同源（prototype/app.js）。
public enum Icons {
    public static let table: [String: String] = [
        "server": "<rect x=\"3\" y=\"4\" width=\"18\" height=\"7\" rx=\"2\"/><rect x=\"3\" y=\"13\" width=\"18\" height=\"7\" rx=\"2\"/><path d=\"M7 7.5h.01M7 16.5h.01\"/>",
        "key": "<circle cx=\"8\" cy=\"15.5\" r=\"4\"/><path d=\"M10.9 12.6 20 3.5M15 8.5l3 3M18 5.5l2 2\"/>",
        "fwd": "<path d=\"M4 7.5h13.5l-3.2-3.2M20 16.5H6.5l3.2 3.2\"/>",
        "fingerprint": "<path d=\"M6.5 11a5.5 5.5 0 0 1 11 0v3.5M9.2 11a2.8 2.8 0 0 1 5.6 0v4a7 7 0 0 1-1 3.6M12 11v5.2a10 10 0 0 1-1.4 4.6M6.5 14.5a13 13 0 0 1-.7 3.7M17.5 17a12 12 0 0 1-.6 2.6M4 8.5A8.5 8.5 0 0 1 12 4a8.4 8.4 0 0 1 6.6 3.2\"/>",
        "clock": "<circle cx=\"12\" cy=\"12\" r=\"8.6\"/><path d=\"M12 7.2v4.8l3.1 2\"/>",
        "broadcast": "<circle cx=\"12\" cy=\"12\" r=\"2\"/><path d=\"M8.6 8.6a4.8 4.8 0 0 0 0 6.8M15.4 8.8a4.8 4.8 0 0 1 0 6.8M5.8 5.8a8.8 8.8 0 0 0 0 12.4M18.2 5.8a8.8 8.8 0 0 1 0 12.4\"/>",
        "cloud": "<path d=\"M7 18a4.5 4.5 0 1 1 .7-8.95A6 6 0 0 1 19.2 10.6 3.8 3.8 0 0 1 18.3 18H7z\"/>",
        "bell": "<path d=\"M18 9.5a6 6 0 1 0-12 0c0 5.5-2.3 6.8-2.3 6.8h16.6S18 15 18 9.5M10.4 19.8a1.9 1.9 0 0 0 3.2 0\"/>",
        "panel": "<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2.5\"/><path d=\"M9.5 4v16\"/>",
        "search": "<circle cx=\"11\" cy=\"11\" r=\"6.8\"/><path d=\"m20.3 20.3-4.4-4.4\"/>",
        "grid": "<rect x=\"4\" y=\"4\" width=\"7\" height=\"7\" rx=\"1.8\"/><rect x=\"13\" y=\"4\" width=\"7\" height=\"7\" rx=\"1.8\"/><rect x=\"4\" y=\"13\" width=\"7\" height=\"7\" rx=\"1.8\"/><rect x=\"13\" y=\"13\" width=\"7\" height=\"7\" rx=\"1.8\"/>",
        "tag": "<path d=\"M3.5 10.8V4.5a1 1 0 0 1 1-1h6.3L20.5 13l-7.5 7.5L3.5 10.8z\"/><circle cx=\"8\" cy=\"8\" r=\"1.4\"/>",
        "list": "<path d=\"M8.5 6h12M8.5 12h12M8.5 18h12M3.8 6h.01M3.8 12h.01M3.8 18h.01\"/>",
        "plug": "<path d=\"M9 3v5M15 3v5M6 8h12v3a6 6 0 0 1-12 0V8zM12 17v4\"/>",
        "terminal": "<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2.5\"/><path d=\"m7 9 3 3-3 3M13 15h4\"/>",
        "monitor": "<rect x=\"3\" y=\"4\" width=\"18\" height=\"12\" rx=\"2\"/><path d=\"M9 20h6M12 16v4\"/>",
        "x": "<path d=\"M6 6l12 12M18 6 6 18\"/>",
        "check": "<path d=\"m4.8 12.6 4.8 4.8L19.4 6.6\"/>",
        "user": "<circle cx=\"12\" cy=\"8\" r=\"3.8\"/><path d=\"M4.5 20.5c1.4-3.8 4.2-5.7 7.5-5.7s6.1 1.9 7.5 5.7\"/>",
        "palette": "<path d=\"M12 3a9 9 0 1 0 0 18h1.6a2 2 0 0 0 1.4-3.4c-.8-.8-.2-2.1 1-2.1h1.9A3.6 3.6 0 0 0 21 11.6 9 9 0 0 0 12 3z\"/><circle cx=\"8\" cy=\"10\" r=\"1.1\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"12\" cy=\"7.3\" r=\"1.1\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"16\" cy=\"10\" r=\"1.1\" fill=\"currentColor\" stroke=\"none\"/>",
        "refresh": "<path d=\"M20.5 12a8.5 8.5 0 1 1-2.5-6M20.5 3.5V8H16\"/>",
        "plus": "<path d=\"M12 5v14M5 12h14\"/>",
        "chevron-d": "<path d=\"m6 9 6 6 6-6\"/>",
        "chevron-r": "<path d=\"m9 6 6 6-6 6\"/>",
        "dots": "<circle cx=\"5\" cy=\"12\" r=\"1.6\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"12\" cy=\"12\" r=\"1.6\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"19\" cy=\"12\" r=\"1.6\" fill=\"currentColor\" stroke=\"none\"/>",
        "info": "<circle cx=\"12\" cy=\"12\" r=\"8.6\"/><path d=\"M12 11v5M12 7.8h.01\"/>",
        "gear": "<circle cx=\"12\" cy=\"12\" r=\"3.2\"/><path d=\"M19.4 13.5a7.5 7.5 0 0 0 0-3l2-1.6-2-3.4-2.4 1a7.6 7.6 0 0 0-2.6-1.5L14 2.5h-4L9.6 5a7.6 7.6 0 0 0-2.6 1.5l-2.4-1-2 3.4 2 1.6a7.5 7.5 0 0 0 0 3l-2 1.6 2 3.4 2.4-1a7.6 7.6 0 0 0 2.6 1.5L10 21.5h4l.4-2.5a7.6 7.6 0 0 0 2.6-1.5l2.4 1 2-3.4z\"/>",
    ]
    public static func icon(_ name: String) -> IconPath {
        IconPath(svgFragment: table[name] ?? "")
    }
}
