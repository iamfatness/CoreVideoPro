// The Social review sheet's model: a port of
// native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.GraphicsSocialReview.cs
// (GraphicsSocialReviewViewModel). It owns nothing secret on disk: the YouTube
// key and the Twitch sign-in live in the login Keychain (SocialCredentials);
// the video id and channel live in memory only, as on Windows. Status strings
// are Windows' verbatim. No chat payload, author, key or cursor is logged.

import Foundation

/// The Keychain items (spec B1/B3/B4): one generic-password item per secret,
/// service us.iamfatness.corevideopro.social. Nothing else is persisted.
enum SocialCredentials {
    static let service = "us.iamfatness.corevideopro.social"
    static var youTubeKey: KeychainStore { KeychainStore(service: service, account: "youtube-api-key") }
    /// One JSON blob, so a rotated access+refresh pair is stored atomically.
    static var twitch: KeychainStore { KeychainStore(service: service, account: "twitch-credentials") }
}

/// A Twitch device-code prompt (verification_uri already validated).
struct TwitchDevicePrompt: Equatable {
    let userCode: String
    let verificationUri: URL
}

/// The provider feeds the model starts. Tasks 15-17 supply the real ones; until
/// then each reports that it is not available, and the dev-only test source
/// (COREVIDEO_SHELL_SOCIAL_TEST_SOURCE=1) proves the review path without them.
struct SocialFeedRunners {
    typealias Report = @MainActor (String) -> Void
    var youTube: @MainActor (_ videoId: String, _ key: String, _ playout: SocialPlayout, _ report: @escaping Report) async -> Void
    var twitch: @MainActor (_ channel: String, _ clientId: String, _ playout: SocialPlayout, _ report: @escaping Report,
                            _ prompt: @escaping @MainActor (TwitchDevicePrompt) -> Void) async -> Void

    /// Production: YouTube (Task 15) and Twitch EventSub (Tasks 16-17) over the shared transport.
    static let live = SocialFeedRunners(
        youTube: { videoId, key, playout, report in
            await YouTubeSocialFeed.run(videoId: videoId, key: key, playout: playout, http: URLSessionSocialHTTP.shared,
                                        sleep: { try? await Task.sleep(nanoseconds: $0) }, report: report)
        },
        twitch: { channel, clientId, playout, report, prompt in
            await TwitchSocialFeed.run(channel: channel, clientId: clientId, auth: sharedTwitchAuthorization, playout: playout,
                                       http: URLSessionSocialHTTP.shared,
                                       connect: { try await URLSessionTwitchSocket.connect() },
                                       sleep: { try? await Task.sleep(nanoseconds: $0) }, report: report, prompt: prompt)
        })

    /// One review owner's authorization (Windows keeps one per view model): the
    /// hourly validation clock and the rotation lock live for the process.
    @MainActor static let sharedTwitchAuthorization = TwitchAuthorization.keychain()

    static let unavailable = SocialFeedRunners(
        youTube: { _, _, _, report in report("YouTube live chat is not available in this build.") },
        twitch: { _, _, _, report, _ in report("Twitch chat is not available in this build.") })

    /// YouTubeChatTransport.IsVideoId: 11 ASCII letters, digits, '-' or '_'.
    static func isVideoId(_ s: String) -> Bool {
        s.utf16.count == 11 && s.unicodeScalars.allSatisfy {
            ("a"..."z").contains($0) || ("A"..."Z").contains($0) || ("0"..."9").contains($0) || $0 == "-" || $0 == "_"
        }
    }

    /// TwitchHttpTransport.Login: 1-25 ASCII letters, digits or underscores.
    static func isChannel(_ s: String) -> Bool {
        (1...25).contains(s.utf16.count) && s.unicodeScalars.allSatisfy {
            ("a"..."z").contains($0) || ("A"..."Z").contains($0) || ("0"..."9").contains($0) || $0 == "_"
        }
    }

    /// TwitchHttpTransport.ClientId: 10-128 ASCII letters or digits.
    static func isClientId(_ s: String) -> Bool {
        (10...128).contains(s.utf16.count) && s.unicodeScalars.allSatisfy {
            ("a"..."z").contains($0) || ("A"..."Z").contains($0) || ("0"..."9").contains($0)
        }
    }
}

@MainActor
final class SocialReviewModel: ObservableObject {
    let playout: SocialPlayout
    var feeds: SocialFeedRunners

    @Published private(set) var provider = "YouTube"
    @Published private(set) var videoId = ""
    @Published private(set) var channel = ""
    @Published private(set) var twitchClientId = ""
    @Published private(set) var devicePrompt: TwitchDevicePrompt?
    @Published private(set) var status = "Disconnected. Connect a public YouTube livestream."
    @Published private(set) var running = false
    /// Bumped on every refresh so the sheet re-reads the queue.
    @Published private(set) var revision = 0

    private var generation = 0
    private var feed: Task<Void, Never>?
    private var tick: Task<Void, Never>?
    private var closed = false
    private var showId: String
    private let sleep: (UInt64) async -> Void

    init(playout: SocialPlayout, showId: String, feeds: SocialFeedRunners = .live,
         sleep: @escaping (UInt64) async -> Void = { try? await Task.sleep(nanoseconds: $0) }) {
        self.playout = playout
        self.showId = showId
        self.feeds = feeds
        self.sleep = sleep
    }

    var sourceLabel: String {
        let source = provider == "Twitch" ? channel : videoId
        return source.isEmpty ? provider + " live chat" : provider + " · " + source
    }

    var items: [SocialItem] { playout.items }
    var isConnected: Bool { playout.isConnected }
    var onProgram: Bool { playout.onProgram }

    /// The 1 s expiry sweep (Windows OnTick): expired items leave the queue and
    /// an on-air card whose item expired is retired.
    func startExpiryTick() {
        guard tick == nil else { return }
        tick = Task { @MainActor [weak self] in
            while let self, !self.closed, !Task.isCancelled {
                await self.sleep(1_000_000_000)
                await self.sweep()
            }
        }
    }

    func sweep() async {
        guard !closed else { return }
        await playout.expire()
        changed()
    }

    func selectProvider(_ next: String) async {
        guard SocialReview.providers.contains(next), next != provider else { return }
        await disconnect()
        provider = next
        setStatus("Disconnected. Connect one " + next + " live chat.")
    }

    func connectYouTube(videoId: String, key: String) async {
        guard SocialFeedRunners.isVideoId(videoId) else {
            setStatus("Enter the 11-character video ID of a public YouTube livestream.")
            return
        }
        let requestedShow = showId
        await disconnect()
        // Windows: a show change while connecting cancels the connect.
        guard requestedShow == showId else { return }
        let store = SocialCredentials.youTubeKey
        let resolved: String?
        if key.isEmpty {
            resolved = store.readString()
        } else {
            guard store.writeString(key) else {
                setStatus("The local YouTube key could not be saved. Check the key and local access.")
                return
            }
            resolved = key
        }
        guard !closed, let resolved, !resolved.isEmpty else {
            if !closed { setStatus("Enter a YouTube Data API key.") }
            return
        }
        self.videoId = videoId
        provider = "YouTube"
        let generation = begin()
        setStatus("Connecting to YouTube…")
        let feeds = self.feeds, playout = self.playout
        run(source: videoId, generation: generation) { report in
            await feeds.youTube(videoId, resolved, playout, report)
        }
    }

    func connectTwitch(channel: String, clientId: String) async {
        guard SocialFeedRunners.isChannel(channel), clientId.isEmpty || SocialFeedRunners.isClientId(clientId) else {
            setStatus("Enter a Twitch channel name and a public app Client ID, or reuse the saved sign-in.")
            return
        }
        let requestedShow = showId
        await disconnect()
        guard !closed, requestedShow == showId else { return }
        provider = "Twitch"
        self.channel = channel.lowercased()
        twitchClientId = clientId
        let generation = begin()
        setStatus("Authorizing Twitch chat…")
        let feeds = self.feeds, playout = self.playout, lowered = self.channel
        run(source: lowered, generation: generation) { [weak self] report in
            await feeds.twitch(lowered, clientId, playout, report) { prompt in
                guard let self, self.generation == generation else { return }
                self.devicePrompt = prompt
                self.setStatus("Enter the displayed code at Twitch to authorize read-only chat access.")
            }
        }
    }

    /// Dev-only (COREVIDEO_SHELL_SOCIAL_TEST_SOURCE=1): a local "YouTube" source
    /// whose messages the operator pastes. Proves the review path before the
    /// provider transports exist; absent unless the variable is set.
    static var testSourceEnabled: Bool { ProcessInfo.processInfo.environment["COREVIDEO_SHELL_SOCIAL_TEST_SOURCE"] == "1" }
    private(set) var testSource: Int64?
    private var testCounter = 0

    func connectTestSource() async {
        await disconnect()
        provider = "YouTube"
        _ = begin()
        testSource = await playout.connect(provider: "YouTube")
        setStatus("Local test source · connected.")
    }

    func pasteTestMessage(author: String, message: String) async {
        guard let source = testSource else { return }
        testCounter += 1
        _ = await playout.receive(source: source, id: "test-\(testCounter)", author: author, message: message)
        changed()
    }

    func disconnect() async {
        generation += 1
        let mine = generation
        devicePrompt = nil
        testSource = nil
        let previous = feed
        feed = nil
        previous?.cancel()
        await playout.disconnect()
        await previous?.value
        // A connect that started while this disconnect was suspended owns the
        // state now: do not overwrite its running flag or status.
        guard mine == generation else { return }
        running = false
        setStatus("Disconnected. Review decisions cleared.")
    }

    func clearLocalData() async {
        await disconnect()
        let deleted: Bool
        if provider == "Twitch" {
            deleted = SocialCredentials.twitch.delete()
            channel = ""; twitchClientId = ""
        } else {
            deleted = SocialCredentials.youTubeKey.delete()
            videoId = ""
        }
        // Windows reports a failed clear through its dialog's catch.
        setStatus(deleted ? "Local " + provider + " credentials and review data cleared."
                          : "Social review could not complete that action. Check the connection and retry.")
    }

    func decide(_ key: SocialItemKey, approve: Bool) {
        let decided = approve ? playout.approve(key) : playout.reject(key)
        setStatus(decided ? (approve ? "Approved. Use Show approved to put it on air." : "Rejected.")
                          : "That version has changed. Select the current message and review it.")
    }

    func show(_ key: SocialItemKey) async {
        let result = await playout.show(key)
        setStatus(result.accepted ? "Show requested. Program confirms when the graphic is on air." : result.reason)
    }

    func hide() async {
        setStatus(await playout.hide() ? "Social graphic hidden." : "Could not clear the social graphic. It will expire.")
    }

    func withdraw(_ key: SocialItemKey) async {
        setStatus(await playout.withdraw(key) ? "Message withdrawn." : "That version has changed. Select the current message.")
    }

    /// The show changed (a different graphics document): stop reviewing.
    func showChanged(to next: String) async {
        guard !closed, next != showId else { return }
        showId = next
        // Windows leaves the ordinary disconnect status ("The show changed…" is
        // only its exception path).
        await disconnect()
    }

    /// A new core generation: the old core's card and preparation are gone;
    /// decisions made against it must not carry over (the next Show also
    /// reinstalls the template on the new core).
    func coreGenerationChanged() async {
        guard isConnected || running else { return }
        await disconnect()
    }

    /// Quit: disconnect (retires a card) within the bound, before the core stops.
    func stopForShutdown() async {
        guard !closed else { return }
        closed = true
        tick?.cancel()
        await disconnect()
    }

    // ── internals ────────────────────────────────────────────────────────

    private func begin() -> Int {
        generation += 1
        running = true
        return generation
    }

    private func run(source: String, generation: Int, _ body: @escaping @MainActor (@escaping SocialFeedRunners.Report) async -> Void) {
        feed = Task { @MainActor [weak self] in
            await body { status in
                guard let self, self.generation == generation else { return }
                if self.playout.isConnected { self.devicePrompt = nil }
                self.setStatus(source + " · " + status)
            }
            guard let self else { return }
            if self.generation == generation {
                self.running = false
                self.devicePrompt = nil
            }
            self.changed()
        }
    }

    private func setStatus(_ text: String) {
        status = text
        changed()
    }

    private func changed() {
        guard !closed else { return }
        revision &+= 1
    }
}

/// What the sheet enables (GraphicsSocialReviewDialog.RefreshSelection, :164-178). Pure.
struct SocialSheetRules: Equatable {
    var approve = false, reject = false, show = false, withdraw = false, hide = false
    var connect = false, provider = false, clear = false, disconnect = false

    static func evaluate(selected: SocialDecision?, connected: Bool, busy: Bool, disconnecting: Bool, onProgram: Bool,
                         running: Bool, provider: String, videoId: String, channel: String, clientId: String) -> SocialSheetRules {
        let active = !busy && !disconnecting && connected
        var r = SocialSheetRules()
        r.approve = active && selected == .pending
        r.reject = r.approve
        r.show = active && selected == .approved
        r.withdraw = active && selected != nil
        r.hide = !busy && onProgram
        r.connect = !busy && !disconnecting && (provider == "Twitch"
            ? SocialFeedRunners.isChannel(channel) && (clientId.isEmpty || SocialFeedRunners.isClientId(clientId))
            : SocialFeedRunners.isVideoId(videoId))
        r.provider = !busy && !disconnecting
        r.clear = !busy && !disconnecting
        r.disconnect = !disconnecting && (running || connected)
        return r
    }

    /// GraphicsSocialReviewDialog.PreserveSelection: a changed revision drops the
    /// selection, so an old click can never approve the replacement unread.
    static func preserveSelection(_ items: [SocialItem], _ key: SocialItemKey?) -> SocialItem? {
        guard let key else { return nil }
        return items.first { $0.key == key }
    }
}

/// Keeps social review out of diagnostics: no chat payload, author, API key or
/// token reaches a support bundle (slice-6 spec :15).
enum SocialRedaction {
    /// A social instance keeps only these non-textual keys. An allow-list,
    /// not a deny-list: the core publishes the payload under fields, cueFields
    /// AND programFields (GraphicsInstances.cpp), and a future key must not leak.
    static let socialInstanceKeys: Set<String> = [
        "instanceId", "templateId", "templateVersion", "scope", "sceneId", "revision", "programRevision", "cueRevision",
        "cuePending", "cuePreparation", "preparation", "visibility", "cued", "showPending", "firstDrawSubmittedFrame",
        "currentlyComposed",
    ]

    /// The core snapshot with every social-review- instance reduced to its
    /// non-textual keys. Everything else is untouched; a snapshot with no
    /// social instance is returned as is.
    static func scrub(snapshot: JSONObject) -> JSONObject {
        guard var graphics = snapshot["graphics"] as? JSONObject,
              let instances = graphics["instances"] as? [JSONObject],
              instances.contains(where: isSocial) else { return snapshot }
        graphics["instances"] = instances.map { instance -> JSONObject in
            guard isSocial(instance) else { return instance }
            var copy = instance.filter { socialInstanceKeys.contains($0.key) }
            copy["redacted"] = "social review"
            return copy
        }
        var out = snapshot
        out["graphics"] = graphics
        return out
    }

    private static func isSocial(_ instance: JSONObject) -> Bool {
        (instance["instanceId"] as? String)?.hasPrefix(SocialPlayout.instancePrefix) == true
    }

    /// Stored social secrets, handed to the bundle writer as literals to scrub
    /// by identity (never written).
    static func secretLiterals() -> [String] {
        var out: [String] = []
        if let key = SocialCredentials.youTubeKey.readString() { out.append(key) }
        if let blob = SocialCredentials.twitch.read(),
           let json = (try? JSONSerialization.jsonObject(with: blob)) as? JSONObject {
            for name in ["accessToken", "refreshToken"] { if let v = json[name] as? String { out.append(v) } }
        }
        return out
    }
}
