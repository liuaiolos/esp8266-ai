import XCTest
@testable import AIClockBridge

final class StatusServiceTests: XCTestCase {
    func testCodexStopRemainsIdleWhenSessionLogIsFresh() {
        let service = StatusService()

        service.recordEvent(agent: "codex", event: "UserPromptSubmit")
        XCTAssertEqual(service.snapshot().codex.status, "working")

        // Codex writes final transcript records after Stop. The event must
        // remain authoritative instead of falling back to the fresh mtime.
        service.recordEvent(agent: "codex", event: "Stop")
        XCTAssertEqual(service.snapshot().codex.status, "idle")
    }

    func testCodexPermissionStopsWorkAnimationAndRaisesAttention() {
        let service = StatusService()
        service.recordEvent(agent: "codex", event: "PermissionRequest")

        let snapshot = service.snapshot().codex
        XCTAssertEqual(snapshot.status, "idle")
        XCTAssertTrue(snapshot.needsInput)
    }
}
