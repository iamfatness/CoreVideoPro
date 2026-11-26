// Social review sheet and model, plan 2026-11-22 Task 14.

import Foundation

extension ShellTests {
    static var socialSheetCases: [(String, () -> Void)] {
        [
            ("social/sheet-incoming-edit-clears-selection", testSocialSheetIncomingEditClearsSelection),
            ("social/sheet-enable-rules", testSocialSheetEnableRules),
            ("social/strip-hides-both-provider-cards-from-cued", testSocialStripHidesBothCards),
            ("social/control-api-cannot-reach-social", testSocialControlApiCannotReachSocial),
            ("social/disconnect-on-core-generation-change", testSocialDisconnectOnCoreGeneration),
            ("social/model-statuses-and-show-change", testSocialModelStatuses),
            ("social/model-expiry-tick-retires-an-expired-card", testSocialExpiryTick),
            ("social/youtube-key-in-keychain-not-prefs", testSocialYouTubeKeyInKeychain),
            ("social/redaction-sentinel", testSocialRedactionSentinel),
            ("social/control-snapshot-carries-no-chat", testSocialControlSnapshotCarriesNoChat),
            ("social/stale-disconnect-does-not-overwrite-a-newer-connect", testSocialStaleDisconnect),
        ]
    }

    /// GraphicsSocialReviewDialogTests.IncomingEditClearsSelectionAndRequiresFreshReview.
    static func testSocialSheetIncomingEditClearsSelection() {
        let q = SocialReview(now: { socialNow })
        let source = q.connect(provider: "YouTube")
        let original = q.receive(source: source, id: "item", author: "Author", message: "Original text")!
        expectEqual(SocialSheetRules.preserveSelection(q.items, original.key), original, "kept while unchanged")
        expect(q.approve(original.key), "approve")
        expectEqual(SocialSheetRules.preserveSelection(q.items, original.key)?.decision, .approved, "decision follows")
        let edited = q.receive(source: source, id: "item", author: "Author", message: "Changed text")!
        expect(SocialSheetRules.preserveSelection(q.items, original.key) == nil, "an edit drops the selection")
        expectEqual(SocialSheetRules.preserveSelection(q.items, edited.key), edited, "the new revision is selectable")
        expect(SocialSheetRules.preserveSelection(q.items, nil) == nil, "nothing selected")
    }

    static func testSocialSheetEnableRules() {
        func rules(_ d: SocialDecision?, connected: Bool = true, busy: Bool = false, disconnecting: Bool = false,
                   onProgram: Bool = false, running: Bool = false, provider: String = "YouTube",
                   video: String = "abcdefghijk", channel: String = "", client: String = "") -> SocialSheetRules {
            SocialSheetRules.evaluate(selected: d, connected: connected, busy: busy, disconnecting: disconnecting,
                                      onProgram: onProgram, running: running, provider: provider, videoId: video,
                                      channel: channel, clientId: client)
        }
        let pending = rules(.pending)
        expect(pending.approve && pending.reject && !pending.show && pending.withdraw, "pending: approve/reject/withdraw")
        let approved = rules(.approved)
        expect(!approved.approve && approved.show && approved.withdraw, "approved: show/withdraw")
        expect(!rules(.rejected).show && !rules(.rejected).approve && rules(.rejected).withdraw, "rejected: withdraw only")
        expect(!rules(nil).withdraw, "nothing selected: no withdraw")
        expect(!rules(.pending, connected: false).approve && !rules(.approved, connected: false).show, "disconnected: nothing")
        expect(!rules(.pending, busy: true).approve && !rules(.approved, busy: true).show, "busy: nothing")
        expect(rules(nil, onProgram: true).hide && !rules(nil, busy: true, onProgram: true).hide && !rules(nil).hide,
               "hide only while on Program and idle")
        expect(rules(nil).connect && !rules(nil, video: "short").connect, "connect needs an 11-character video id")
        expect(rules(nil, provider: "Twitch", channel: "somechannel").connect, "Twitch: a channel, blank client id")
        expect(!rules(nil, provider: "Twitch", channel: "somechannel", client: "short").connect, "Twitch: a bad client id")
        expect(rules(nil, provider: "Twitch", channel: "somechannel", client: "abcdef123456").connect, "Twitch: a good client id")
        expect(!rules(nil, connected: false, running: false).disconnect && rules(nil, connected: false, running: true).disconnect,
               "disconnect while running or connected")
        expect(!rules(nil, disconnecting: true).provider && !rules(nil, busy: true).clear, "provider/clear wait for idle")
    }

    static func testSocialStripHidesBothCards() {
        var state = GraphicsState()
        state.schemaVersion = 1; state.coreEpoch = "e"
        var a = GraphicsInstance(); a.instanceId = "social-review-a"; a.templateId = "youtube-chat-card"; a.cueRevision = 1
        var b = GraphicsInstance(); b.instanceId = "social-review-b"; b.templateId = "twitch-chat-card"; b.cueRevision = 1
        state.instances = [a, b]
        let cued = GraphicsStrip.project(state, rows: [])
        expectEqual(cued.onPreview.count, 0, "neither provider card is listed as cued")
        b.cueRevision = 0; b.programRevision = 1; b.visibility = "visible"
        state.instances = [b]
        expectEqual(GraphicsStrip.project(state, rows: []).onProgram.map(\.label), ["Twitch chat"], "the Twitch card is labelled")
        a.cueRevision = 0; a.programRevision = 1; a.visibility = "visible"
        state.instances = [a]
        expectEqual(GraphicsStrip.project(state, rows: []).onProgram.map(\.label), ["YouTube chat"], "the YouTube card is labelled")
    }

    /// No remote control action reaches comms or social: every graphics.* action
    /// except the lower-third pair is blocked, and no comms./social. id exists.
    static func testSocialControlApiCannotReachSocial() {
        let ids = ControlCatalog.all.map(\.action.id)
        expect(!ids.isEmpty, "the catalog is populated")
        expect(!ids.contains { $0.hasPrefix("comms.") || $0.hasPrefix("social.") }, "no comms./social. ids")
        for entry in ControlCatalog.all where entry.action.id.hasPrefix("graphics.") && !entry.action.id.hasPrefix("graphics.lowerThird.") {
            if case .blocked = entry.availability { continue }
            expect(false, "\(entry.action.id) must stay blocked (it could reach a social card)")
        }
    }

    @MainActor static func reviewModel(_ f: SocialFixture) -> SocialReviewModel {
        SocialReviewModel(playout: f.make(), showId: "show-a", sleep: { _ in })
    }

    @MainActor static func testSocialDisconnectOnCoreGeneration() {
        let model = AppModel()
        let review = model.graphics.socialReview
        run { await review.connectTestSource() }
        expect(review.isConnected, "connected")
        model.handleBridgeStatus(.connected(renderer: "metal"))
        model.stopTimersForTesting()
        spin(until: { !review.isConnected }, limit: 2)
        expect(!review.isConnected, "a new core generation disconnects review")
        expectEqual(review.status, "Disconnected. Review decisions cleared.", "Windows copy")
    }

    @MainActor static func testSocialModelStatuses() {
        let f = SocialFixture()
        let m = reviewModel(f)
        run { await m.connectYouTube(videoId: "short", key: "") }
        expectEqual(m.status, "Enter the 11-character video ID of a public YouTube livestream.", "bad video id")
        run { await m.connectYouTube(videoId: "abcdefghijk", key: "") }
        expectEqual(m.status, "Enter a YouTube Data API key.", "no key stored, none entered")
        run { await m.connectTwitch(channel: "bad channel", clientId: "") }
        expectEqual(m.status, "Enter a Twitch channel name and a public app Client ID, or reuse the saved sign-in.", "bad channel")
        run { await m.selectProvider("Twitch") }
        expectEqual(m.status, "Disconnected. Connect one Twitch live chat.", "provider change")
        expectEqual(m.sourceLabel, "Twitch live chat", "label")
        run { await m.connectTestSource() }
        run { await m.pasteTestMessage(author: "A", message: "hello") }
        guard let item = m.items.first else { expect(false, "test item"); return }
        m.decide(item.key, approve: true)
        expectEqual(m.status, "Approved. Use Show approved to put it on air.", "approved copy")
        m.decide(item.key, approve: true)
        expectEqual(m.status, "That version has changed. Select the current message and review it.", "decided twice")
        run { await m.showChanged(to: "show-b") }
        expectEqual(m.status, "Disconnected. Review decisions cleared.", "show change: Windows' ordinary disconnect status")
        expect(!m.isConnected && m.items.isEmpty, "show change disconnects and clears")
    }

    @MainActor static func testSocialExpiryTick() {
        var t = socialNow
        let f = SocialFixture()
        let playout = SocialPlayout(invoke: { _, _ in GraphicsOperationResult(accepted: true) }, observe: { f.state },
                                    sleep: { _ in }, now: { t })
        let m = SocialReviewModel(playout: playout, showId: "s", sleep: { _ in })
        run { await m.connectTestSource() }
        run { await m.pasteTestMessage(author: "A", message: "old") }
        expectEqual(m.items.count, 1, "queued")
        t += 15 * 60
        run { await m.sweep() }
        expect(m.items.isEmpty, "the sweep expires a 15-minute-old message")
    }

    @MainActor static func testSocialYouTubeKeyInKeychain() {
        let keychain = KeychainStore.defaultBackend as? MemoryKeychain
        let f = SocialFixture()
        let m = reviewModel(f)
        var started = false
        m.feeds.youTube = { _, key, _, _ in started = key == "AIzaTESTKEY1234567890ab" }
        run { await m.connectYouTube(videoId: "abcdefghijk", key: "AIzaTESTKEY1234567890ab") }
        spin(until: { started }, limit: 2)
        expect(started, "the feed got the key")
        expect(keychain?.keys.contains { $0.service == "us.iamfatness.corevideopro.social" && $0.account == "youtube-api-key" } == true,
               "the key is a Keychain item")
        let prefs = AppModel().currentPrefs()
        let json = String(data: (try? JSONEncoder().encode(prefs)) ?? Data(), encoding: .utf8) ?? ""
        expect(!json.contains("AIzaTESTKEY1234567890ab"), "and never in prefs")
        run { await m.connectYouTube(videoId: "abcdefghijk", key: "") }
        expectEqual(SocialCredentials.youTubeKey.readString(), "AIzaTESTKEY1234567890ab", "a blank field reuses the stored key")
        run { await m.clearLocalData() }
        expect(SocialCredentials.youTubeKey.readString() == nil, "Clear local data deletes it")
    }

    /// No author, message (or later key/token) reaches a support bundle.
    @MainActor static func testSocialRedactionSentinel() {
        let author = "SENTINEL_AUTHOR_7f3a", message = "SENTINEL_MSG_91c2", key = "AIzaSENTINELKEY1234567890ab"
        let model = AppModel()
        _ = SocialCredentials.youTubeKey.writeString(key)
        defer { SocialCredentials.youTubeKey.delete() }
        // A scripted session through the test source: approve, show, withdraw.
        let f = SocialFixture()
        let review = SocialReviewModel(playout: f.make(), showId: "s", sleep: { _ in })
        f.ready = true; f.autoPrepare = true
        run { await review.connectTestSource() }
        run { await review.pasteTestMessage(author: author, message: message) }
        guard let item = review.items.first else { expect(false, "item"); return }
        review.decide(item.key, approve: true)
        run { await review.show(item.key) }
        // The core snapshot as it looked while the card was on air.
        // The core's real row shape: the payload rides fields, cueFields and programFields.
        let payload: JSONObject = ["author": author, "message": message]
        let onAir: [JSONObject] = f.state.instances.map { ["instanceId": $0.instanceId, "templateId": "youtube-chat-card",
                                                          "revision": 1, "programRevision": 1, "visibility": "visible",
                                                          "fields": payload, "cueFields": payload, "programFields": payload,
                                                          "label": author, "warning": message] }
        model.applySnapshotForTesting(["graphics": ["schemaVersion": 1, "coreEpoch": "epoch", "instances": onAir] as JSONObject])
        run { await review.withdraw(item.key) }
        model.pushWarning("social: " + review.status)
        var input = model.supportBundleInput()
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("cvp-social-sentinel-\(getpid())")
        try? FileManager.default.removeItem(at: dir)
        input.destinationRoot = dir
        input.shellLogPath = "/nonexistent"; input.coreLogPath = "/nonexistent"; input.crashReportDirectory = "/nonexistent"
        guard let result = try? SupportBundle.write(input) else { expect(false, "bundle written"); return }
        defer { try? FileManager.default.removeItem(at: dir) }
        var scanned = 0
        let files = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil)?.compactMap { $0 as? URL } ?? []
        for file in files where !file.hasDirectoryPath {
            guard let data = try? Data(contentsOf: file) else { continue }
            scanned += 1
            let text = String(decoding: data, as: UTF8.self)
            for sentinel in [author, message, key] {
                expect(!text.contains(sentinel), "\(file.lastPathComponent) must not contain \(sentinel.prefix(14))…")
            }
        }
        expect(scanned >= 3, "the bundle has files to scan (\(scanned))")
        expect(!result.includedFiles.isEmpty, "the core snapshot was written")
    }

    /// GET /snapshot serves the published box: no social payload in it.
    @MainActor static func testSocialControlSnapshotCarriesNoChat() {
        let model = AppModel()
        let payload: JSONObject = ["author": "SENTINEL_AUTHOR_7f3a", "message": "SENTINEL_MSG_91c2"]
        model.applySnapshotForTesting(["graphics": ["schemaVersion": 1, "coreEpoch": "e", "instances": [
            ["instanceId": "social-review-1", "templateId": "twitch-chat-card", "revision": 2, "programRevision": 2,
             "visibility": "visible", "fields": payload, "cueFields": payload, "programFields": payload],
            ["instanceId": "playlist-title", "fields": ["name": "Keep me"]],
        ] as [JSONObject]] as JSONObject])
        guard let (published, _) = model.controlSnapshotBox.read(),
              let data = try? JSONSerialization.data(withJSONObject: published) else { expect(false, "published"); return }
        let text = String(decoding: data, as: UTF8.self)
        expect(!text.contains("SENTINEL_AUTHOR_7f3a") && !text.contains("SENTINEL_MSG_91c2"), "no chat in the control snapshot")
        expect(text.contains("Keep me"), "other graphics are untouched")
        let social = ((published["graphics"] as? JSONObject)?["instances"] as? [JSONObject])?.first
        expectEqual(social?["programRevision"] as? Int, 2, "state survives (the strip still reads on-air)")
        expectEqual(model.lastSnapshot.isEmpty, false, "the shell's own copy is intact for its read models")
    }

    /// A disconnect still suspended when a new connect begins must not reset
    /// the new connection's running flag or status.
    @MainActor static func testSocialStaleDisconnect() {
        let f = SocialFixture()
        let m = reviewModel(f)
        var gates: [CheckedContinuation<Void, Never>] = []
        m.feeds.youTube = { _, _, _, report in
            report("Connected to YouTube live chat.")
            await withCheckedContinuation { gates.append($0) }
        }
        _ = SocialCredentials.youTubeKey.writeString("AIzaTESTKEY1234567890ab")
        defer { SocialCredentials.youTubeKey.delete() }
        run { await m.connectYouTube(videoId: "abcdefghijk", key: "") }
        spin(until: { gates.count == 1 }, limit: 1)
        let stale = Task { @MainActor in await m.disconnect() }   // suspends awaiting feed 1
        spin(until: { false }, limit: 0.1)
        let fresh = Task { @MainActor in await m.connectYouTube(videoId: "abcdefghijk", key: "") }
        spin(until: { gates.count == 2 }, limit: 1)               // feed 2 is running
        gates[0].resume()                                         // feed 1 ends: the stale disconnect resumes
        _ = awaitMain(3) { await stale.value; await fresh.value; return true }
        spin(until: { false }, limit: 0.2)
        expect(m.running, "the newer connection is still running")
        expectEqual(m.status, "abcdefghijk · Connected to YouTube live chat.", "and its status stands")
        if gates.count > 1 { gates[1].resume() }
        run { await m.disconnect() }
    }
}
