// swift-tools-version:5.10
// Harbor macOS — M1a。Swift 5 语言模式（Swift 6.3 编译器）。
// 无任何机器绝对路径：core 经 ../core 相对引用，链接参数由
// scripts/build-macos-app.sh 从 M0 link-line.txt 注入 -Xlinker。
import PackageDescription

let package = Package(
    name: "Harbor",
    platforms: [.macOS(.v13)],
    targets: [
        .systemLibrary(name: "CHarbor", path: "Sources/CHarbor"),
        .target(name: "HarborKit",
                dependencies: ["CHarbor"],
                path: "Sources/HarborKit"),
        .executableTarget(name: "Harbor",
                          dependencies: ["HarborKit"],
                          path: "Sources/Harbor"),
        .testTarget(name: "HarborTests",
                    dependencies: ["HarborKit"],
                    path: "Tests/HarborTests"),
    ]
)
