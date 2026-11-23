// YouTube transport and feed, plan 2026-11-22 Task 15: ports of
// native-shell/CoreVideoPro.MediaCore.Tests/YouTubeChatTransportTests.cs (the
// JSON bodies are the C# literals verbatim) plus the Mac's redirect and
// Keychain checks.

import Foundation
import Network

extension ShellTests {
    static var socialYouTubeCases: [(String, () -> Void)] {
        [
            ("social/youtube-requests-use-fixed-tls-host-and-map-kinds", testYouTubeRequestsAndKinds),
            ("social/youtube-failures-classified-without-exposing-key", testYouTubeFailuresClassified),
            ("social/youtube-wrong-source-or-missing-removal-target-fails-closed", testYouTubeMissingRemovalTarget),
            ("social/youtube-cursor-advances-and-reconnect-requires-review", testYouTubeCursorAndReconnect),
            ("social/youtube-redirect-is-not-followed", testYouTubeRedirectNotFollowed),
            ("social/youtube-oversized-body-fails-closed", testYouTubeOversizedBody),
            ("social/redaction-sentinel-youtube-key-through-a-feed", testYouTubeSentinelThroughFeed),
        ]
    }

    static let ytKey = "fixture-api-key-abcdefghijklmnop"

    final class ListenerReady: @unchecked Sendable { var flag = false }

    /// A scripted transport: each request is checked, then answered in order.
    final class ScriptedHTTP: SocialHTTP, @unchecked Sendable {
        var responses: [(Int, String)]
        var requests: [SocialHTTPRequest] = []
        var check: (SocialHTTPRequest, Int) -> Void = { _, _ in }
        init(_ responses: [(Int, String)]) { self.responses = responses }
        func send(_ request: SocialHTTPRequest, deadline: TimeInterval) async throws -> SocialHTTPResponse {
            requests.append(request)
            check(request, responses.count)
            guard !responses.isEmpty else { throw SocialFailure.retry }
            let (status, body) = responses.removeFirst()
            return SocialHTTPResponse(status: status, body: Data(body.utf8))
        }
    }

    // YouTubeChatTransportTests.cs:13-30 literals.
    static let ytLookup = #"{"items":[{"id":"abcDEF123_4","status":{"privacyStatus":"public"},"liveStreamingDetails":{"activeLiveChatId":"chat-id"}}]}"#
    static let ytPage = #"""
    {"pollingIntervalMillis":21000,"nextPageToken":"next+/","items":[
      {"id":"message","snippet":{"type":"textMessageEvent","liveChatId":"chat-id","authorChannelId":"channel","textMessageDetails":{"messageText":"Synthetic text"}},"authorDetails":{"displayName":"Synthetic author"}},
      {"id":"delete-event","snippet":{"type":"messageDeletedEvent","liveChatId":"chat-id","messageDeletedDetails":{"deletedMessageId":"deleted"}}},
      {"id":"retract-event","snippet":{"type":"messageRetractedEvent","liveChatId":"chat-id","messageRetractedDetails":{"retractedMessageId":"retracted"}}},
      {"id":"tombstone","snippet":{"type":"tombstone","liveChatId":"chat-id"}},
      {"id":"ban","snippet":{"type":"userBannedEvent","liveChatId":"chat-id","userBannedDetails":{"bannedUserDetails":{"channelId":"banned-author"}}}},
      {"id":"ignored","snippet":{"type":"superChatEvent","liveChatId":"chat-id"}}
    ]}
    """#

    @MainActor static func testYouTubeRequestsAndKinds() {
        let http = ScriptedHTTP([(200, ytLookup), (200, ytPage)])
        http.check = { request, remaining in
            expectEqual(request.url.host, "www.googleapis.com", "fixed host")
            expectEqual(request.url.scheme, "https", "TLS")
            expect(!request.url.absoluteString.contains(ytKey), "the key is never in the URL")
            expectEqual(request.headers["x-goog-api-key"], ytKey, "the key rides the header")
            if remaining == 1 { expect(request.url.absoluteString.contains("pageToken=previous%2B%2F"), "cursor escaped") }
        }
        let chat = awaitMain { (try? await YouTubeChatTransport.resolve(videoId: "abcDEF123_4", key: ytKey, http: http)) ?? "" }
        expectEqual(chat, "chat-id", "resolved")
        let fetched: YouTubeChatTransport.Page?? = awaitMain { try? await YouTubeChatTransport.page(chatId: "chat-id", cursor: "previous+/", key: ytKey, http: http) }
        guard let page = fetched ?? nil else { expect(false, "page"); return }
        expectEqual(page.next, "next+/", "cursor")
        expectEqual(page.waitSeconds, 21, "21000 ms interval")
        let expected: [SocialFeedEvent] = [
            .text(id: "message", author: "Synthetic author", message: "Synthetic text", authorId: "channel", order: 0),
            .remove(id: "deleted"), .remove(id: "retracted"), .remove(id: "tombstone"), .ban(authorId: "banned-author"),
        ]
        expectEqual(page.events, expected, "kinds mapped; superChatEvent ignored")
        expect(http.responses.isEmpty, "both requests made")
    }

    @MainActor static func testYouTubeFailuresClassified() {
        let cases: [(Int, String, SocialFailure)] = [
            (302, "", .authorization), (401, "", .authorization), (403, "quotaExceeded", .retry),
            (403, "liveChatEnded", .ended), (400, "invalidPageToken", .retry),
        ]
        for (status, reason, expected) in cases {
            let body = #"{"error":{"message":"\#(ytKey)","errors":[{"reason":"\#(reason)"}]}}"#
            let http = ScriptedHTTP([(status, body)])
            var caught: Error?
            _ = awaitMain {
                do { _ = try await YouTubeChatTransport.page(chatId: "chat-id", cursor: nil, key: ytKey, http: http) }
                catch { caught = error }
                return true
            }
            expectEqual(caught as? SocialFailure, expected, "\(status) \(reason)")
            expect(!String(describing: caught as Any).contains(ytKey), "\(status): the error never contains the key")
            expect(!YouTubeChatTransport.message(expected).contains(ytKey), "the message is fixed text")
        }
    }

    @MainActor static func testYouTubeMissingRemovalTarget() {
        let missing = #"{"pollingIntervalMillis":1000,"nextPageToken":"next","items":[{"id":"event","snippet":{"type":"messageDeletedEvent","liveChatId":"chat-id"}}]}"#
        let wrongChat = #"{"pollingIntervalMillis":1000,"nextPageToken":"next","items":[{"id":"m","snippet":{"type":"textMessageEvent","liveChatId":"other","authorChannelId":"a","textMessageDetails":{"messageText":"t"}},"authorDetails":{"displayName":"A"}}]}"#
        for body in [missing, wrongChat] {
            let http = ScriptedHTTP([(200, body)])
            var caught: Error?
            _ = awaitMain {
                do { _ = try await YouTubeChatTransport.page(chatId: "chat-id", cursor: nil, key: ytKey, http: http) }
                catch { caught = error }
                return true
            }
            expectEqual(caught as? SocialFailure, .invalid, "fails closed")
        }
    }

    @MainActor static func testYouTubeCursorAndReconnect() {
        let first = #"{"pollingIntervalMillis":1000,"nextPageToken":"cursor","items":[{"id":"msg","snippet":{"type":"textMessageEvent","liveChatId":"chat-id","authorChannelId":"author-id","textMessageDetails":{"messageText":"Text"}},"authorDetails":{"displayName":"Author"}}]}"#
        let http = ScriptedHTTP([(200, ytLookup), (200, first), (503, ""), (200, ytLookup), (200, first)])
        // Approval alone sends no native command: any invoke fails the test.
        let review = SocialPlayout(invoke: { _, _ in expect(false, "approval sent a native command"); return GraphicsOperationResult() },
                                   observe: { nil }, sleep: { _ in }, now: { socialNow })
        var waits: [Double] = []
        var previous: SocialItemKey?
        let feed = Task { @MainActor in
            await YouTubeSocialFeed.run(videoId: "abcDEF123_4", key: ytKey, playout: review, http: http, sleep: { ns in
                waits.append(Double(ns) / 1e9)
                switch waits.count {
                case 1:
                    previous = review.items.first?.key
                    expect(previous.map { review.approve($0) } == true, "approve the first message")
                case 2:
                    expect(!review.isConnected, "a retry disconnects")
                    expect(review.items.isEmpty, "and clears decisions")
                case 3:
                    expectEqual(review.items.map(\.decision), [.pending], "the replayed message needs review again")
                    expect(previous.map { !review.approve($0) } ?? false, "the old approval cannot be reused")
                    withUnsafeCurrentTask { $0?.cancel() }
                default: break
                }
            }, report: { _ in })
        }
        _ = awaitMain(5) { await feed.value; return true }
        expectEqual(waits, [15, 30, 15], "15 s floor, 30 s backoff, 15 s")
        expect(http.requests.count >= 3 && http.requests[2].url.absoluteString.contains("pageToken=cursor"), "the cursor advanced")
        expect(http.requests.count == 5 && !http.requests[4].url.absoluteString.contains("pageToken="), "a reconnect starts fresh")
        expect(!review.isConnected && review.items.isEmpty, "the feed leaves nothing behind")
    }

    /// URLSessionSocialHTTP refuses redirects: the 302 is the response and the
    /// target is never requested (local NWListener).
    @MainActor static func testYouTubeRedirectNotFollowed() {
        final class Hits: @unchecked Sendable { var paths: [String] = []; let lock = NSLock() }
        let hits = Hits()
        guard let listener = try? NWListener(using: .tcp, on: .any) else { expect(false, "listener"); return }
        listener.newConnectionHandler = { conn in
            conn.start(queue: .global())
            conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { data, _, _, _ in
                let head = String(decoding: data ?? Data(), as: UTF8.self)
                let path = head.split(separator: " ").dropFirst().first.map(String.init) ?? ""
                hits.lock.lock(); hits.paths.append(path); hits.lock.unlock()
                let reply = path == "/start"
                    ? "HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                    : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"
                conn.send(content: Data(reply.utf8), completion: .contentProcessed { _ in conn.cancel() })
            }
        }
        let ready = ListenerReady()
        listener.stateUpdateHandler = { if case .ready = $0 { ready.flag = true } }
        listener.start(queue: .global())
        defer { listener.cancel() }
        spin(until: { ready.flag }, limit: 3)
        guard let port = listener.port?.rawValue, let url = URL(string: "http://127.0.0.1:\(port)/start") else {
            expect(false, "listener port"); return
        }
        let http = URLSessionSocialHTTP()
        let response = awaitMain(5) { try? await http.send(SocialHTTPRequest(method: "GET", url: url), deadline: 4) } ?? nil
        expectEqual(response?.status, 302, "the redirect is the response")
        spin(until: { false }, limit: 0.3)
        hits.lock.lock(); let paths = hits.paths; hits.lock.unlock()
        expectEqual(paths, ["/start"], "/elsewhere is never requested")
    }

    @MainActor static func testYouTubeOversizedBody() {
        let big = #"{"items":[{"id":""# + String(repeating: "x", count: URLSessionSocialHTTP.bodyLimit) + #""}]}"#
        guard let listener = try? NWListener(using: .tcp, on: .any) else { expect(false, "listener"); return }
        listener.newConnectionHandler = { conn in
            conn.start(queue: .global())
            conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { _, _, _, _ in
                // No Content-Length: the body is delimited by the close, so only the
                // streaming cap can stop it (a declared length is refused up front).
                let reply = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + big
                conn.send(content: Data(reply.utf8), completion: .contentProcessed { _ in conn.cancel() })
            }
        }
        let ready = ListenerReady()
        listener.stateUpdateHandler = { if case .ready = $0 { ready.flag = true } }
        listener.start(queue: .global())
        defer { listener.cancel() }
        spin(until: { ready.flag }, limit: 3)
        guard let port = listener.port?.rawValue, let url = URL(string: "http://127.0.0.1:\(port)/big") else { expect(false, "port"); return }
        var caught: Error?
        _ = awaitMain(8) {
            do { _ = try await URLSessionSocialHTTP().send(SocialHTTPRequest(method: "GET", url: url), deadline: 6) }
            catch { caught = error }
            return true
        }
        expectEqual(caught as? SocialFailure, .invalid, "1 MiB + 1 fails closed")
    }

    /// A key that went through a full scripted feed session never reaches a bundle.
    @MainActor static func testYouTubeSentinelThroughFeed() {
        let key = "AIzaSENTINELKEY1234567890ab"
        let model = AppModel()
        let review = model.graphics.socialReview
        let http = ScriptedHTTP([(200, ytLookup), (200, #"{"pollingIntervalMillis":1000,"nextPageToken":"c","items":[]}"#)])
        var ranWith: String?
        review.feeds.youTube = { videoId, k, playout, report in
            ranWith = k
            await YouTubeSocialFeed.run(videoId: videoId, key: k, playout: playout, http: http,
                                        sleep: { _ in withUnsafeCurrentTask { $0?.cancel() } }, report: report)
        }
        run { await review.connectYouTube(videoId: "abcDEF123_4", key: key) }
        spin(until: { ranWith != nil && !review.running }, limit: 3)
        expectEqual(ranWith, key, "the feed ran with the key")
        model.pushWarning("social: " + review.status)
        var input = model.supportBundleInput()
        expect(input.knownSecretLiterals.contains(key), "the stored key is handed to the scrubber")
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("cvp-yt-sentinel-\(getpid())")
        try? FileManager.default.removeItem(at: dir)
        input.destinationRoot = dir
        input.shellLogPath = "/nonexistent"; input.coreLogPath = "/nonexistent"; input.crashReportDirectory = "/nonexistent"
        _ = try? SupportBundle.write(input)
        defer { try? FileManager.default.removeItem(at: dir); SocialCredentials.youTubeKey.delete() }
        for file in FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil)?.compactMap({ $0 as? URL }) ?? []
        where !file.hasDirectoryPath {
            let text = String(decoding: (try? Data(contentsOf: file)) ?? Data(), as: UTF8.self)
            expect(!text.contains(key), "\(file.lastPathComponent) must not contain the API key")
        }
        expect(http.requests.allSatisfy { !$0.url.absoluteString.contains(key) }, "no request URL carried the key")
    }
}
