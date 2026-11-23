import XCTest
import HarborKit

/// M3 SFTP 纯逻辑：面包屑/路径拆分、RENAME 冲突名、mode 串、目录优先排序。
final class M3SftpTests: XCTestCase {
    // ---------- crumbs / parentDir ----------
    func testCrumbs() {
        XCTAssertEqual(SftpKit.crumbs("/").map(\.label), ["/"])
        let c2 = SftpKit.crumbs("/a/b")
        XCTAssertEqual(c2.map(\.label), ["/", "a", "b"])
        XCTAssertEqual(c2.map(\.path), ["/", "/a", "/a/b"])
        // 连续分隔与尾斜杠
        XCTAssertEqual(SftpKit.crumbs("/a//b/").map(\.path),
                       ["/", "/a", "/a/b"])
    }
    func testParentDir() {
        XCTAssertEqual(SftpKit.parentDir("/"), "/")
        XCTAssertEqual(SftpKit.parentDir("/a"), "/")
        XCTAssertEqual(SftpKit.parentDir("/a/b"), "/a")
        XCTAssertEqual(SftpKit.parentDir("/a/b/"), "/a")
    }
    func testPathJoin() {
        XCTAssertEqual(SftpKit.pathJoin("/", "x"), "/x")
        XCTAssertEqual(SftpKit.pathJoin("/a", "x"), "/a/x")
        XCTAssertEqual(SftpKit.pathJoin("/a/", "x"), "/a/x")
    }

    // ---------- 冲突名（linux ren_variant 同规） ----------
    func testRenVariant() {
        XCTAssertEqual(SftpKit.renVariant("big.bin", isDir: false, 1),
                       "big (1).bin")
        XCTAssertEqual(SftpKit.renVariant("a.b.c", isDir: false, 2),
                       "a.b (2).c")
        XCTAssertEqual(SftpKit.renVariant("sub", isDir: true, 1),
                       "sub (1)")
        XCTAssertEqual(SftpKit.renVariant(".bashrc", isDir: false, 1),
                       ".bashrc (1)")
        XCTAssertEqual(SftpKit.renVariant("noext", isDir: false, 3),
                       "noext (3)")
    }
    func testFirstFreeName() {
        let have: Set<String> = ["f.txt", "f (1).txt", "f (2).txt"]
        XCTAssertEqual(
            SftpKit.firstFreeName("f.txt", isDir: false) { have.contains($0) },
            "f (3).txt")
        XCTAssertEqual(
            SftpKit.firstFreeName("g.txt", isDir: false) { have.contains($0) },
            "g (1).txt")
    }

    // ---------- mode 串 ----------
    func testModeString() {
        XCTAssertEqual(SftpKit.modeString(0o755), "rwxr-xr-x")
        XCTAssertEqual(SftpKit.modeString(0o644), "rw-r--r--")
        XCTAssertEqual(SftpKit.modeString(0o000), "---------")
        // 文件类型位不混入
        XCTAssertEqual(SftpKit.modeString(0o100755), "rwxr-xr-x")
    }

    // ---------- 目录优先排序 ----------
    private struct E {
        let n: String; let d: Bool; let v: Int64
    }
    private func sort(_ es: [E], _ k: SftpKit.SortKey,
                      _ asc: Bool = true) -> [E] {
        SftpKit.sortEntries(es, key: k, ascending: asc,
                            isDir: { $0.d }, name: { $0.n },
                            value: { $0.v })
    }
    func testSortDirsFirst() {
        let es = [E(n: "z.txt", d: false, v: 1),
                  E(n: "adir", d: true, v: 0),
                  E(n: "a.txt", d: false, v: 1)]
        XCTAssertEqual(sort(es, .name).map(\.n),
                       ["adir", "a.txt", "z.txt"])
    }
    func testSortValueWithNameTiebreak() {
        let es = [E(n: "b", d: false, v: 5),
                  E(n: "a", d: false, v: 5),
                  E(n: "c", d: false, v: 1)]
        XCTAssertEqual(sort(es, .size).map(\.n), ["c", "a", "b"])
        XCTAssertEqual(sort(es, .size, false).map(\.n), ["b", "a", "c"])
    }

    // ---------- statPath ----------
    func testStatPath() throws {
        let d = NSTemporaryDirectory() + "hb-m3-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: d,
                                                withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: d) }
        let f = d + "/f"
        FileManager.default.createFile(atPath: f, contents: Data("x".utf8))
        let l = d + "/lnk"
        try FileManager.default.createSymbolicLink(atPath: l,
                                                   withDestinationPath: f)
        guard let sd = SftpKit.statPath(d) else { return XCTFail() }
        XCTAssertTrue(SftpKit.modeIsDir(sd.st_mode))
        guard let sl = SftpKit.statPath(l, follow: false) else {
            return XCTFail()
        }
        XCTAssertTrue(SftpKit.modeIsLnk(sl.st_mode))
        guard let sf = SftpKit.statPath(l, follow: true) else {
            return XCTFail()
        }
        XCTAssertTrue(SftpKit.modeIsReg(sf.st_mode))
        XCTAssertNil(SftpKit.statPath(d + "/nonexistent"))
    }
}
