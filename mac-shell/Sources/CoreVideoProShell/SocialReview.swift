// The social review queue: a port of
// native-shell/CoreVideoPro.MediaCore/Services/GraphicsSocialReview.cs. Session
// only, never persisted. Its owner (SocialPlayout) serialises feed callbacks,
// decisions and Show submission.
//
// The rules, all Windows':
//   - ids are 1-256 printable ASCII ('!'...'~');
//   - text over the limit in UTF-16 units, or carrying a control character,
//     is refused; otherwise trimmed and NFC-normalised, and the result must be
//     non-empty and at most 80 (author) / 192 (message) UTF-8 bytes. Refused,
//     never truncated;
//   - a full queue refuses new ids; nothing is evicted;
//   - an identical replay keeps its key and decision; a changed payload is a
//     new revision, pending, keeping the original receivedAt (expiry cannot be
//     extended by replaying);
//   - a tombstone blocks replays; room clear, per-author clear and a session
//     ban are order fences; overflowing the removal budget disconnects (fails
//     closed);
//   - disconnect() resets everything except the source counter, so an old
//     source's late callback can never land in a new session.
// "Approval alone sends no native command and leaves Program unchanged."

import Foundation

enum SocialDecision: Equatable { case pending, approved, rejected }

struct SocialItemKey: Hashable { let source: Int64; let id: String; let revision: Int64 }

struct SocialItem: Equatable {
    let key: SocialItemKey
    let author: String
    let message: String
    let receivedAt: Date
    let decision: SocialDecision
    let provider: String
    let authorId: String
    let order: Int64

    func with(decision: SocialDecision) -> SocialItem {
        SocialItem(key: key, author: author, message: message, receivedAt: receivedAt, decision: decision,
                   provider: provider, authorId: authorId, order: order)
    }
}

enum SocialFeedEvent: Equatable {
    case text(id: String, author: String, message: String, authorId: String, order: Int64)
    case remove(id: String)
    case ban(authorId: String)
    case clearRoom(order: Int64)
    case clearUser(authorId: String, order: Int64)
}

final class SocialReview {
    static let providers: Set<String> = ["YouTube", "Twitch"]
    static let authorLimit = 80, messageLimit = 192

    private let now: () -> Date
    private let capacity: Int
    private let removalCapacity: Int
    private let lifetime: TimeInterval
    private var store: [String: SocialItem] = [:]
    private var removed: Set<String> = []
    private var banned: Set<String> = []
    private var authorClears: [String: Int64] = [:]
    private var roomClear: Int64 = 0
    private var source: Int64 = 0
    private var revision: Int64 = 0
    private var provider = "YouTube"

    private(set) var isConnected = false

    init(now: @escaping () -> Date = Date.init, capacity: Int = 200, removalCapacity: Int = 4096,
         lifetime: TimeInterval = 15 * 60) {
        precondition(capacity > 0 && removalCapacity > 0 && lifetime > 0)
        self.now = now
        self.capacity = capacity
        self.removalCapacity = removalCapacity
        self.lifetime = lifetime
    }

    /// Expired first, newest revision first.
    var items: [SocialItem] {
        expire(now())
        return store.values.sorted { $0.key.revision > $1.key.revision }
    }

    /// "YouTube" | "Twitch" (Windows throws on anything else).
    func connect(provider: String) -> Int64 {
        precondition(Self.providers.contains(provider), "Unsupported social provider.")
        disconnect()
        self.provider = provider
        isConnected = true
        source += 1
        return source
    }

    func disconnect() {
        isConnected = false
        store.removeAll()
        removed.removeAll()
        banned.removeAll()
        authorClears.removeAll()
        roomClear = 0
        revision = 0
    }

    @discardableResult
    func receive(source: Int64, id: String, author: String, message: String, authorId: String = "",
                 order: Int64 = 0) -> SocialItem? {
        let t = now()
        expire(t)
        if !isCurrent(source) || !Self.validId(id) || removed.contains(id) || banned.contains(authorId)
            || (roomClear > 0 && order <= roomClear)
            || (authorClears[authorId].map { order <= $0 } ?? false) {
            return nil
        }
        let current = store[id]
        guard let a = Self.normalize(author, limit: Self.authorLimit),
              let m = Self.normalize(message, limit: Self.messageLimit) else {
            if current != nil { remove(id) }
            return nil
        }
        if let current, current.author == a, current.message == m, current.authorId == authorId { return current }
        if current == nil, store.count >= capacity { return nil }
        revision += 1
        let item = SocialItem(key: SocialItemKey(source: source, id: id, revision: revision), author: a, message: m,
                              receivedAt: current?.receivedAt ?? t, decision: .pending, provider: provider,
                              authorId: authorId, order: order)
        store[id] = item
        return item
    }

    func withdraw(source: Int64, id: String) {
        if isCurrent(source), Self.validId(id) { remove(id) }
    }

    @discardableResult
    func withdraw(_ key: SocialItemKey) -> Bool {
        guard find(key) != nil else { return false }
        remove(key.id)
        return true
    }

    /// One feed event. True when the source is the current one.
    @discardableResult
    func apply(source: Int64, _ event: SocialFeedEvent) -> Bool {
        guard isCurrent(source) else { return false }
        switch event {
        case let .text(id, author, message, authorId, order):
            receive(source: source, id: id, author: author, message: message, authorId: authorId, order: order)
        case let .remove(id): withdraw(source: source, id: id)
        case let .ban(authorId): ban(source: source, authorId: authorId)
        case let .clearUser(authorId, order): clearBefore(source: source, order: order, authorId: authorId)
        case let .clearRoom(order): clearBefore(source: source, order: order, authorId: nil)
        }
        return true
    }

    func approve(_ key: SocialItemKey) -> Bool { decide(key, .approved) }
    func reject(_ key: SocialItemKey) -> Bool { decide(key, .rejected) }

    func approved(_ key: SocialItemKey) -> SocialItem? {
        guard let item = find(key), item.decision == .approved else { return nil }
        return item
    }

    func isCurrent(_ source: Int64) -> Bool { isConnected && source == self.source }

    // ── internals ────────────────────────────────────────────────────────

    private func clearBefore(source: Int64, order: Int64, authorId: String?) {
        guard isCurrent(source) else { return }
        if order <= 0 || (authorId.map { !Self.validId($0) } ?? false) {
            disconnect()
            return
        }
        if let authorId { authorClears[authorId] = max(authorClears[authorId] ?? 0, order) }
        else { roomClear = max(roomClear, order) }
        if removed.count + banned.count + authorClears.count > removalCapacity {
            disconnect()
            return
        }
        for item in store.values where item.order <= order && (authorId == nil || item.authorId == authorId) {
            remove(item.key.id)
            if !isConnected { break }
        }
    }

    private func ban(source: Int64, authorId: String) {
        guard isCurrent(source), Self.validId(authorId) else { return }
        banned.insert(authorId)
        if banned.count + removed.count + authorClears.count > removalCapacity {
            disconnect()
            return
        }
        for item in store.values where item.authorId == authorId {
            remove(item.key.id)
            if !isConnected { break }
        }
    }

    private func decide(_ key: SocialItemKey, _ decision: SocialDecision) -> Bool {
        guard let item = find(key), item.decision == .pending else { return false }
        store[key.id] = item.with(decision: decision)
        return true
    }

    private func find(_ key: SocialItemKey) -> SocialItem? {
        expire(now())
        guard isCurrent(key.source), Self.validId(key.id), let item = store[key.id], item.key == key else { return nil }
        return item
    }

    private func expire(_ t: Date) {
        for item in store.values where t.timeIntervalSince(item.receivedAt) >= lifetime {
            remove(item.key.id)
            if !isConnected { break }
        }
    }

    private func remove(_ id: String) {
        store.removeValue(forKey: id)
        removed.insert(id)
        if removed.count + banned.count + authorClears.count > removalCapacity { disconnect() }
    }

    static func validId(_ id: String) -> Bool {
        let units = id.utf16
        return (1...256).contains(units.count) && units.allSatisfy { (0x21...0x7E).contains($0) }
    }

    /// C# Normalize: refuse over-long (UTF-16 units) or control-bearing text;
    /// trim; NFC; non-empty and within the UTF-8 byte limit.
    static func normalize(_ text: String, limit: Int) -> String? {
        guard text.utf16.count <= limit,
              !text.unicodeScalars.contains(where: { $0.properties.generalCategory == .control }) else { return nil }
        let normalized = text.trimmingCharacters(in: .whitespacesAndNewlines).precomposedStringWithCanonicalMapping
        return !normalized.isEmpty && normalized.utf8.count <= limit ? normalized : nil
    }
}
