// Zoom sign-in + join status tests (signed 1.0.0 defects, 2026-11-15).
//
// 1. The sign-in callback went to the OBS plugin's helper app, because both
//    products register corevideo:// and LaunchServices picked the other one.
//    The callback now arrives on a one-shot 127.0.0.1 listener (RFC 8252 §7.3).
// 2. A join failure showed "Timed out waiting for Zoom meeting join result."
//    while Zoom's real reason (504, sign-in required) arrived a moment later.

import Foundation
import Network

extension ShellTests {
    // ── loopback: the pure pieces ────────────────────────────────────────────

    static func testOAuthLoopbackParsesTheRequestLine() {
        typealias H = OAuthLoopbackHttp
        expectEqual(H.parseRequestLine("GET /oauth/callback?state=s&broker_token=t HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"),
                    H.RequestLine(method: "GET", path: "/oauth/callback", query: "state=s&broker_token=t"),
                    "the broker redirect splits into path and query")
        expectEqual(H.parseRequestLine("GET /favicon.ico HTTP/1.1\r\n\r\n")?.path, "/favicon.ico",
                    "a favicon request parses (and is not the callback)")
        expectEqual(H.parseRequestLine("GET /oauth/callback HTTP/1.0\r\n\r\n")?.query, "",
                    "no query is an empty query")
        expect(H.parseRequestLine("garbage") == nil, "garbage is not a request")
        expect(H.parseRequestLine("GET oauth/callback HTTP/1.1\r\n\r\n") == nil, "a target must start with /")
        expect(H.parseRequestLine("GET /oauth/callback SMTP\r\n\r\n") == nil, "the version must be HTTP/1.x")
        expect(H.isCallback(H.parseRequestLine("GET /oauth/callback?x=1 HTTP/1.1")), "GET /oauth/callback is the callback")
        expect(!H.isCallback(H.parseRequestLine("POST /oauth/callback HTTP/1.1")), "only GET")
        expect(!H.isCallback(H.parseRequestLine("GET /oauth/callback/x HTTP/1.1")), "the path is exact (broker rule)")
        expect(!H.isCallback(H.parseRequestLine("GET /favicon.ico HTTP/1.1")), "favicon is not the callback")
        expectEqual(H.returnUri(port: 51234), "http://127.0.0.1:51234/oauth/callback",
                    "the return uri is the shape the broker allowlists")
        expectEqual(H.callbackURL(port: 51234, query: "state=s&broker_token=t")?.absoluteString,
                    "http://127.0.0.1:51234/oauth/callback?state=s&broker_token=t",
                    "handleCallback gets the same query")
    }

    static func testOAuthLoopbackPageSaysWhatHappened() {
        let ok = OAuthLoopbackHttp.page(status: "")
        expect(ok.contains("CoreVideo Pro is signed in to Zoom"), "success page names the app")
        expect(ok.contains("close this tab"), "success page says the tab can close")
        let failed = OAuthLoopbackHttp.page(status: "Zoom sign-in failed: <access_denied>")
        expect(failed.contains("did not finish"), "error page says it failed")
        expect(failed.contains("&lt;access_denied&gt;") && !failed.contains("<access_denied>"),
               "the error text is escaped")
        let response = String(decoding: OAuthLoopbackHttp.response(status: 404, reason: "Not Found", body: "x"),
                              as: UTF8.self)
        expect(response.hasPrefix("HTTP/1.1 404 Not Found\r\n") && response.contains("Content-Length: 1\r\n"),
               "response head is well formed")
    }

    static func testOAuthLoopbackLifecycleIsOneShot() {
        let callback = OAuthLoopbackHttp.parseRequestLine("GET /oauth/callback?state=s HTTP/1.1")
        let favicon = OAuthLoopbackHttp.parseRequestLine("GET /favicon.ico HTTP/1.1")
        var life = OAuthLoopbackLifecycle()
        expect(!life.begin(), "first sign-in has nothing to stop")
        let first = life.generation
        expectEqual(life.request(favicon, generation: first), .notFound, "favicon is 404")
        expectEqual(life.phase, .listening, "favicon does not consume the one-shot")
        expectEqual(life.request(callback, generation: first), .callback, "the callback is taken")
        expectEqual(life.request(callback, generation: first), .closed, "a second callback is refused")
        expect(!life.timedOut(generation: first), "a handled sign-in does not time out")
        life.finished(generation: first)
        expectEqual(life.phase, .stopped, "the listener stops after the callback")

        var again = OAuthLoopbackLifecycle()
        _ = again.begin()
        let stale = again.generation
        expect(again.begin(), "a new sign-in stops the listening one")
        expectEqual(again.request(callback, generation: stale), .closed, "the old listener's request is refused")
        expect(!again.timedOut(generation: stale), "the old listener's timer is ignored")
        expect(again.timedOut(generation: again.generation), "the current one times out")
        expectEqual(again.phase, .stopped, "and stops")
        expectEqual(again.request(callback, generation: again.generation), .closed,
                    "nothing is accepted after the timeout")
    }

    static func testOAuthBrokerUrlCarriesTheLoopbackReturnUri() {
        let url = ZoomOAuth.brokerStartURL(state: "abc", returnUri: OAuthLoopbackHttp.returnUri(port: 50001))
        let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
        let value = { (name: String) in items.first { $0.name == name }?.value }
        expectEqual(value("return_uri"), "http://127.0.0.1:50001/oauth/callback",
                    "the broker is told the loopback callback, not corevideo://")
        expectEqual(value("state"), "abc", "state rides along")
        expectEqual(value("redirect_uri"), "https://corevideo.iamfatness.us/oauth/callback",
                    "Zoom still redirects to the broker")
    }

    // ── loopback: over a real socket ─────────────────────────────────────────

    @MainActor
    static func testOAuthLoopbackListenerEndToEnd() {
        final class Seen: @unchecked Sendable {
            let lock = NSLock()
            var urls: [URL] = []
            var timedOut = false
        }
        let seen = Seen()
        let listener = OAuthLoopbackListener()
        let bound = Box<Result<UInt16, Error>>()
        listener.start(onCallback: { url in
            seen.lock.withLock { seen.urls.append(url) }
            return ""
        }, onTimeout: {}, completion: { bound.value = $0 })
        spin(until: { bound.value != nil }, limit: 3)
        guard case .success(let port)? = bound.value, port != 0 else {
            expect(false, "loopback listener binds an ephemeral port: \(String(describing: bound.value))")
            listener.stop()
            return
        }
        if case .hostPort(let host, _)? = OAuthLoopbackListener.listenParameters().requiredLocalEndpoint {
            expectEqual("\(host)", "127.0.0.1", "the listener binds loopback only, never the LAN")
        } else {
            expect(false, "the listener must pin a local endpoint (loopback only)")
        }
        let base = "http://127.0.0.1:\(port)"
        let favicon = http("GET", base + "/favicon.ico")
        expectEqual(favicon.status, 404, "favicon is 404 over the wire")
        expect(listener.isListening, "favicon does not consume the one-shot")
        let callback = http("GET", base + "/oauth/callback?state=S1&broker_token=SECRET")
        expectEqual(callback.status, 200, "the callback is answered")
        expect(callback.body.contains("signed in to Zoom"), "with the signed-in page")
        let urls = seen.lock.withLock { seen.urls }
        expectEqual(urls.count, 1, "the handler ran once")
        let query = urls.first.flatMap { URLComponents(url: $0, resolvingAgainstBaseURL: false)?.queryItems } ?? []
        expectEqual(query.first { $0.name == "state" }?.value, "S1", "the handler got state")
        expectEqual(query.first { $0.name == "broker_token" }?.value, "SECRET", "and the broker token")
        spin(until: { !listener.isListening }, limit: 2)
        expect(!listener.isListening, "the listener stops after the callback")
        let second = http("GET", base + "/oauth/callback?state=S2&broker_token=T2")
        expect(second.status != 200, "a second callback is not served (status \(second.status))")
        expectEqual(seen.lock.withLock { seen.urls.count }, 1, "the handler never runs twice")

        // Timeout: no callback within the window stops the listener and says so.
        let slow = OAuthLoopbackListener()
        let slowBound = Box<Result<UInt16, Error>>()
        slow.start(timeout: 0.2, onCallback: { _ in "" },
                   onTimeout: { seen.lock.withLock { seen.timedOut = true } },
                   completion: { slowBound.value = $0 })
        spin(until: { seen.lock.withLock { seen.timedOut } }, limit: 3)
        expect(seen.lock.withLock { seen.timedOut }, "the timeout fires")
        expect(!slow.isListening, "and the listener is stopped")
        listener.stop()
    }

    // ── join status: late truth replaces the timeout ─────────────────────────

    static func testZoomJoinStatusPrefersZoomsReasonOverATimeout() {
        typealias Z = ZoomJoinStatus
        let timeout = "Timed out waiting for Zoom meeting join result."
        let reason = "Zoom requires you to sign in to join this meeting. Use Sign in with Zoom, then join again."
        let timedOut: JSONObject = ["meetingState": "error", "warnings": [timeout]]
        let truth: JSONObject = ["meetingState": "error", "warnings": [reason]]
        let both: JSONObject = ["meetingState": "error", "warnings": [reason, timeout]]

        expectEqual(Z.atJoin(signedIn: false), Z.signInMayBeRequired, "not signed in: say sign-in may be required")
        expect(Z.signInMayBeRequired.contains("sign in"), "the hint names sign-in")
        expectEqual(Z.atJoin(signedIn: true), "", "signed in: no hint")

        var status = Z.atJoin(signedIn: false)
        status = Z.reconcile(current: status, snapshot: ["meetingState": "joining"])
        expectEqual(status, Z.signInMayBeRequired, "the hint survives while joining")
        status = Z.reconcile(current: status, snapshot: timedOut)
        expectEqual(status, timeout, "a bare timeout shows the timeout")
        status = Z.reconcile(current: status, snapshot: truth)
        expectEqual(status, reason, "the later truth REPLACES the timeout")
        expectEqual(Z.reconcile(current: "", snapshot: both), reason,
                    "Zoom's reason wins over a timeout in the same snapshot")
        status = Z.reconcile(current: status, snapshot: ["meetingState": "idle"])
        expectEqual(status, reason, "the reason stays up until the next join")
        expectEqual(Z.reconcile(current: status, snapshot: ["meetingState": "in_meeting"]), "",
                    "a joined meeting clears it")
        expectEqual(Z.reconcile(current: "", snapshot: ["meetingState": "error", "warnings": ["meeting_failed: MEETING_FAIL_X"]]),
                    "meeting_failed: MEETING_FAIL_X", "an unmapped reason is shown raw")
        expect(Z.isTimeout("Timed out waiting for Zoom SDK authentication."), "the auth timeout is a timeout too")
        expectEqual(Z.coreJoinWaitMs, "45000", "the Mac core waits as long as the Windows core")
        expect(Z.joinRequestTimeout > 90, "the bridge outlasts the auth wait plus the join wait")
    }
}
