import XCTest
@testable import HarborKit

/// FR-12 ShareKit 纯逻辑：链接解析/配对码/结束原因。
final class ShareKitTests: XCTestCase {

    private func b64url(_ s: String) -> String {
        Data(s.utf8).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    func testParseLink() {
        let srv = b64url("http://127.0.0.1:8787")
        let sec = b64url("0123456789abcdef")
        let link = "harbor://share/abc-123?s=\(srv)#\(sec)"
        let p = ShareKit.parseLink(link)
        XCTAssertEqual(p?.shareId, "abc-123")
        XCTAssertEqual(p?.server, "http://127.0.0.1:8787")
        XCTAssertTrue(p?.secret == true)
    }

    func testParseLinkNoSecretNoQuery() {
        let p = ShareKit.parseLink("harbor://share/id-9")
        XCTAssertEqual(p?.shareId, "id-9")
        XCTAssertEqual(p?.server, "")
        XCTAssertFalse(p?.secret == true)
    }

    func testParseLinkBad() {
        XCTAssertNil(ShareKit.parseLink(""))
        XCTAssertNil(ShareKit.parseLink("harbor://share/"))
        XCTAssertNil(ShareKit.parseLink("harbor://other/x"))
        XCTAssertNil(ShareKit.parseLink("https://x/share/y"))
    }

    func testB64urlDecode() {
        XCTAssertEqual(ShareKit.b64urlDecode(
            b64url("http://a.b:8787")), "http://a.b:8787")
        XCTAssertNil(ShareKit.b64urlDecode("!!!"))
    }

    func testCodeFmt() {
        XCTAssertEqual(ShareKit.codeFmt("482913"), "482 913")
        XCTAssertEqual(ShareKit.codeFmt("123"), "123")
        XCTAssertEqual(ShareKit.codeFmt(""), "")
    }

    func testCodeValid() {
        XCTAssertTrue(ShareKit.codeValid("482913"))
        XCTAssertFalse(ShareKit.codeValid("48291"))
        XCTAssertFalse(ShareKit.codeValid("4829134"))
        XCTAssertFalse(ShareKit.codeValid("48a913"))
        XCTAssertFalse(ShareKit.codeValid(""))
    }

    func testEndReason() {
        // 对齐 hb-share-page.c show_ended 的映射（中/英由 L() 决定，
        // 这里断言 key 不混）
        XCTAssertNotEqual(ShareKit.endReason("stopped"),
                          ShareKit.endReason("kicked"))
        XCTAssertNotEqual(ShareKit.endReason("owner_gone"),
                          ShareKit.endReason("expired"))
        XCTAssertNotEqual(ShareKit.endReason("locked"),
                          ShareKit.endReason("expired"))
        XCTAssertEqual(ShareKit.endReason(nil),
                       ShareKit.endReason("stopped"))
        XCTAssertEqual(ShareKit.endReason("weird"),
                       ShareKit.endReason("other"))
    }

    func testPermName() {
        XCTAssertNotEqual(ShareKit.permName(true),
                          ShareKit.permName(false))
        XCTAssertNotEqual(ShareKit.partPermName(true),
                          ShareKit.partPermName(false))
    }

    func testWsURL() {
        // 对齐 linux ws_url()：http→ws, https→wss, bare→wss
        XCTAssertEqual(ShareKit.wsURL("http://127.0.0.1:8787", "/x"),
                       "ws://127.0.0.1:8787/x")
        XCTAssertEqual(ShareKit.wsURL("https://h", "/x"), "wss://h/x")
        XCTAssertEqual(ShareKit.wsURL("h:443", "/x"), "wss://h:443/x")
    }
}
