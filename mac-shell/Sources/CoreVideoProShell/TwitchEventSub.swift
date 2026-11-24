// Twitch chat over EventSub WebSocket for social review: ports of
// native-shell/CoreVideoPro.MediaCore/Services/TwitchChatSocket.cs,
// TwitchEventSubSession.cs and TwitchSocialFeed.cs.
//   - wss://eventsub.wss.twitch.tv/ws only, 12 s connect, text messages of at
//     most 65536 bytes (binary or oversize fails closed, a close is a gap);
//   - review is enabled only after all four subscriptions are confirmed, in
//     this order, so removals exist before any message can arrive:
//     channel.chat.clear, channel.chat.clear_user_messages,
//     channel.chat.message_delete, channel.chat.message;
//   - deliveries de-duplicated by message_id over a window of 4096; ids are
//     "<broadcasterId>:<message_id>"; non-text and shared-chat messages from
//     another origin are dropped; clears are ordered by Twitch's timestamp to
//     the NANOSECOND (an integer parse, never Date);
//   - revocation stops with no retry; every connection gap, planned
//     migration included, disconnects review (approvals cleared, card
//     retired); reconnect budget 3 (1, 2, 4 s), each forcing a token refresh.

import Foundation

// ── the socket ──────────────────────────────────────────────────────────

protocol TwitchSocket: AnyObject {
    /// One whole text message. Throws TwitchFailure.invalid for binary or
    /// oversize, .retry on a close, an error or the deadline.
    func receive(deadline: TimeInterval) async throws -> String
    func close()
}

/// TwitchChatSocket over URLSessionWebSocketTask (which reassembles fragments
/// into whole messages). No client ping (Windows KeepAliveInterval=0): the
/// server's keepalive is the liveness signal.
final class URLSessionTwitchSocket: NSObject, TwitchSocket, URLSessionWebSocketDelegate, @unchecked Sendable {
    static let endpoint = URL(string: "wss://eventsub.wss.twitch.tv/ws")!
    static let messageLimit = 65536

    private var session: URLSession!
    private var task: URLSessionWebSocketTask!
    private let opened = SocialSignal()

    private init(url: URL) {
        super.init()
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieAcceptPolicy = .never
        config.httpShouldSetCookies = false
        config.urlCache = nil
        config.connectionProxyDictionary = [kCFNetworkProxiesHTTPEnable as String: false, "HTTPSEnable": false]
        session = URLSession(configuration: config, delegate: self, delegateQueue: nil)
        task = session.webSocketTask(with: url)
        task.maximumMessageSize = Self.messageLimit
    }

    /// Connect with a 12 s deadline. `url` is the canonical endpoint in
    /// production; tests point it at a local server.
    static func connect(url: URL = endpoint, deadline: TimeInterval = 12) async throws -> URLSessionTwitchSocket {
        let socket = URLSessionTwitchSocket(url: url)
        socket.task.resume()
        let ok = await socket.opened.wait(timeout: deadline)
        guard ok else { socket.close(); throw TwitchFailure.retry }
        return socket
    }

    func receive(deadline: TimeInterval) async throws -> String {
        let task = self.task!
        // The callback form plus a one-shot gate: whichever comes first wins —
        // the message, an error, or the deadline (which also closes the socket:
        // a missed keepalive is a connection gap). The async receive() wrapper
        // was observed never to resume after a cancel, so it is not used.
        final class Gate: @unchecked Sendable {
            let lock = NSLock(); var done = false
            func claim() -> Bool { lock.lock(); defer { lock.unlock() }; if done { return false }; done = true; return true }
        }
        let gate = Gate()
        let message: URLSessionWebSocketTask.Message = try await withCheckedThrowingContinuation { cont in
            let timer = Task.detached { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(max(0, deadline) * 1_000_000_000))
                guard gate.claim() else { return }
                cont.resume(throwing: TwitchFailure.retry)
                self?.close()
            }
            task.receive { result in
                timer.cancel()
                guard gate.claim() else { return }
                switch result {
                case .success(let m): cont.resume(returning: m)
                case .failure(let error):
                    // Over maximumMessageSize: an invalid message (fail closed), not a gap.
                    let ns = error as NSError
                    let tooBig = task.closeCode == .messageTooBig || (ns.domain == NSPOSIXErrorDomain && ns.code == Int(EMSGSIZE))
                    cont.resume(throwing: tooBig ? TwitchFailure.invalid : TwitchFailure.retry)
                }
            }
        }
        switch message {
        case .string(let text):
            guard text.utf8.count <= Self.messageLimit else { throw TwitchFailure.invalid }
            return text
        case .data:
            throw TwitchFailure.invalid
        @unknown default:
            throw TwitchFailure.invalid
        }
    }

    func close() {
        task?.cancel(with: .goingAway, reason: nil)
        session?.invalidateAndCancel()   // releases the delegate (self)
        opened.fire(false)
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?) {
        opened.fire(true)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        opened.fire(false)
    }

    // A redirect is never followed.
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

/// A one-shot boolean signal with a timeout.
final class SocialSignal: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Bool?
    private var waiters: [CheckedContinuation<Bool, Never>] = []

    func fire(_ result: Bool) {
        lock.lock()
        guard value == nil else { lock.unlock(); return }
        value = result
        let pending = waiters; waiters = []
        lock.unlock()
        pending.forEach { $0.resume(returning: result) }
    }

    func wait(timeout: TimeInterval) async -> Bool {
        let timer = Task { try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000)); fire(false) }
        defer { timer.cancel() }
        return await withCheckedContinuation { cont in
            lock.lock()
            if let value { lock.unlock(); cont.resume(returning: value); return }
            waiters.append(cont)
            lock.unlock()
        }
    }
}

// ── the session ─────────────────────────────────────────────────────────

/// Twitch's RFC 3339 timestamps as Unix nanoseconds (TwitchJson.Timestamp).
/// An integer parse of the fraction: a Date loses the nanosecond that orders
/// a clear against an adjacent message.
enum TwitchTimestamp {
    static func nanoseconds(_ value: String) -> Int64? {
        let chars = Array(value.utf8)
        guard (20...30).contains(chars.count), chars.last == UInt8(ascii: "Z") else { return nil }
        let head = String(decoding: chars[0..<19], as: UTF8.self)
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "UTC")
        f.dateFormat = "yyyy-MM-dd'T'HH:mm:ss"
        guard let seconds = f.date(from: head), f.string(from: seconds) == head else { return nil }
        let fraction = chars[19..<(chars.count - 1)]
        var nanos: Int64 = 0
        if !fraction.isEmpty {
            // "." plus 1-9 digits (C#: fraction length 2..10), padded right to nanoseconds.
            guard fraction.first == UInt8(ascii: "."), (2...10).contains(fraction.count) else { return nil }
            let digits = fraction.dropFirst()
            guard digits.allSatisfy({ (UInt8(ascii: "0")...UInt8(ascii: "9")).contains($0) }),
                  let n = Int64(String(decoding: digits, as: UTF8.self).padding(toLength: 9, withPad: "0", startingAt: 0))
            else { return nil }
            nanos = n
        }
        let (scaled, o1) = Int64(seconds.timeIntervalSince1970).multipliedReportingOverflow(by: 1_000_000_000)
        guard !o1 else { return nil }
        let (total, o2) = scaled.addingReportingOverflow(nanos)
        guard !o2, total > 0 else { return nil }
        return total
    }
}

struct TwitchEventSubSession {
    static let requiredTypes = ["channel.chat.clear", "channel.chat.clear_user_messages",
                                "channel.chat.message_delete", "channel.chat.message"]
    static let dedupeWindow = 4096

    enum ReadOutcome: Equatable { case keepalive, reconnect, revoked, events([SocialFeedEvent]) }

    let id: String
    let keepaliveSeconds: Int
    private let channel: String
    private let user: String
    private var subscriptions: [String: String] = [:]
    private var seen: Set<String> = []
    private var deliveries: [String] = []

    var isReady: Bool { subscriptions.count == Self.requiredTypes.count }

    private init(id: String, channel: String, user: String, keepalive: Int) {
        self.id = id; self.channel = channel; self.user = user; keepaliveSeconds = keepalive
    }

    static func welcome(_ json: String, channel: String, user: String) throws -> TwitchEventSubSession {
        let root = try TwitchJSON.parse(Data(json.utf8))
        let metadata = root["metadata"] as? JSONObject
        let session = (root["payload"] as? JSONObject)?["session"] as? JSONObject
        let id = TwitchJSON.text(session, "id")
        guard TwitchJSON.text(metadata, "message_type") == "session_welcome",
              TwitchJSON.identifier(TwitchJSON.text(metadata, "message_id")), TwitchJSON.identifier(id),
              TwitchJSON.userId(channel), TwitchJSON.userId(user), TwitchJSON.text(session, "status") == "connected",
              let timeout = session?["keepalive_timeout_seconds"] as? NSNumber, CFGetTypeID(timeout) != CFBooleanGetTypeID(),
              timeout.doubleValue == timeout.doubleValue.rounded(), (10...600).contains(timeout.intValue),
              TwitchTimestamp.nanoseconds(TwitchJSON.text(metadata, "message_timestamp")) != nil
        else { throw TwitchFailure.invalid }
        return TwitchEventSubSession(id: id, channel: channel, user: user, keepalive: timeout.intValue)
    }

    mutating func confirm(type: String, response: Data) throws {
        let root = try TwitchJSON.parse(response)
        guard Self.requiredTypes.contains(type), let rows = root["data"] as? [Any], rows.count == 1,
              let row = rows[0] as? JSONObject else { throw TwitchFailure.invalid }
        try validate(row, type: type, enabled: true)
        let subscription = TwitchJSON.text(row, "id")
        if subscriptions.contains(where: { $0.key != type && $0.value == subscription }) { throw TwitchFailure.invalid }
        subscriptions[type] = subscription
    }

    mutating func read(_ json: String) throws -> ReadOutcome {
        guard isReady else { throw TwitchFailure.invalid }
        let root = try TwitchJSON.parse(Data(json.utf8))
        let metadata = root["metadata"] as? JSONObject
        guard let payload = root["payload"] as? JSONObject else { throw TwitchFailure.invalid }
        let delivery = TwitchJSON.text(metadata, "message_id")
        guard TwitchJSON.identifier(delivery), let order = TwitchTimestamp.nanoseconds(TwitchJSON.text(metadata, "message_timestamp"))
        else { throw TwitchFailure.invalid }
        let kind = TwitchJSON.text(metadata, "message_type")
        if kind == "session_keepalive" { return .keepalive }
        if kind == "session_reconnect" {
            let session = payload["session"] as? JSONObject
            let target = TwitchJSON.text(session, "reconnect_url")
            guard TwitchJSON.text(session, "id") == id, TwitchJSON.text(session, "status") == "reconnecting",
                  target.utf16.count <= 4096, let c = URLComponents(string: target), c.scheme == "wss",
                  c.host?.lowercased() == "eventsub.wss.twitch.tv", c.port == nil || c.port == 443,
                  c.user == nil, c.password == nil, c.fragment == nil else { throw TwitchFailure.invalid }
            return .reconnect
        }
        guard kind == "notification" || kind == "revocation" else { throw TwitchFailure.invalid }
        let type = TwitchJSON.text(metadata, "subscription_type")
        let subscription = payload["subscription"] as? JSONObject
        try validate(subscription, type: type, enabled: kind == "notification")
        guard TwitchJSON.text(metadata, "subscription_version") == "1", let expected = subscriptions[type],
              TwitchJSON.text(subscription, "id") == expected else { throw TwitchFailure.invalid }
        if kind == "revocation" { return .revoked }
        let item = payload["event"] as? JSONObject
        guard TwitchJSON.text(item, "broadcaster_user_id") == channel else { throw TwitchFailure.invalid }
        let mapped = try map(type: type, item: item, order: order)
        if seen.contains(delivery) { return .events([]) }
        seen.insert(delivery)
        deliveries.append(delivery)
        if deliveries.count > Self.dedupeWindow { seen.remove(deliveries.removeFirst()) }
        return .events(mapped.map { [$0] } ?? [])
    }

    private func map(type: String, item: JSONObject?, order: Int64) throws -> SocialFeedEvent? {
        if type == "channel.chat.clear" { return .clearRoom(order: order) }
        let author = TwitchJSON.text(item, type == "channel.chat.message" ? "chatter_user_id" : "target_user_id")
        guard TwitchJSON.userId(author) else { throw TwitchFailure.invalid }
        if type == "channel.chat.clear_user_messages" { return .clearUser(authorId: author, order: order) }
        let raw = TwitchJSON.text(item, "message_id")
        guard TwitchJSON.identifier(raw), raw.utf16.count <= 128 else { throw TwitchFailure.invalid }
        let id = channel + ":" + raw
        if type == "channel.chat.message_delete" { return .remove(id: id) }
        // Shared-room origins and non-text products are outside the selected channel.
        let origin = TwitchJSON.text(item, "source_broadcaster_user_id")
        guard TwitchJSON.text(item, "message_type") == "text", origin.isEmpty || origin == channel else { return nil }
        return .text(id: id, author: TwitchJSON.text(item, "chatter_user_name"),
                     message: TwitchJSON.text(item?["message"] as? JSONObject, "text"), authorId: author, order: order)
    }

    private func validate(_ s: JSONObject?, type: String, enabled: Bool) throws {
        let condition = s?["condition"] as? JSONObject, transport = s?["transport"] as? JSONObject
        guard Self.requiredTypes.contains(type), TwitchJSON.text(s, "type") == type, TwitchJSON.text(s, "version") == "1",
              TwitchJSON.identifier(TwitchJSON.text(s, "id")), !enabled || TwitchJSON.text(s, "status") == "enabled",
              TwitchJSON.text(condition, "broadcaster_user_id") == channel, TwitchJSON.text(condition, "user_id") == user,
              TwitchJSON.text(transport, "method") == "websocket", TwitchJSON.text(transport, "session_id") == id
        else { throw TwitchFailure.invalid }
    }
}

extension TwitchHttpTransport {
    /// POST helix/eventsub/subscriptions for one required type, then confirm it.
    static func subscribe(_ session: inout TwitchEventSubSession, type: String, channel: String,
                          credentials c: TwitchCredentials, http: SocialHTTP) async throws {
        guard TwitchEventSubSession.requiredTypes.contains(type) else { throw TwitchFailure.invalid }
        let body: JSONObject = ["type": type, "version": "1",
                                "condition": ["broadcaster_user_id": channel, "user_id": c.userId],
                                "transport": ["method": "websocket", "session_id": session.id]]
        let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
        let root = try await request(helix("POST", "eventsub/subscriptions", c, body: data), http: http)
        let confirmed = (try? JSONSerialization.data(withJSONObject: root)) ?? Data()
        try session.confirm(type: type, response: confirmed)
    }
}

// ── the feed ────────────────────────────────────────────────────────────

/// One selected channel; every connection gap starts a new private review.
enum TwitchSocialFeed {
    @MainActor
    static func run(channel: String, clientId: String, auth: TwitchAuthorization, playout: SocialPlayout, http: SocialHTTP,
                    connect: @escaping () async throws -> TwitchSocket, sleep: @escaping (UInt64) async -> Void,
                    report: @escaping @MainActor (String) -> Void,
                    prompt: @escaping @MainActor (TwitchDevicePrompt) -> Void) async {
        var source: Int64 = 0
        do {
            await playout.disconnect()
            guard TwitchAuthorization.isChannel(channel) else { throw TwitchFailure.invalid }
            report("Authorizing Twitch chat…")
            var credentials = try await auth.authorize(clientId: clientId, prompt: prompt)
            var retry = 0
            while true {
                try Task.checkCancellation()
                do {
                    if retry > 0 { credentials = try await auth.maintain(force: true) }
                    report("Connecting to Twitch chat…")
                    let broadcaster = try await TwitchHttpTransport.channel(channel, credentials: credentials, http: http)
                    let socket = try await connect()
                    defer { socket.close() }
                    let welcome = try await socket.receive(deadline: 10)
                    var session = try TwitchEventSubSession.welcome(welcome, channel: broadcaster, user: credentials.userId)
                    // Removals first: review opens only when all four are confirmed.
                    for type in TwitchEventSubSession.requiredTypes {
                        try await TwitchHttpTransport.subscribe(&session, type: type, channel: broadcaster,
                                                                credentials: credentials, http: http)
                    }
                    source = await playout.connect(provider: "Twitch")
                    report("Connected to Twitch live chat: " + channel + ".")
                    let opened = source
                    do {
                        try await withThrowingTaskGroup(of: Void.self) { group in
                            group.addTask { @MainActor in try await read(socket: socket, session: session, playout: playout, source: opened) }
                            group.addTask { @MainActor in
                                while true {
                                    await sleep(30_000_000_000)
                                    try Task.checkCancellation()
                                    _ = try await auth.maintain()
                                }
                            }
                            defer { group.cancelAll(); socket.close() }
                            try await group.next()
                        }
                    } catch {
                        // Invalidate approval before anything else (C# finally).
                        await playout.disconnect(source: opened)
                        throw error
                    }
                    await playout.disconnect(source: opened)
                } catch TwitchFailure.retry where retry < 3 {
                    if source != 0 { await playout.disconnect(source: source) }
                    report("Twitch chat disconnected. Review decisions cleared. Reconnecting…")
                    await sleep(UInt64(1 << retry) * 1_000_000_000)
                    retry += 1
                }
            }
        } catch let failure as TwitchFailure {
            report(failure.message)
        } catch {
            // Cancelled: the owner disconnected.
        }
        if source != 0 { await playout.disconnect(source: source) }
    }

    @MainActor
    private static func read(socket: TwitchSocket, session initial: TwitchEventSubSession, playout: SocialPlayout,
                             source: Int64) async throws {
        var session = initial
        while true {
            let text = try await socket.receive(deadline: TimeInterval(session.keepaliveSeconds))
            try Task.checkCancellation()
            switch try session.read(text) {
            case .revoked: throw TwitchFailure.revoked
            // A planned move uses a fresh canonical connection too; no unproven continuity.
            case .reconnect: throw TwitchFailure.retry
            case .keepalive: continue
            case .events(let events):
                guard await playout.apply(source: source, events) else { throw TwitchFailure.invalid }
            }
        }
    }
}
