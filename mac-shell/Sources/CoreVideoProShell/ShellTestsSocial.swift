// Social review, plan 2026-11-22 Task 12: the queue, ported from
// native-shell/CoreVideoPro.MediaCore.Tests/GraphicsSocialReviewTests.cs with
// the same literals and fixed clock (2026-11-04T18:00:00Z).

import Foundation

extension ShellTests {
    static var socialCases: [(String, () -> Void)] {
        [
            ("social/review-approval-applies-only-to-exact-revision", testSocialApprovalExactRevision),
            ("social/review-removal-before-and-after-approval-fences-replays", testSocialRemovalFencesReplays),
            ("social/review-disconnect-and-replacement-fence-old-callbacks", testSocialDisconnectFences),
            ("social/review-expiry-not-extended-by-replay-and-overflow-fails-closed", testSocialExpiryAndOverflow),
            ("social/review-same-bounded-plain-text", testSocialBoundedPlainText),
            ("social/review-twitch-clear-fences-by-order", testSocialTwitchClearFences),
            ("social/review-ban-and-user-clear", testSocialBanAndUserClear),
            ("social/review-edit-keeps-original-received-at", testSocialEditKeepsReceivedAt),
            ("social/review-byte-and-unit-limits-refuse-never-truncate", testSocialByteAndUnitLimits),
        ]
    }

    static let socialNow = ISO8601DateFormatter().date(from: "2026-11-04T18:00:00Z")!

    static func testSocialApprovalExactRevision() {
        let q = SocialReview(now: { socialNow })
        let source = q.connect(provider: "YouTube")
        let first = q.receive(source: source, id: "first", author: "Author", message: "First message")!
        let other = q.receive(source: source, id: "other", author: "Author", message: "Other message")!
        expect(q.approved(first.key) == nil, "not approved yet")
        expect(q.approve(first.key), "approve")
        expectEqual(q.approved(first.key)?.message, "First message", "approved text")
        expect(q.approved(other.key) == nil, "only the exact item")
        expectEqual(q.receive(source: source, id: "first", author: "Author", message: "First message")?.key, first.key,
                    "identical replay keeps the key")
        expect(q.approved(first.key) != nil, "and the approval")
        let edited = q.receive(source: source, id: "first", author: "Author", message: "Edited message")!
        expect(edited.key != first.key, "an edit is a new revision")
        expect(!q.approve(first.key), "old revision cannot be approved")
        expect(!q.reject(first.key), "or rejected")
        expect(q.approved(first.key) == nil, "old approval gone")
        expect(q.approved(edited.key) == nil, "edit starts pending")
        expect(q.reject(edited.key), "reject the edit")
        expect(!q.approve(edited.key), "a decision is final")
        expectEqual(q.receive(source: source, id: "first", author: "Author", message: "Edited message")?.decision, .rejected,
                    "replay keeps the decision")
    }

    static func testSocialRemovalFencesReplays() {
        let q = SocialReview(now: { socialNow })
        let source = q.connect(provider: "YouTube")
        q.withdraw(source: source, id: "early")
        expect(q.receive(source: source, id: "early", author: "Author", message: "Arrived after deletion") == nil, "tombstone")
        let item = q.receive(source: source, id: "shown", author: "Author", message: "Message")!
        expect(q.approve(item.key), "approve")
        q.withdraw(source: source, id: "shown")
        expect(q.approved(item.key) == nil, "withdrawn")
        expect(q.receive(source: source, id: "shown", author: "Author", message: "Message") == nil, "replay fenced")
        expect(q.items.isEmpty, "empty")
    }

    static func testSocialDisconnectFences() {
        let q = SocialReview(now: { socialNow })
        let firstSource = q.connect(provider: "YouTube")
        let first = q.receive(source: firstSource, id: "id", author: "Author", message: "Message")!
        expect(q.approve(first.key), "approve")
        q.disconnect()
        expect(q.approved(first.key) == nil, "disconnect clears")
        expect(q.receive(source: firstSource, id: "old", author: "Author", message: "Late callback") == nil, "late callback")
        let secondSource = q.connect(provider: "YouTube")
        let second = q.receive(source: secondSource, id: "id", author: "Author", message: "Message")!
        q.withdraw(source: firstSource, id: "id")
        expect(!q.approve(first.key), "old key")
        expect(q.approved(second.key) == nil, "new session starts pending")
        expect(q.approve(second.key), "approve new")
    }

    static func testSocialExpiryAndOverflow() {
        var time = socialNow
        let q = SocialReview(now: { time }, capacity: 2, removalCapacity: 1, lifetime: 10)
        let source = q.connect(provider: "YouTube")
        let first = q.receive(source: source, id: "first", author: "Author", message: "Message")!
        expect(q.approve(first.key), "approve first")
        time += 9
        expectEqual(q.receive(source: source, id: "first", author: "Author", message: "Message")?.key, first.key, "replay at 9 s")
        let second = q.receive(source: source, id: "second", author: "Author", message: "Message")!
        expect(q.approve(second.key), "approve second")
        expect(q.receive(source: source, id: "third", author: "Author", message: "Queue full") == nil, "full queue refuses")
        time += 1
        expect(q.approved(first.key) == nil, "expired at 10 s despite the replay")
        expect(q.receive(source: source, id: "first", author: "Author", message: "Replay after expiry") == nil, "tombstoned")
        q.withdraw(source: source, id: "another")
        expect(!q.isConnected, "removal budget exceeded: fails closed")
        expect(q.approved(second.key) == nil, "approvals gone")
        expect(q.receive(source: source, id: "new", author: "Author", message: "Late callback") == nil, "late callback")
    }

    static func testSocialBoundedPlainText() {
        let q = SocialReview(now: { socialNow })
        let source = q.connect(provider: "YouTube")
        expect(q.receive(source: source, id: "bad", author: "Author", message: "Bad\u{0}text") == nil, "control char")
        expect(q.receive(source: source, id: "long", author: "Author", message: String(repeating: "x", count: 193)) == nil,
               "193 bytes refused, never truncated")
        let item = q.receive(source: source, id: "valid", author: " Author ", message: " Cafe\u{301} ")!
        expectEqual(item.author, "Author", "trimmed")
        expectEqual(item.message, "Café", "NFC")
        expect(q.approve(item.key), "approve")
        expectEqual(q.approved(item.key)?.message, item.message, "same text")
        expect(q.receive(source: source, id: "bad id", author: "A", message: "m") == nil, "id with a space")
        expect(q.receive(source: source, id: String(repeating: "a", count: 257), author: "A", message: "m") == nil, "id too long")
    }

    /// The fences GraphicsSocialPlayoutTests.TwitchClear… drives through the model.
    static func testSocialTwitchClearFences() {
        let q = SocialReview(now: { socialNow })
        let source = q.connect(provider: "Twitch")
        let early = q.receive(source: source, id: "1234:early", author: "Viewer", message: "hello", authorId: "u1", order: 100)!
        expectEqual(early.provider, "Twitch", "provider stamped")
        expect(q.apply(source: source, .clearRoom(order: 200)), "clear accepted")
        expect(q.items.isEmpty, "the room clear retires items at or before it")
        expect(q.receive(source: source, id: "1234:late", author: "Viewer", message: "late", authorId: "u1", order: 199) == nil,
               "order 199 is fenced")
        expectEqual(q.receive(source: source, id: "1234:new", author: "Viewer", message: "new", authorId: "u1", order: 201)?.decision,
                    .pending, "order 201 is pending")
        expect(!q.apply(source: source + 1, .clearRoom(order: 999)), "a foreign source is not current")
        expect(q.apply(source: source, .clearRoom(order: 0)), "a zero-order clear is current...")
        expect(!q.isConnected, "...and fails closed")
    }

    static func testSocialBanAndUserClear() {
        let q = SocialReview(now: { socialNow })
        let source = q.connect(provider: "Twitch")
        q.receive(source: source, id: "a", author: "A", message: "one", authorId: "u1", order: 10)
        q.receive(source: source, id: "b", author: "B", message: "two", authorId: "u2", order: 11)
        q.apply(source: source, .clearUser(authorId: "u1", order: 10))
        expectEqual(q.items.map(\.key.id), ["b"], "only u1 cleared")
        expect(q.receive(source: source, id: "c", author: "A", message: "three", authorId: "u1", order: 9) == nil, "u1 fenced")
        q.apply(source: source, .ban(authorId: "u2"))
        expect(q.items.isEmpty, "ban retires u2")
        expect(q.receive(source: source, id: "d", author: "B", message: "four", authorId: "u2", order: 99) == nil, "banned")
    }

    /// A changed payload is a new revision but keeps the original receivedAt,
    /// so an edit stream cannot keep a message alive past its lifetime.
    static func testSocialEditKeepsReceivedAt() {
        var time = socialNow
        let q = SocialReview(now: { time }, lifetime: 10)
        let source = q.connect(provider: "YouTube")
        let first = q.receive(source: source, id: "m", author: "A", message: "one")!
        time += 9
        let edited = q.receive(source: source, id: "m", author: "A", message: "two")!
        expect(edited.key != first.key, "new revision")
        expectEqual(edited.receivedAt, first.receivedAt, "original receivedAt kept")
        time += 1
        expect(q.items.isEmpty, "expires 10 s after the FIRST arrival")
    }

    /// C# checks the raw UTF-16 length first (before trimming) and the UTF-8
    /// byte count after; either refuses, and nothing is ever truncated.
    static func testSocialByteAndUnitLimits() {
        let q = SocialReview(now: { socialNow })
        let source = q.connect(provider: "YouTube")
        let wide = String(repeating: "\u{E9}", count: 100)   // 100 UTF-16 units, 200 UTF-8 bytes
        expect(q.receive(source: source, id: "wide", author: "A", message: wide) == nil, "over 192 UTF-8 bytes: refused")
        let fits = String(repeating: "\u{E9}", count: 96)    // 192 bytes exactly
        expectEqual(q.receive(source: source, id: "fits", author: "A", message: fits)?.message, fits, "192 bytes fits")
        let padded = " " + String(repeating: "x", count: 192) // 193 units raw, 192 after trimming
        expect(q.receive(source: source, id: "padded", author: "A", message: padded) == nil,
               "the UTF-16 limit applies before trimming (C# text.Length)")
        expect(q.receive(source: source, id: "auth", author: String(repeating: "a", count: 81), message: "m") == nil,
               "author limit 80")
    }
}
