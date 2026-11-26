// Twitch EventSub, plan 2026-11-22 Task 17: ports of TwitchChatSocketTests,
// TwitchEventSubSessionTests and TwitchSocialFeedTests (C# literals), plus a
// real URLSessionWebSocketTask against a local WebSocket server.

import Foundation
import Network

extension ShellTests {
    static var socialTwitchEventSubCases: [(String, () -> Void)] {
        [
            ("social/twitch-session-requires-all-four-subscriptions-and-dedupes", testTwitchSessionReady),
            ("social/twitch-session-foreign-subscription-cannot-enable-review", testTwitchSessionForeign),
            ("social/twitch-session-nanosecond-ordering", testTwitchSessionNanoseconds),
            ("social/twitch-session-clear-fences-retire-only-pre-clear-items", testTwitchSessionClearFences),
            ("social/twitch-session-revocation-and-migration-explicit", testTwitchSessionRevocationMigration),
            ("social/twitch-session-shared-chat-and-non-text-dropped", testTwitchSessionSharedChat),
            ("social/twitch-socket-real-websocket", testTwitchRealSocket),
            ("social/twitch-feed-removal-subscriptions-ready-before-any-message", testTwitchFeedRemovalFirst),
            ("social/twitch-feed-failed-required-subscription-never-enables-review", testTwitchFeedRefusedSubscription),
            ("social/twitch-feed-revocation-clears-approved-without-retry", testTwitchFeedRevocation),
            ("social/twitch-feed-unexpected-disconnect-and-migration-clear-approvals", testTwitchFeedGaps),
            ("social/twitch-feed-gap-retires-on-air-card", testTwitchFeedGapRetiresCard),
            ("social/twitch-session-reconnect-url-every-check", testTwitchSessionReconnectURL),
            ("social/twitch-session-maps-before-dedupe", testTwitchSessionMapsBeforeDedupe),
            ("social/twitch-socket-utf8-and-exact-limit", testTwitchRealSocketEdges),
            ("social/twitch-socket-owner-cancel-is-prompt", testTwitchSocketCancel),
            ("social/twitch-feed-reconnect-budget-1-2-4-with-forced-refresh", testTwitchFeedReconnectBudget),
            ("social/twitch-feed-reconnects-into-a-fresh-review", testTwitchFeedReconnects),
            ("social/twitch-feed-disconnects-before-draining", testTwitchFeedDisconnectsBeforeDrain),
            ("social/twitch-feed-owner-cancel-during-welcome-no-reconnect", testTwitchFeedCancelDuringWelcome),
        ]
    }

    // ── TwitchEventSubSessionTests helpers (literals) ────────────────────

    static let twChannel = "1234", twUser = "5678", twSession = "fixture-session"
    static let twTypes = TwitchEventSubSession.requiredTypes

    static func jsonString(_ o: Any) -> String {
        String(data: (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys])) ?? Data(), encoding: .utf8) ?? ""
    }

    static func twWelcome(_ id: String = twSession, keepalive: Int = 10) -> String {
        jsonString(["metadata": ["message_id": "welcome", "message_type": "session_welcome",
                                 "message_timestamp": "2026-11-05T06:00:00.000000000Z"],
                    "payload": ["session": ["id": id, "status": "connected", "keepalive_timeout_seconds": keepalive]]])
    }

    static func twSubscription(_ type: String, channel: String = twChannel, user: String = twUser, session: String = twSession,
                               status: String = "enabled") -> JSONObject {
        ["id": "sub-" + type, "type": type, "version": "1", "status": status,
         "condition": ["broadcaster_user_id": channel, "user_id": user],
         "transport": ["method": "websocket", "session_id": session]]
    }

    static func twConfirmation(_ type: String, channel: String = twChannel, user: String = twUser, session: String = twSession) -> String {
        jsonString(["data": [twSubscription(type, channel: channel, user: user, session: session)]])
    }

    static func twNotification(_ delivery: String, _ type: String, _ entry: JSONObject,
                               stamp: String = "2026-11-05T06:00:01.000000001Z", channel: String = twChannel,
                               kind: String = "notification", status: String = "enabled") -> String {
        jsonString(["metadata": ["message_id": delivery, "message_type": kind, "message_timestamp": stamp,
                                 "subscription_type": type, "subscription_version": "1"],
                    "payload": ["subscription": twSubscription(type, channel: channel, status: status), "event": entry]])
    }

    static func twMessage(_ id: String = "message-a", author: String = "99", text: String = "Synthetic message") -> JSONObject {
        ["broadcaster_user_id": twChannel, "chatter_user_id": author, "chatter_user_name": "Synthetic viewer",
         "message_id": id, "message_type": "text", "message": ["text": text]]
    }

    static func twReady() -> TwitchEventSubSession {
        var s = try! TwitchEventSubSession.welcome(twWelcome(), channel: twChannel, user: twUser)
        for type in twTypes { try! s.confirm(type: type, response: Data(twConfirmation(type).utf8)) }
        return s
    }

    static func events(_ outcome: TwitchEventSubSession.ReadOutcome?) -> [SocialFeedEvent] {
        if case .events(let e)? = outcome { return e }
        return []
    }

    // ── session ports ───────────────────────────────────────────────────

    static func testTwitchSessionReady() {
        guard var s = try? TwitchEventSubSession.welcome(twWelcome(), channel: twChannel, user: twUser) else { expect(false, "welcome"); return }
        expectEqual(s.keepaliveSeconds, 10, "keepalive")
        for type in twTypes.prefix(3) { try? s.confirm(type: type, response: Data(twConfirmation(type).utf8)) }
        expect(!s.isReady, "three of four is not ready")
        let wire = twNotification("delivery-a", twTypes[3], twMessage())
        expect((try? s.read(wire)) == nil, "reading before ready fails")
        try? s.confirm(type: twTypes[3], response: Data(twConfirmation(twTypes[3]).utf8))
        expect(s.isReady, "ready")
        let first = events(try? s.read(wire))
        expectEqual(first, [.text(id: "1234:message-a", author: "Synthetic viewer", message: "Synthetic message", authorId: "99",
                                  order: TwitchTimestamp.nanoseconds("2026-11-05T06:00:01.000000001Z")!)], "mapped")
        expect(events(try? s.read(wire)).isEmpty, "a duplicate delivery is dropped")
    }

    static func testTwitchSessionForeign() {
        for (channel, user, socket) in [("wrong", twUser, twSession), (twChannel, "wrong", twSession), (twChannel, twUser, "wrong")] {
            guard var s = try? TwitchEventSubSession.welcome(twWelcome(), channel: twChannel, user: twUser) else { continue }
            expect((try? s.confirm(type: twTypes[0], response: Data(twConfirmation(twTypes[0], channel: channel, user: user, session: socket).utf8))) == nil,
                   "foreign subscription refused")
            expect(!s.isReady, "not ready")
        }
    }

    static func testTwitchSessionNanoseconds() {
        var s = twReady()
        let removed = events(try? s.read(twNotification("delete", twTypes[2], ["broadcaster_user_id": twChannel, "target_user_id": "99", "message_id": "message-a"])))
        expectEqual(removed, [.remove(id: "1234:message-a")], "delete")
        let user = events(try? s.read(twNotification("clear-user", twTypes[1], ["broadcaster_user_id": twChannel, "target_user_id": "99"],
                                                     stamp: "2026-11-05T06:00:01.000000002Z")))
        let room = events(try? s.read(twNotification("clear-room", twTypes[0], ["broadcaster_user_id": twChannel],
                                                     stamp: "2026-11-05T06:00:01.000000003Z")))
        guard case .clearUser(_, let uo)? = user.first, case .clearRoom(let ro)? = room.first else { expect(false, "clears"); return }
        expectEqual(ro - uo, 1, "room-clear order minus user-clear order == 1 ns")
        expect((try? s.read(twNotification("foreign", twTypes[3], twMessage(), channel: "other"))) == nil, "foreign channel refused")
        expectEqual(TwitchTimestamp.nanoseconds("2026-11-05T06:00:00Z").map { $0 % 1_000_000_000 }, 0, "no fraction")
        expectEqual(TwitchTimestamp.nanoseconds("2026-11-05T06:00:00.5Z").map { $0 % 1_000_000_000 }, 500_000_000, "padded fraction")
        expect(TwitchTimestamp.nanoseconds("2026-11-05T06:00:00.1234567890Z") == nil, "ten digits refused")
        expect(TwitchTimestamp.nanoseconds("2026-11-05 06:00:00Z") == nil, "wrong separator refused")
    }

    static func testTwitchSessionClearFences() {
        var s = twReady()
        let review = SocialReview(now: { socialNow })
        let source = review.connect(provider: "Twitch")
        func apply(_ wire: String) { for e in events(try? s.read(wire)) { review.apply(source: source, e) } }
        apply(twNotification("first", twTypes[3], twMessage("old")))
        guard let old = review.items.first else { expect(false, "old"); return }
        expect(review.approve(old.key), "approve")
        apply(twNotification("newer", twTypes[3], twMessage("new"), stamp: "2026-11-05T06:00:03Z"))
        apply(twNotification("clear", twTypes[1], ["broadcaster_user_id": twChannel, "target_user_id": "99"], stamp: "2026-11-05T06:00:02Z"))
        expect(review.approved(old.key) == nil, "pre-clear item retired")
        expectEqual(review.items.map(\.key.id), ["1234:new"], "post-clear item kept")
        apply(twNotification("late", twTypes[3], twMessage("late"), stamp: "2026-11-05T06:00:01.500000000Z"))
        expectEqual(review.items.count, 1, "a delayed pre-clear delivery is refused")
        apply(twNotification("room", twTypes[0], ["broadcaster_user_id": twChannel], stamp: "2026-11-05T06:00:04Z"))
        expect(review.items.isEmpty, "room clear")
        apply(twNotification("late-room", twTypes[3], twMessage("late-room", author: "100"), stamp: "2026-11-05T06:00:03.999999999Z"))
        expect(review.items.isEmpty, "1 ns before the room clear is fenced")
        apply(twNotification("after", twTypes[3], twMessage("after"), stamp: "2026-11-05T06:00:04.000000001Z"))
        expectEqual(review.items.map(\.decision), [.pending], "1 ns after is pending")
    }

    static func testTwitchSessionRevocationMigration() {
        var s = twReady()
        let revoke = jsonString(["metadata": ["message_id": "revoke", "message_type": "revocation", "message_timestamp": "2026-11-05T06:00:05Z",
                                              "subscription_type": twTypes[0], "subscription_version": "1"],
                                 "payload": ["subscription": twSubscription(twTypes[0], status: "authorization_revoked")]])
        expectEqual(try? s.read(revoke), .revoked, "revocation")
        let reconnect = jsonString(["metadata": ["message_id": "move", "message_type": "session_reconnect", "message_timestamp": "2026-11-05T06:00:05Z"],
                                    "payload": ["session": ["id": twSession, "status": "reconnecting",
                                                            "reconnect_url": "wss://eventsub.wss.twitch.tv/ws?fixture=next"]]])
        var r = twReady()
        expectEqual(try? r.read(reconnect), .reconnect, "planned migration")
        var bad = twReady()
        let evil = reconnect.replacingOccurrences(of: "eventsub.wss.twitch.tv", with: "evil.example")
        expect((try? bad.read(evil)) == nil, "a migration to another host fails closed")
        var e = twReady()
        expect((try? e.read("{}")) == nil, "empty envelope")
        expect((try? e.read(String(repeating: "x", count: 65537))) == nil, "65537 bytes refused")
        expect((try? TwitchEventSubSession.welcome(twWelcome(keepalive: 0), channel: twChannel, user: twUser)) == nil, "keepalive 0 refused")
        let ten = twWelcome(keepalive: 10)
        expect(ten.contains(#""keepalive_timeout_seconds":10"#), "fixture shape")
        expect((try? TwitchEventSubSession.welcome(ten, channel: twChannel, user: twUser)) != nil, "keepalive 10 accepted")
        let tenPointZero = ten.replacingOccurrences(of: #""keepalive_timeout_seconds":10"#, with: #""keepalive_timeout_seconds":10.0"#)
        expect((try? TwitchEventSubSession.welcome(tenPointZero, channel: twChannel, user: twUser)) == nil,
               "keepalive 10.0 refused (C# TryGetInt32)")
    }

    static func testTwitchSessionSharedChat() {
        var s = twReady()
        var shared = twMessage("shared"); shared["source_broadcaster_user_id"] = "4321"
        expect(events(try? s.read(twNotification("s1", twTypes[3], shared))).isEmpty, "shared chat from another origin dropped")
        var cheer = twMessage("cheer"); cheer["message_type"] = "power_ups_message_effect"
        expect(events(try? s.read(twNotification("s2", twTypes[3], cheer))).isEmpty, "non-text dropped")
        var own = twMessage("own"); own["source_broadcaster_user_id"] = twChannel
        expectEqual(events(try? s.read(twNotification("s3", twTypes[3], own))).count, 1, "own-origin shared chat kept")
    }

    // ── a real WebSocket ────────────────────────────────────────────────

    /// A local WebSocket server that answers each connection with `frames`.
    final class WSServer: @unchecked Sendable {
        let listener: NWListener
        var ready = false
        init?(frames: [(Data, NWProtocolWebSocket.Opcode)], thenClose: Bool) {
            let ws = NWProtocolWebSocket.Options()
            ws.autoReplyPing = true
            let params = NWParameters.tcp
            params.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)
            guard let l = try? NWListener(using: params, on: .any) else { return nil }
            listener = l
            l.stateUpdateHandler = { [weak self] in if case .ready = $0 { self?.ready = true } }
            l.newConnectionHandler = { conn in
                conn.start(queue: .global())
                var rest = frames
                func next() {
                    guard !rest.isEmpty else {
                        if thenClose {
                            let meta = NWProtocolWebSocket.Metadata(opcode: .close)
                            conn.send(content: nil, contentContext: NWConnection.ContentContext(identifier: "close", metadata: [meta]),
                                      isComplete: true, completion: .contentProcessed { _ in })
                        }
                        return
                    }
                    let (data, op) = rest.removeFirst()
                    let meta = NWProtocolWebSocket.Metadata(opcode: op)
                    conn.send(content: data, contentContext: NWConnection.ContentContext(identifier: "f", metadata: [meta]),
                              isComplete: true, completion: .contentProcessed { _ in next() })
                }
                next()
            }
            l.start(queue: .global())
        }
        var url: URL? { listener.port.flatMap { URL(string: "ws://127.0.0.1:\($0.rawValue)/ws") } }
    }

    @MainActor static func receiveOne(_ frames: [(Data, NWProtocolWebSocket.Opcode)], close: Bool = false, deadline: TimeInterval = 3)
        -> (String?, Error?) {
        guard let server = WSServer(frames: frames, thenClose: close) else { return (nil, nil) }
        defer { server.listener.cancel() }
        spin(until: { server.ready }, limit: 3)
        guard let url = server.url else { return (nil, nil) }
        var text: String?, failure: Error?
        _ = awaitMain(deadline + 5) {
            do {
                let socket = try await URLSessionTwitchSocket.connect(url: url, deadline: 5)
                defer { socket.close() }
                text = try await socket.receive(deadline: deadline)
            } catch { failure = error }
            return true
        }
        return (text, failure)
    }

    @MainActor static func testTwitchRealSocket() {
        let ok = receiveOne([(Data("{é}".utf8), .text)])
        expectEqual(ok.0, "{é}", "a whole UTF-8 text message")
        let binary = receiveOne([(Data([1, 2, 3]), .binary)])
        expectEqual(binary.1 as? TwitchFailure, .invalid, "binary fails closed")
        let oversize = receiveOne([(Data(String(repeating: "x", count: 65537).utf8), .text)])
        expectEqual(oversize.1 as? TwitchFailure, .invalid, "over 65536 bytes fails closed (not a reconnect)")
        let closed = receiveOne([], close: true)
        expectEqual(closed.1 as? TwitchFailure, .retry, "a close is a connection gap")
        let silent = receiveOne([], deadline: 0.5)
        expectEqual(silent.1 as? TwitchFailure, .retry, "a missed keepalive deadline is a gap")
    }

    // ── feed ports (TwitchSocialFeedTests) ──────────────────────────────

    final class ScriptedTwitchSocket: TwitchSocket, @unchecked Sendable {
        private var queue: [String] = []
        private var waiter: CheckedContinuation<String, Error>?
        private var lost = false
        var reads = 0
        var deadlines: [TimeInterval] = []
        var closed = false
        init(session: String = twSession) { queue.append(twWelcome(session)) }
        func post(_ s: String) {
            if let w = waiter { waiter = nil; w.resume(returning: s) } else { queue.append(s) }
        }
        func lose() {
            lost = true
            if let w = waiter { waiter = nil; w.resume(throwing: TwitchFailure.retry) }
        }
        func receive(deadline: TimeInterval) async throws -> String {
            reads += 1
            deadlines.append(deadline)
            if !queue.isEmpty { return queue.removeFirst() }
            if lost || closed { throw TwitchFailure.retry }
            return try await withCheckedThrowingContinuation { waiter = $0 }
        }
        func close() {
            closed = true
            if let w = waiter { waiter = nil; w.resume(throwing: TwitchFailure.retry) }
        }
    }

    @MainActor final class TwitchFeedFixture {
        let clock = TwitchClock()
        var saved: TwitchCredentials?
        var order: [String] = []
        var pauseText = false, refuseText = false
        var textGate: CheckedContinuation<Void, Never>?
        var subscribingText = false
        var connections = 0
        var reports: [String] = []
        var retried = false
        var retrySleeps: [UInt64] = []
        var parkRetries = true           // false: a reconnect backoff returns at once
        var releaseMaintain = false      // true: the next 30 s maintain wait returns once
        var validations = 0
        var validationsAtFirstConnect = -1
        var blockValidation = false
        var validationGate: CheckedContinuation<Void, Never>?
        var failConnect = false
        var connectOverride: (() async throws -> TwitchSocket)?
        var nextSockets: [ScriptedTwitchSocket] = []   // reconnects take these in turn
        let socket = ScriptedTwitchSocket()
        let playout: SocialPlayout
        var auth: TwitchAuthorization!
        var http: FnHTTP!
        var feed: Task<Void, Never>?

        init(invoke: SocialPlayout.Invoke? = nil, observe: (() -> GraphicsState?)? = nil) {
            playout = SocialPlayout(invoke: invoke ?? { _, _ in expect(false, "unapproved command"); return GraphicsOperationResult() },
                                    observe: observe ?? { nil }, sleep: { _ in }, now: { socialNow })
            saved = ShellTests.twTokens(clock)
            http = FnHTTP { [unowned self] request in
                if request.url.host == "id.twitch.tv" {
                    await MainActor.run { self.validations += 1 }
                    if await MainActor.run(body: { self.blockValidation }) {
                        await withCheckedContinuation { c in Task { @MainActor in self.validationGate = c } }
                    }
                    return (200, ShellTests.twValidation())
                }
                expectEqual(request.url.host, "api.twitch.tv", "helix host")
                expectEqual(request.headers["Client-Id"], ShellTests.twClient, "client id header")
                if request.method == "GET" { return (200, #"{"data":[{"id":"1234","login":"fixturechannel"}]}"#) }
                let body = (try? JSONSerialization.jsonObject(with: request.body ?? Data())) as? JSONObject ?? [:]
                let type = body["type"] as? String ?? ""
                let condition = body["condition"] as? JSONObject
                expectEqual(condition?["broadcaster_user_id"] as? String, "1234", "subscription channel")
                expectEqual(condition?["user_id"] as? String, "5678", "subscription user")
                await MainActor.run { self.order.append(type) }
                if type == "channel.chat.message" {
                    await MainActor.run { self.subscribingText = true }
                    if await MainActor.run(body: { self.pauseText }) {
                        await withCheckedContinuation { c in Task { @MainActor in self.textGate = c } }
                    }
                    if await MainActor.run(body: { self.refuseText }) { return (403, "{}") }
                }
                return (202, ShellTests.twConfirmation(type))
            }
            auth = TwitchAuthorization(http: http, read: { [unowned self] in self.saved },
                                       save: { [unowned self] in self.saved = $0; return true },
                                       clear: { [unowned self] in self.saved = nil }, now: { [clock] in clock.now }, sleep: clock.sleep)
        }

        func run() {
            feed = Task { @MainActor in
                await TwitchSocialFeed.run(channel: "fixturechannel", clientId: ShellTests.twClient, auth: auth, playout: playout, http: http,
                                           connect: { [unowned self] in try await self.connect() },
                                           sleep: { [unowned self] ns in
                                               let park: Bool = await MainActor.run {
                                                   if ns == 30_000_000_000 {
                                                       if self.releaseMaintain { self.releaseMaintain = false; return false }
                                                       return true
                                                   }
                                                   self.retried = true
                                                   self.retrySleeps.append(ns)
                                                   return self.parkRetries
                                               }
                                               // park: unless released, a retry or maintain wait never elapses
                                               while park && !Task.isCancelled {
                                                   try? await Task.sleep(nanoseconds: 20_000_000)
                                                   if ns == 30_000_000_000, await MainActor.run(body: {
                                                       if self.releaseMaintain { self.releaseMaintain = false; return true }
                                                       return false
                                                   }) { return }
                                               }
                                           },
                                           report: { [unowned self] in self.reports.append($0) },
                                           prompt: { _ in expect(false, "unexpected device prompt") })
            }
        }

        func connect() async throws -> TwitchSocket {
            connections += 1
            if validationsAtFirstConnect < 0 { validationsAtFirstConnect = validations }
            if failConnect { throw TwitchFailure.retry }
            if let connectOverride { return try await connectOverride() }
            if connections > 1, !nextSockets.isEmpty { return nextSockets.removeFirst() }
            return socket
        }

        func stop() { feed?.cancel(); _ = ShellTests.awaitMain(5) { await self.feed?.value; return true } }
    }

    @MainActor static func testTwitchFeedRemovalFirst() {
        let f = TwitchFeedFixture()
        f.pauseText = true
        f.run()
        spin(until: { f.subscribingText && f.textGate != nil }, limit: 5)
        expect(!f.playout.isConnected, "review is closed while the message subscription is pending")
        f.textGate?.resume()
        spin(until: { f.socket.reads >= 2 }, limit: 5)
        f.socket.post(twNotification("a", "channel.chat.message", twMessage("a")))
        f.socket.post(twNotification("b", "channel.chat.message", twMessage("b")))
        spin(until: { f.socket.reads >= 4 }, limit: 5)
        expectEqual(f.order, twTypes, "subscriptions in the required order")
        expectEqual(f.playout.items.count, 2, "two messages")
        expect(f.playout.items.allSatisfy { $0.decision == .pending }, "pending")
        f.stop()
        expect(f.playout.items.isEmpty, "stopping clears review")
        expect(f.socket.closed, "the socket is closed")
    }

    @MainActor static func testTwitchFeedRefusedSubscription() {
        let f = TwitchFeedFixture()
        f.refuseText = true
        f.run()
        _ = awaitMain(5) { await f.feed?.value; return true }
        expect(!f.playout.isConnected, "never enabled")
        expect(f.playout.items.isEmpty, "empty")
        expect(f.socket.closed, "socket closed")
    }

    @MainActor static func testTwitchFeedRevocation() {
        let f = TwitchFeedFixture()
        f.run()
        spin(until: { f.socket.reads >= 2 }, limit: 5)
        expectEqual(Array(f.socket.deadlines.prefix(2)), [10, 10], "welcome 10 s, then the keepalive")
        f.socket.post(twNotification("a", "channel.chat.message", twMessage("a")))
        spin(until: { f.socket.reads >= 3 }, limit: 5)
        guard let item = f.playout.items.first else { expect(false, "item"); f.stop(); return }
        expect(f.playout.approve(item.key), "approve")
        f.socket.post(twNotification("revoked", "channel.chat.message", twMessage("a"), kind: "revocation", status: "authorization_revoked"))
        _ = awaitMain(5) { await f.feed?.value; return true }
        expect(!f.playout.isConnected && f.playout.items.isEmpty, "revocation clears review")
        expect(!f.playout.approve(item.key), "approval gone")
        expectEqual(f.connections, 1, "no retry")
        expectEqual(f.reports.last, "Twitch removed chat access. Review decisions cleared.", "fixed text")
    }

    @MainActor static func testTwitchFeedGaps() {
        for planned in [false, true] {
            let f = TwitchFeedFixture()
            f.run()
            spin(until: { f.socket.reads >= 2 }, limit: 5)
            f.socket.post(twNotification("a", "channel.chat.message", twMessage("a")))
            spin(until: { f.socket.reads >= 3 }, limit: 5)
            guard let item = f.playout.items.first else { expect(false, "item"); f.stop(); continue }
            expect(f.playout.approve(item.key), "approve")
            if planned {
                f.socket.post(jsonString(["metadata": ["message_id": "move", "message_type": "session_reconnect", "message_timestamp": "2026-11-05T06:00:05Z"],
                                          "payload": ["session": ["id": twSession, "status": "reconnecting",
                                                                  "reconnect_url": "wss://eventsub.wss.twitch.tv/ws?fixture=next"]]]))
            } else {
                f.socket.lose()
            }
            spin(until: { f.retried }, limit: 5)
            expect(f.retried, "planned=\(planned): retry scheduled")
            expect(!f.playout.isConnected && f.playout.items.isEmpty, "planned=\(planned): approvals cleared")
            expect(!f.playout.approve(item.key), "planned=\(planned): old approval unusable")
            expect(f.reports.contains("Twitch chat disconnected. Review decisions cleared. Reconnecting…"), "fixed text")
            f.stop()
        }
    }

    /// An approved card on air through a scripted core; the socket drops; the
    /// playout clears and removes the card and no approved item survives.
    @MainActor static func testTwitchFeedGapRetiresCard() {
        let core = SocialFixture(provider: "Twitch")
        core.ready = true; core.autoPrepare = true
        let corePlayout = core.make()
        let f = TwitchFeedFixture()
        // Drive the same feed into the core-backed playout.
        let feed = Task { @MainActor in
            await TwitchSocialFeed.run(channel: "fixturechannel", clientId: ShellTests.twClient, auth: f.auth, playout: corePlayout, http: f.http,
                                       connect: { f.socket }, sleep: { _ in while !Task.isCancelled { try? await Task.sleep(nanoseconds: 20_000_000) } },
                                       report: { _ in }, prompt: { _ in })
        }
        spin(until: { f.socket.reads >= 2 }, limit: 5)
        f.socket.post(twNotification("a", "channel.chat.message", twMessage("a")))
        spin(until: { f.socket.reads >= 3 }, limit: 5)
        guard let item = corePlayout.items.first else { expect(false, "item"); feed.cancel(); return }
        expect(corePlayout.approve(item.key), "approve")
        expectEqual(awaitMain { await corePlayout.show(item.key) }?.accepted, true, "shown")
        expect(corePlayout.onProgram, "on air")
        f.socket.lose()
        spin(until: { !corePlayout.isConnected && !corePlayout.onProgram }, limit: 5)
        let actions = core.commands.map { "\($0.action) \($0.body["action"] as? String ?? "")" }
        expect(actions.contains("graphics.instance.trigger clear") && actions.contains("graphics.instance.remove "), "clear + remove sent")
        expect(!corePlayout.items.contains { $0.decision == .approved }, "no approved item survives the gap")
        expect(core.state.instances.isEmpty, "the card is gone")
        feed.cancel()
        _ = awaitMain(5) { await feed.value; return true }
    }

    // ── review follow-ups (Task 17) ─────────────────────────────────────

    static func twReconnect(_ url: String, id: String = twSession, status: String = "reconnecting") -> String {
        jsonString(["metadata": ["message_id": "move", "message_type": "session_reconnect", "message_timestamp": "2026-11-05T06:00:05Z"],
                    "payload": ["session": ["id": id, "status": status, "reconnect_url": url]]])
    }

    /// Every reconnect_url check (TwitchEventSubSession.cs): only wss to
    /// eventsub.wss.twitch.tv on 443, no userinfo or fragment, at most 4096,
    /// for THIS session in status reconnecting.
    static func testTwitchSessionReconnectURL() {
        let good = "wss://eventsub.wss.twitch.tv/ws?fixture=next"
        for ok in [good, "wss://EventSub.WSS.Twitch.TV:443/ws"] {
            var s = twReady()
            expectEqual(try? s.read(twReconnect(ok)), .reconnect, "accepted: \(ok)")
        }
        let long = "wss://eventsub.wss.twitch.tv/ws?" + String(repeating: "a", count: 4096)
        let bad: [(String, String)] = [
            (twReconnect("ws://eventsub.wss.twitch.tv/ws"), "ws scheme"),
            (twReconnect("wss://eventsub.wss.twitch.tv:8443/ws"), "another port"),
            (twReconnect("wss://user:pw@eventsub.wss.twitch.tv/ws"), "userinfo"),
            (twReconnect("wss://user@eventsub.wss.twitch.tv/ws"), "user only"),
            (twReconnect(good + "#frag"), "fragment"),
            (twReconnect(long), "over 4096"),
            (twReconnect(good, id: "other-session"), "another session"),
            (twReconnect(good, status: "connected"), "status not reconnecting"),
        ]
        for (json, what) in bad {
            var s = twReady()
            expect((try? s.read(json)) == nil, "refused: \(what)")
        }
    }

    /// Map before dedupe: a repeated delivery id carrying an invalid payload
    /// still fails closed; it is not swallowed as a duplicate.
    static func testTwitchSessionMapsBeforeDedupe() {
        var s = twReady()
        expectEqual(events(try? s.read(twNotification("d1", twTypes[3], twMessage("m1")))).count, 1, "first delivery")
        expectEqual(events(try? s.read(twNotification("d1", twTypes[3], twMessage("m1")))).count, 0, "valid duplicate dropped")
        expect((try? s.read(twNotification("d1", twTypes[3], twMessage("m1", author: "not-a-user")))) == nil,
               "an invalid payload under a seen delivery id still throws")
    }

    @MainActor static func testTwitchRealSocketEdges() {
        let exact = receiveOne([(Data(String(repeating: "x", count: 65536).utf8), .text)])
        expectEqual(exact.0?.utf8.count, 65536, "exactly 65536 bytes is accepted")
        let utf8 = receiveOne([(Data([0x7B, 0xC3, 0x28, 0x7D]), .text)])
        expect(utf8.0 == nil, "invalid UTF-8 never reaches the session")
        expectEqual(utf8.1 as? TwitchFailure, .invalid, "invalid UTF-8 fails closed like C# (EPROTO), not a reconnect")
    }

    /// A TCP listener that accepts and never answers: the WebSocket handshake hangs.
    final class SilentTCP: @unchecked Sendable {
        let listener: NWListener
        var ready = false
        var held: [NWConnection] = []
        init?() {
            guard let l = try? NWListener(using: .tcp, on: .any) else { return nil }
            listener = l
            l.stateUpdateHandler = { [weak self] in if case .ready = $0 { self?.ready = true } }
            l.newConnectionHandler = { [weak self] c in c.start(queue: .global()); self?.held.append(c) }
            l.start(queue: .global())
        }
        var url: URL? { listener.port.flatMap { URL(string: "ws://127.0.0.1:\($0.rawValue)/ws") } }
    }

    /// The owner's disconnect cancels a hung connect or a pending receive at
    /// once, not after the 12 s connect or 10 s welcome deadline.
    @MainActor static func testTwitchSocketCancel() {
        if let tcp = SilentTCP() {
            defer { tcp.listener.cancel() }
            spin(until: { tcp.ready }, limit: 3)
            if let url = tcp.url {
                let start = Date()
                var failure: Error?
                let task = Task { @MainActor in
                    do { _ = try await URLSessionTwitchSocket.connect(url: url, deadline: 12) } catch { failure = error }
                }
                spin(until: { false }, limit: 0.3)
                task.cancel()
                _ = awaitMain(5) { await task.value; return true }
                expect(Date().timeIntervalSince(start) < 3, "a cancelled connect returns at once")
                expectEqual(failure as? TwitchFailure, .retry, "as a gap the feed ignores once cancelled")
            } else { expect(false, "silent listener port") }
        } else { expect(false, "silent listener") }
        guard let server = WSServer(frames: [], thenClose: false) else { expect(false, "server"); return }
        defer { server.listener.cancel() }
        spin(until: { server.ready }, limit: 3)
        guard let url = server.url else { expect(false, "server port"); return }
        var opened = false, failure: Error?
        let start = Date()
        let task = Task { @MainActor in
            do {
                let socket = try await URLSessionTwitchSocket.connect(url: url, deadline: 5)
                opened = true
                defer { socket.close() }
                _ = try await socket.receive(deadline: 10)
            } catch { failure = error }
        }
        spin(until: { opened }, limit: 5)
        expect(opened, "precondition: connected")
        spin(until: { false }, limit: 0.3)
        task.cancel()
        _ = awaitMain(5) { await task.value; return true }
        expect(Date().timeIntervalSince(start) < 6, "a cancelled receive returns at once, not at the 10 s deadline")
        expectEqual(failure as? TwitchFailure, .retry, "a cancelled receive is a gap")
    }

    /// Global constraint: reconnect budget 3 at 1, 2, 4 s, each reconnect
    /// forcing a token check first; the fourth failure stops with the fixed text.
    @MainActor static func testTwitchFeedReconnectBudget() {
        let f = TwitchFeedFixture()
        f.failConnect = true
        f.parkRetries = false
        f.run()
        _ = awaitMain(5) { await f.feed?.value; return true }
        expectEqual(f.retrySleeps, [1_000_000_000, 2_000_000_000, 4_000_000_000], "backoff 1, 2, 4 s")
        expectEqual(f.connections, 4, "the first try and three reconnects, no more")
        expectEqual(f.validations - f.validationsAtFirstConnect, 3, "every reconnect forces a token check")
        expectEqual(f.reports.filter { $0 == "Twitch chat disconnected. Review decisions cleared. Reconnecting…" }.count, 3,
                    "one reconnect notice per retry")
        expectEqual(f.reports.last, "Twitch chat disconnected. Review decisions cleared.", "the budget spent: fixed text")
        expect(!f.playout.isConnected, "review stays closed")
    }

    @MainActor static func testTwitchFeedReconnects() {
        let f = TwitchFeedFixture()
        f.parkRetries = false
        let second = ScriptedTwitchSocket()
        f.nextSockets = [second]
        f.run()
        spin(until: { f.socket.reads >= 2 }, limit: 5)
        f.socket.post(twNotification("a", "channel.chat.message", twMessage("a")))
        spin(until: { f.socket.reads >= 3 }, limit: 5)
        guard let item = f.playout.items.first else { expect(false, "item"); f.stop(); return }
        expect(f.playout.approve(item.key), "approve")
        f.socket.lose()
        spin(until: { second.reads >= 2 && f.playout.isConnected }, limit: 5)
        expectEqual(f.connections, 2, "a second connection")
        expectEqual(f.retrySleeps, [1_000_000_000], "after the first backoff")
        expect(f.playout.isConnected, "review reopens on the new session")
        expect(f.playout.items.isEmpty && !f.playout.approve(item.key), "a fresh review: the old approval is gone")
        second.post(twNotification("b", "channel.chat.message", twMessage("b")))
        spin(until: { second.reads >= 3 }, limit: 5)
        expectEqual(f.playout.items.count, 1, "the new session feeds review")
        f.stop()
    }

    /// C# finally order: approval is invalidated before waiting on a child
    /// stuck in a token call (the hourly validation here).
    @MainActor static func testTwitchFeedDisconnectsBeforeDrain() {
        let f = TwitchFeedFixture()
        f.run()
        spin(until: { f.socket.reads >= 2 }, limit: 5)
        f.socket.post(twNotification("a", "channel.chat.message", twMessage("a")))
        spin(until: { f.socket.reads >= 3 }, limit: 5)
        guard let item = f.playout.items.first else { expect(false, "item"); f.stop(); return }
        expect(f.playout.approve(item.key), "approve")
        f.blockValidation = true
        f.clock.now += 3601                  // the hourly validation is due
        f.releaseMaintain = true             // the next 30 s maintain wait elapses
        spin(until: { f.validationGate != nil }, limit: 5)
        expect(f.validationGate != nil, "precondition: maintain is inside a token call")
        f.socket.lose()
        spin(until: { !f.playout.isConnected }, limit: 2)
        expect(!f.playout.isConnected && f.playout.items.isEmpty, "review disconnects while the token call is still out")
        expect(!f.playout.approve(item.key), "the approval is unusable at once")
        f.blockValidation = false
        f.validationGate?.resume(); f.validationGate = nil
        f.stop()
    }

    /// The owner disconnects while the feed waits for the welcome: it ends at
    /// once and never reports a reconnect.
    @MainActor static func testTwitchFeedCancelDuringWelcome() {
        guard let server = WSServer(frames: [], thenClose: false) else { expect(false, "server"); return }
        defer { server.listener.cancel() }
        spin(until: { server.ready }, limit: 3)
        guard let url = server.url else { expect(false, "server port"); return }
        let f = TwitchFeedFixture()
        var opened = false
        f.connectOverride = {
            let socket = try await URLSessionTwitchSocket.connect(url: url, deadline: 5)
            await MainActor.run { opened = true }
            return socket
        }
        f.run()
        spin(until: { opened }, limit: 5)
        expect(opened, "precondition: connected, waiting for the welcome")
        spin(until: { false }, limit: 0.3)
        let start = Date()
        f.stop()
        expect(Date().timeIntervalSince(start) < 3, "the feed ends at once, not at the 10 s welcome deadline")
        expect(!f.reports.contains { $0.hasSuffix("Reconnecting…") }, "a cancel is not a reconnect")
        expectEqual(f.connections, 1, "and nothing reconnects")
    }
}
