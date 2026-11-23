// YouTube live chat for social review: ports of
// native-shell/CoreVideoPro.MediaCore/Services/YouTubeChatTransport.cs and
// YouTubeSocialFeed.cs. The API key travels ONLY in the x-goog-api-key header,
// never in a URL; redirects are refused (a 3xx is an authorization failure);
// polls are never faster than 15 s; a retryable failure backs off 30, 60,
// 120 s and every reconnect clears review decisions. Error text is fixed.

import Foundation

enum YouTubeChatTransport {
    static let origin = "https://www.googleapis.com/youtube/v3/"
    static let deadline: TimeInterval = 12
    static let pageFields = "pollingIntervalMillis,nextPageToken,offlineAt," +
        "items(id,snippet(type,liveChatId,authorChannelId,textMessageDetails,messageDeletedDetails,messageRetractedDetails,userBannedDetails),authorDetails(displayName))"

    /// YouTubeChatException messages, verbatim.
    static func message(_ failure: SocialFailure) -> String {
        switch failure {
        case .authorization: return "YouTube rejected the API key or the request."
        case .ended: return "This public livestream has no active chat."
        case .retry: return "YouTube chat is temporarily unavailable."
        case .invalid: return "YouTube returned an unexpected chat response."
        }
    }

    static func isVideoId(_ s: String) -> Bool { SocialFeedRunners.isVideoId(s) }
    static func isApiKey(_ s: String) -> Bool {
        (20...128).contains(s.utf16.count) && s.unicodeScalars.allSatisfy(isIdentifierScalar)
    }
    private static func isIdentifierScalar(_ c: Unicode.Scalar) -> Bool {
        ("a"..."z").contains(c) || ("A"..."Z").contains(c) || ("0"..."9").contains(c) || c == "-" || c == "_"
    }
    private static func isProviderValue(_ s: String, limit: Int = 256) -> Bool {
        (1...limit).contains(s.utf16.count) && s.utf16.allSatisfy { (0x21...0x7E).contains($0) }
    }

    /// Uri.EscapeDataString: everything but the RFC 3986 unreserved set.
    static func escape(_ s: String) -> String {
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~")
        return s.addingPercentEncoding(withAllowedCharacters: allowed) ?? s
    }

    static func resolve(videoId: String, key: String, http: SocialHTTP) async throws -> String {
        guard isVideoId(videoId) else { throw SocialFailure.invalid }
        let query = "videos?part=liveStreamingDetails,status&id=\(videoId)&fields=items(id,status/privacyStatus,liveStreamingDetails/activeLiveChatId)"
        let root = try await fetch(query, key: key, http: http)
        guard let items = root["items"] as? [Any] else { throw SocialFailure.invalid }
        if items.isEmpty { throw SocialFailure.ended }
        guard items.count == 1, let item = items[0] as? JSONObject, text(item, "id") == videoId else { throw SocialFailure.invalid }
        let chatId = text(item["liveStreamingDetails"] as? JSONObject, "activeLiveChatId")
        if text(item["status"] as? JSONObject, "privacyStatus") != "public" || chatId.isEmpty { throw SocialFailure.ended }
        guard isProviderValue(chatId) else { throw SocialFailure.invalid }
        return chatId
    }

    /// `waitMs` is max(pollingIntervalMillis, 15000): never faster than 15 s.
    struct Page: Equatable { var events: [SocialFeedEvent]; var next: String; var waitMs: Int
        var waitSeconds: Double { Double(waitMs) / 1000 } }

    static func page(chatId: String, cursor: String?, key: String, http: SocialHTTP) async throws -> Page {
        guard isProviderValue(chatId), cursor.map({ isProviderValue($0, limit: 2048) }) ?? true else { throw SocialFailure.invalid }
        var query = "liveChat/messages?part=id,snippet,authorDetails&maxResults=200&liveChatId=\(escape(chatId))&fields=" + escape(pageFields)
        if let cursor { query += "&pageToken=" + escape(cursor) }
        let page = try await fetch(query, key: key, http: http)
        if !text(page, "offlineAt").isEmpty { throw SocialFailure.ended }
        let next = text(page, "nextPageToken")
        guard isProviderValue(next, limit: 2048), let interval = page["pollingIntervalMillis"] as? NSNumber,
              CFGetTypeID(interval) != CFBooleanGetTypeID(), interval.doubleValue == interval.doubleValue.rounded(),
              interval.doubleValue > 0, interval.doubleValue <= Double(Int32.max),
              let rows = page["items"] as? [Any], rows.count <= 200 else { throw SocialFailure.invalid }
        var events: [SocialFeedEvent] = []
        for case let row as JSONObject in rows {
            let snippet = row["snippet"] as? JSONObject
            let kind = text(snippet, "type")
            guard text(snippet, "liveChatId") == chatId, !kind.isEmpty else { throw SocialFailure.invalid }
            if kind == "chatEndedEvent" { throw SocialFailure.ended }
            let event: SocialFeedEvent?
            switch kind {
            case "textMessageEvent":
                event = .text(id: text(row, "id"), author: text(row["authorDetails"] as? JSONObject, "displayName"),
                              message: text(snippet?["textMessageDetails"] as? JSONObject, "messageText"),
                              authorId: text(snippet, "authorChannelId"), order: 0)
            case "messageDeletedEvent":
                event = .remove(id: text(snippet?["messageDeletedDetails"] as? JSONObject, "deletedMessageId"))
            case "messageRetractedEvent":
                event = .remove(id: text(snippet?["messageRetractedDetails"] as? JSONObject, "retractedMessageId"))
            case "tombstone":
                event = .remove(id: text(row, "id"))
            case "userBannedEvent":
                event = .ban(authorId: text((snippet?["userBannedDetails"] as? JSONObject)?["bannedUserDetails"] as? JSONObject, "channelId"))
            default:
                event = nil
            }
            guard let event else { continue }
            switch event {
            case let .text(id, _, _, authorId, _):
                guard isProviderValue(id), isProviderValue(authorId) else { throw SocialFailure.invalid }
            case let .remove(id):
                guard isProviderValue(id) else { throw SocialFailure.invalid }
            case let .ban(authorId):
                guard isProviderValue(authorId) else { throw SocialFailure.invalid }
            default: break
            }
            events.append(event)
        }
        return Page(events: events, next: next, waitMs: max(Int(interval.doubleValue), 15000))
    }

    private static func fetch(_ query: String, key: String, http: SocialHTTP) async throws -> JSONObject {
        guard isApiKey(key), let url = URL(string: origin + query) else { throw SocialFailure.authorization }
        let response = try await http.send(SocialHTTPRequest(method: "GET", url: url, headers: ["x-goog-api-key": key]),
                                           deadline: deadline)
        if (300..<400).contains(response.status) { throw SocialFailure.authorization }
        guard (200..<300).contains(response.status) else { throw failure(status: response.status, body: response.body) }
        guard let root = (try? JSONSerialization.jsonObject(with: response.body)) as? JSONObject, depth(root) <= 20 else {
            throw SocialFailure.invalid
        }
        return root
    }

    static func failure(status: Int, body: Data) -> SocialFailure {
        if status >= 500 || status == 429 { return .retry }
        let errors = (((try? JSONSerialization.jsonObject(with: body)) as? JSONObject)?["error"] as? JSONObject)?["errors"] as? [Any]
        let reason = text(errors?.first as? JSONObject, "reason")
        switch reason {
        case "liveChatEnded", "liveChatDisabled", "liveChatNotFound": return .ended
        case "quotaExceeded", "rateLimitExceeded", "userRateLimitExceeded", "invalidPageToken": return .retry
        default: return .authorization
        }
    }

    private static func depth(_ value: Any, _ level: Int = 1) -> Int {
        if let o = value as? JSONObject { return o.values.map { depth($0, level + 1) }.max() ?? level }
        if let a = value as? [Any] { return a.map { depth($0, level + 1) }.max() ?? level }
        return level
    }

    private static func text(_ o: JSONObject?, _ key: String) -> String { o?[key] as? String ?? "" }
}

/// One explicitly connected public YouTube chat (YouTubeSocialFeed.RunAsync).
/// Approval state never crosses a lost connection.
enum YouTubeSocialFeed {
    @MainActor
    static func run(videoId: String, key: String, playout: SocialPlayout, http: SocialHTTP,
                    sleep: @escaping (UInt64) async -> Void, report: @escaping @MainActor (String) -> Void) async {
        var source: Int64 = 0
        await loop(videoId: videoId, key: key, playout: playout, http: http, sleep: sleep, report: report, source: &source)
        // C# finally: the session this feed opened never outlives it.
        if source != 0 { await playout.disconnect(source: source) }
    }

    @MainActor
    private static func loop(videoId: String, key: String, playout: SocialPlayout, http: SocialHTTP,
                             sleep: @escaping (UInt64) async -> Void, report: @escaping @MainActor (String) -> Void,
                             source: inout Int64) async {
        var retries = 0
        while !Task.isCancelled {
            source = await playout.connect(provider: "YouTube")
            report("Connecting to YouTube…")
            do {
                let chat = try await YouTubeChatTransport.resolve(videoId: videoId, key: key, http: http)
                var cursor: String?
                while true {
                    if Task.isCancelled { return }
                    let page = try await YouTubeChatTransport.page(chatId: chat, cursor: cursor, key: key, http: http)
                    if Task.isCancelled { return }
                    guard await playout.apply(source: source, page.events) else {
                        report("Review queue reset. Reconnect to start a new session.")
                        return
                    }
                    retries = 0
                    cursor = page.next
                    report("Connected to YouTube live chat.")
                    await sleep(UInt64(page.waitMs) * 1_000_000)
                }
            } catch SocialFailure.retry where retries < 3 {
                await playout.disconnect(source: source)
                let wait = 30 * (1 << retries)
                retries += 1
                report("YouTube chat disconnected. Review decisions cleared. Reconnecting…")
                await sleep(UInt64(wait) * 1_000_000_000)
            } catch let failure as SocialFailure {
                report(YouTubeChatTransport.message(failure))
                return
            } catch {
                report(YouTubeChatTransport.message(.invalid))
                return
            }
        }
    }
}
