// mac streaming Phase 2, slice D1 (plan 2026-11-21-mac-streaming-phase2). Kept in
// its own file, like ShellTestsMedia.swift, so parallel branches extending
// ShellTests.swift do not collide line-for-line.
//
// Every expectation is a constant the fixture chose (a sentence, a label, a
// number), never a value read back from the code under test.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var streamingCases: [(String, () -> Void)] {
        [
            ("streaming/srt-validation-matches-windows", testSrtValidationMatchesWindows),
            ("streaming/hls-validation-matches-windows", testHlsValidationMatchesWindows),
            ("streaming/selected-destinations-refusal-order", testSelectedDestinationsRefusalOrder),
            ("streaming/payload-per-protocol", testDestinationPayloadPerProtocol),
            ("streaming/invalid-row-never-sent", testAnInvalidEnabledRowIsNeverSent),
            ("streaming/status-projection-table", testDestinationStatusProjectionTable),
            ("streaming/status-never-live-from-a-launch", testStatusIsNeverLiveFromALaunch),
            ("streaming/start-sends-every-valid-destination", testStartSendsEveryValidDestination),
            ("streaming/start-refuses-with-the-windows-sentence", testStartRefusesWithTheWindowsSentence),
            ("streaming/retry-sends-recover-output-sender", testRetrySendsRecoverOutputSender),
            ("streaming/snapshot-projects-rows-and-quit-status", testSnapshotProjectsRowsAndQuitStatus),
            ("streaming/prefs-keep-rows-never-the-passphrase", testPrefsKeepRowsNeverThePassphrase),
            ("streaming/footer-health-reads-the-projection", testFooterHealthReadsTheProjection),
            ("streaming/launch-env-names-the-bundled-guard-and-ffmpeg", testLaunchEnvNamesTheBundledGuardAndFfmpeg),
            ("streaming/unguarded-helper-is-loud", testAnUnguardedHelperIsLoud),
            ("streaming/destinations-are-frozen-while-streaming", testDestinationsAreFrozenWhileStreaming),
        ]
    }

    // ── validation (ports of StudioStreamOutputValidationTests) ─────────────

    private static func srt(_ mode: String = "caller", host: String = "receiver.example.com", port: String = "9000",
                            latency: String = "120", streamId: String = "", key: String = "0",
                            passphrase: String = "") -> String? {
        StreamDestinationPolicy.validateSrt(mode: mode, host: host, port: port, latencyMs: latency,
                                            streamId: streamId, keyLength: key, passphrase: passphrase)
    }

    static func testSrtValidationMatchesWindows() {
        for mode in ["caller", "listener", "rendezvous"] {
            expect(srt(mode) == nil, "SRT \(mode) without encryption is valid (Srt_AcceptsSupportedModes)")
        }
        expectEqual(srt(key: "16"), "Configure an SRT passphrase when encryption is enabled.",
                    "Srt_RequiresPassphraseWhenEncryptionEnabled")
        expectEqual(srt(passphrase: "unused-passphrase"), "Choose an SRT key length before entering a passphrase.",
                    "Srt_RejectsPassphraseWhenEncryptionDisabled")
        expect(srt(port: "") != nil, "Srt_DoesNotSerializeSettingsUntilFullyValid: empty port refused")
        expectEqual(srt("broadcast"), "Choose SRT mode caller, listener, or rendezvous before streaming.", "bad mode")
        expectEqual(srt(host: "  "), "Configure SRT host before streaming.", "blank host")
        expectEqual(srt(port: "70000"), "Configure SRT port between 1 and 65535 before streaming.", "port range")
        expectEqual(srt(latency: "19"), "Configure SRT latency of at least 20 ms before streaming.", "latency floor")
        expect(srt(latency: "20") == nil, "20 ms latency is the floor, accepted")
        expectEqual(srt(streamId: String(repeating: "s", count: 513)),
                    "SRT stream ID must be 512 characters or less.", "stream id bound")
        expectEqual(srt(key: "8"), "Choose SRT encryption key length 0, 16, 24, or 32.", "key length set")
        expectEqual(srt(key: "16", passphrase: "short"),
                    "SRT passphrase must be 10 to 79 characters when encryption is enabled.", "passphrase length")
        expect(srt(key: "32", passphrase: "0123456789") == nil, "10-character passphrase with AES-256 is valid")
    }

    static func testHlsValidationMatchesWindows() {
        let first = "Configure an http:// or https:// HLS playlist URL before streaming."
        let second = "HLS playlist URL must end in .m3u8 and contain no credentials, query, fragment, or whitespace."
        expect(StreamDestinationPolicy.validateHls("https://origin.example/live/program.m3u8") == nil, "https m3u8 valid")
        expectEqual(StreamDestinationPolicy.validateHls("srt://origin.example/live/program.m3u8"), first, "scheme")
        expectEqual(StreamDestinationPolicy.validateHls(""), first, "empty")
        expectEqual(StreamDestinationPolicy.validateHls("https://user:secret@origin.example/live/program.m3u8"), second, "user info")
        expectEqual(StreamDestinationPolicy.validateHls("https://origin.example/live/program.m3u8?token=secret"), second, "query")
        expectEqual(StreamDestinationPolicy.validateHls("https://origin.example/live/program.ts"), second, "not m3u8")
        expectEqual(StreamDestinationPolicy.validateHls("https://origin.example/live/pro gram.m3u8"), second,
                    "whitespace reaches Windows' SECOND sentence, not the first")
    }

    static func testSelectedDestinationsRefusalOrder() {
        var config = StreamDestinationsConfig()
        config.rtmpEnabled = false
        expectEqual(StreamDestinationPolicy.validateSelected(config, StreamDestinationSecrets()),
                    "Select at least one stream destination.", "nothing enabled")
        config.rtmpEnabled = true
        config.srtEnabled = true                 // and SRT is incomplete (no host)
        expectEqual(StreamDestinationPolicy.validateSelected(config, StreamDestinationSecrets()),
                    "Configure RTMP stream key before streaming.", "RTMP is judged first, in Windows order")
        let secrets = StreamDestinationSecrets(rtmpKey: "abcd-efgh")
        expectEqual(StreamDestinationPolicy.validateSelected(config, secrets),
                    "Configure SRT host before streaming.", "then SRT")
        config.srtHost = "receiver.example.com"
        expect(StreamDestinationPolicy.validateSelected(config, secrets) == nil, "both valid")
    }

    // ── payload ──────────────────────────────────────────────────────────────

    static func testDestinationPayloadPerProtocol() {
        var config = StreamDestinationsConfig()
        config.rtmpUrl = "rtmp://live.example.com/app"
        config.srtEnabled = true
        config.srtHost = "receiver.example.com"
        config.srtPort = "9001"
        config.srtLatencyMs = "250"
        config.srtKeyLength = "16"
        config.srtStreamId = "#!::r=live"
        config.hlsEnabled = true
        config.hlsPlaylistUrl = "https://origin.example/live/program.m3u8"
        let secrets = StreamDestinationSecrets(rtmpKey: "rtmp-secret-key", srtPassphrase: "srt-passphrase-1")
        expectEqual(StreamDestinationPolicy.enabledDestinations(config, secrets), ["rtmp", "srt", "hls"],
                    "destinations in rtmp/srt/hls order")
        let rows = StreamDestinationPolicy.destinationSettings(config, secrets, bitrateMbps: 8.5)
        expectEqual(rows.count, 3, "one settings row per destination")
        let rtmp = rows.first { $0["id"] as? String == "rtmp" } ?? [:]
        expectEqual(rtmp["url"] as? String, "rtmp://live.example.com/app", "rtmp url")
        expectEqual(rtmp["streamKey"] as? String, "rtmp-secret-key", "rtmp key")
        expectEqual(rtmp["targetBitrateMbps"] as? Double, 8.5, "rtmp bitrate is the operator's")
        let srtRow = rows.first { $0["id"] as? String == "srt" } ?? [:]
        expectEqual(srtRow["mode"] as? String, "caller", "srt mode")
        expectEqual(srtRow["host"] as? String, "receiver.example.com", "srt host")
        expectEqual(srtRow["port"] as? Int, 9001, "srt port is an integer")
        expectEqual(srtRow["latencyMs"] as? Int, 250, "srt latency ms")
        expectEqual(srtRow["latencyUs"] as? Int, 250_000, "srt latency us (Windows sends both)")
        expectEqual(srtRow["keyLength"] as? Int, 16, "srt key length")
        expectEqual(srtRow["passphrase"] as? String, "srt-passphrase-1", "srt passphrase")
        expectEqual(srtRow["streamId"] as? String, "#!::r=live", "srt stream id")
        expectEqual(srtRow["targetBitrateMbps"] as? Double, 8.5, "srt carries the stream bitrate (D13)")
        let hls = rows.first { $0["id"] as? String == "hls" } ?? [:]
        expectEqual(hls["protocol"] as? String, "hls", "hls protocol")
        expectEqual(hls["url"] as? String, "https://origin.example/live/program.m3u8", "hls url")
        expectEqual(hls["videoCodec"] as? String, "h264", "HLS requires H.264 (validateHlsSettings)")
        // Secrets appear only in their own field.
        for row in rows {
            for (key, value) in row where key != "streamKey" && key != "passphrase" {
                let text = "\(value)"
                expect(!text.contains("rtmp-secret-key") && !text.contains("srt-passphrase-1"),
                       "no secret leaks into \(row["id"] ?? "?").\(key)")
            }
        }
        config.srtStreamId = ""
        let noId = StreamDestinationPolicy.destinationSettings(config, secrets, bitrateMbps: 8.5)
        expect(noId.first { $0["id"] as? String == "srt" }?["streamId"] == nil, "empty stream id is omitted")
    }

    static func testAnInvalidEnabledRowIsNeverSent() {
        var config = StreamDestinationsConfig()
        config.srtEnabled = true          // no host: invalid
        config.hlsEnabled = true
        config.hlsPlaylistUrl = "https://origin.example/live/program.ts"   // invalid
        let secrets = StreamDestinationSecrets(rtmpKey: "abcd-efgh")
        expectEqual(StreamDestinationPolicy.enabledDestinations(config, secrets), ["rtmp"], "only the valid row")
        let rows = StreamDestinationPolicy.destinationSettings(config, secrets, bitrateMbps: 6)
        expectEqual(rows.map { $0["id"] as? String ?? "" }, ["rtmp"], "only the valid row's settings")
        expectEqual(StreamDestinationPolicy.rowError("srt", config, secrets), "Configure SRT host before streaming.",
                    "the SRT row explains itself")
        config.srtEnabled = false
        expect(StreamDestinationPolicy.rowError("srt", config, secrets) == nil, "a disabled row has no error")
    }

    // ── status projection ────────────────────────────────────────────────────

    private static func sender(_ state: String?, status: String = "live", supervisor: JSONObject? = nil,
                               error: String? = nil, warning: String? = nil) -> JSONObject {
        var row: JSONObject = ["destination": "rtmp", "status": status, "framesSent": 10]
        if let state {
            var lifecycle: JSONObject = ["sessionId": "s", "desiredActive": true, "state": state,
                                         "health": "unknown", "finalized": false]
            if let error { lifecycle["error"] = error }
            row["lifecycle"] = lifecycle
        }
        if let supervisor { row["supervisor"] = supervisor }
        if let warning { row["warning"] = warning }
        return row
    }

    private static func sup(healthy: Bool, gaveUp: Bool = false, failures: Int = 0, nextMs: Double = 0,
                            progressAgeMs: Double = -1, reason: String = "") -> JSONObject {
        ["healthy": healthy, "gaveUp": gaveUp, "consecutiveFailures": failures, "nextAttemptInMs": nextMs,
         "lastProgressAgeMs": progressAgeMs, "reason": reason, "failureClass": "none"]
    }

    static func testDestinationStatusProjectionTable() {
        typealias P = DestinationStatusProjection
        expectEqual(P.project(nil, desired: true).label, "Starting", "no sender yet, desired")
        expectEqual(P.project(nil, desired: false).label, "Off", "no sender, not desired")
        expectEqual(P.project(sender("preparing", supervisor: sup(healthy: false)), desired: true).label,
                    "Connecting", "preparing, no failures")
        let reconnecting = P.project(sender("preparing", supervisor: sup(healthy: false, failures: 2, nextMs: 4200,
                                                                         reason: "ffmpeg exited")), desired: true)
        expectEqual(reconnecting.label, "Reconnecting in 5 s", "4200 ms rounds UP to 5 s")
        expectEqual(reconnecting.detail, "ffmpeg exited", "the supervisor's reason")
        let live = P.project(sender("producing", supervisor: sup(healthy: true)), desired: true)
        expectEqual(live, DestinationStatus(label: "Live", tone: .good, isLive: true), "producing + healthy")
        let unconfirmed = P.project(sender("producing", supervisor: sup(healthy: false, progressAgeMs: 2300)),
                                    desired: true)
        expectEqual(unconfirmed.label, "Not confirmed", "producing but supervisor unhealthy")
        expectEqual(unconfirmed.detail, "No media accepted for 3 s.", "2300 ms of no progress")
        expect(!unconfirmed.isLive, "not confirmed is not live")
        let interrupted = P.project(sender("interrupted", supervisor: sup(healthy: false, failures: 1, nextMs: 9000,
                                                                          progressAgeMs: 1500)), desired: true)
        expectEqual(interrupted.label, "Interrupted", "interrupted")
        expectEqual(interrupted.detail, "No media accepted for 2 s; retrying in 9 s.", "interrupted detail")
        let gaveUp = P.project(sender("failed", status: "failed",
                                      supervisor: sup(healthy: false, gaveUp: true, failures: 5,
                                                      reason: "gave up after 5 consecutive failures")),
                               desired: true)
        expectEqual(gaveUp.label, "Stopped retrying", "gave up")
        expect(gaveUp.showsRetry, "gave up offers Retry")
        expectEqual(gaveUp.detail, "gave up after 5 consecutive failures", "gave-up reason")
        // Give-up is the supervisor's word, whatever the lifecycle reads.
        let gaveUpInterrupted = P.project(sender("interrupted", status: "failed",
                                                 supervisor: sup(healthy: false, gaveUp: true, failures: 5,
                                                                 progressAgeMs: 30_000, reason: "endpoint-missing")),
                                          desired: true)
        expectEqual(gaveUpInterrupted.label, "Stopped retrying", "gave up while the lifecycle reads interrupted")
        expect(gaveUpInterrupted.showsRetry, "and still offers Retry")
        let failed = P.project(sender("failed", status: "failed", supervisor: sup(healthy: false),
                                      error: "endpoint refused"), desired: true)
        expectEqual(failed.label, "Failed", "failed without give-up")
        expectEqual(failed.detail, "endpoint refused", "lifecycle error")
        expect(!failed.showsRetry, "Retry only on give-up (the supervisor is still in charge)")
        expectEqual(P.project(sender("stopping", supervisor: sup(healthy: true)), desired: false).label,
                    "Stopping", "stopping")
        expectEqual(P.project(sender("finalizing", supervisor: sup(healthy: true)), desired: false).label,
                    "Stopping", "finalizing")
        expectEqual(P.project(sender("completed", supervisor: sup(healthy: false)), desired: false).label,
                    "Stopped", "completed")
        expectEqual(P.project(sender(nil), desired: true).label, "Unknown", "absent lifecycle is UNKNOWN")
        expectEqual(P.project(sender("producing"), desired: true).label, "Unsupervised",
                    "producing without a supervisor node is not claimed live")
        let session: JSONObject = ["senders": [sender("producing", supervisor: sup(healthy: true)),
                                               ["destination": "srt", "status": "starting"]]]
        expectEqual(P.sender("srt", in: session)?["status"] as? String, "starting", "sender lookup by destination")
        expect(P.sender("hls", in: session) == nil, "absent destination")
    }

    /// CLAUDE.md "Destination lifecycle is TRUTHFUL": the published status can
    /// still read "live" from the adapter's LAUNCH while nothing was accepted.
    static func testStatusIsNeverLiveFromALaunch() {
        let launched = sender("preparing", status: "live", supervisor: sup(healthy: false))
        let projected = DestinationStatusProjection.project(launched, desired: true)
        expectEqual(projected.label, "Connecting", "a launch reads Connecting")
        expect(!projected.isLive, "a launch is never live")
        let retiredLive = sender("live", status: "live", supervisor: sup(healthy: false))
        expect(!DestinationStatusProjection.project(retiredLive, desired: true).isLive,
               "the retired 'live' lifecycle name still needs a healthy supervisor")
    }

    // ── AppModel wiring (Task 7) ─────────────────────────────────────────────

    /// A model whose outbound requests are captured, and whose Keychain writes
    /// go nowhere (the harness must never touch the operator's Keychain).
    @MainActor
    private static func streamingModel() -> (AppModel, () -> [JSONObject], () -> [(String, String)]) {
        let model = AppModel()
        final class Box: @unchecked Sendable {
            let lock = NSLock()
            var bodies: [JSONObject] = []
            var saved: [(String, String)] = []
        }
        let box = Box()
        model.quitRequestForTesting = { body, _ in
            box.lock.withLock { box.bodies.append(body) }
            return [:]
        }
        model.streamSecretSaver = { value, account in box.lock.withLock { box.saved.append((value, account)) } }
        return (model, { box.lock.withLock { box.bodies } }, { box.lock.withLock { box.saved } })
    }

    private static func commands(_ body: JSONObject?) -> [JSONObject] { body?["commands"] as? [JSONObject] ?? [] }

    @MainActor
    static func testStartSendsEveryValidDestination() {
        let (model, bodies, saved) = streamingModel()
        model.streamKey = "rtmp-key-1"
        model.streamDestinations.srtEnabled = true
        model.streamDestinations.srtHost = "receiver.example.com"
        model.streamDestinations.srtKeyLength = "16"
        model.srtPassphrase = "srt-passphrase-1"
        model.toggleStreaming()
        spin(until: { !bodies().isEmpty }, limit: 3)
        let start = commands(bodies().first).first { $0["type"] as? String == "start-program-output" }
        expectEqual(start?["destinations"] as? [String], ["rtmp", "srt"], "both valid destinations start")
        let ids = (start?["destinationSettings"] as? [JSONObject])?.map { $0["id"] as? String ?? "" }
        expectEqual(ids, ["rtmp", "srt"], "one settings row each")
        expect(model.streamingDesired, "streaming intent set")
        let accounts = saved().map(\.1)
        expect(accounts.contains("rtmp") && accounts.contains(StreamKeychain.srtPassphraseAccount),
               "both secrets go to the Keychain seam, never prefs")
    }

    @MainActor
    static func testStartRefusesWithTheWindowsSentence() {
        let (model, bodies, _) = streamingModel()
        model.streamDestinations.rtmpEnabled = false
        model.streamDestinations.srtEnabled = true          // no host
        model.toggleStreaming()
        spin(until: { false }, limit: 0.3)
        expect(bodies().isEmpty, "nothing is sent for an invalid destination set")
        expect(!model.streamingDesired, "no streaming intent")
        expect(model.warnings.contains { $0.contains("Configure SRT host before streaming.") },
               "the operator reads Windows' sentence")
        expectEqual(model.streamStartRefusal, "Configure SRT host before streaming.", "the refusal is exposed for the UI")
    }

    @MainActor
    static func testRetrySendsRecoverOutputSender() {
        let (model, bodies, _) = streamingModel()
        model.retryDestination("srt")
        spin(until: { !bodies().isEmpty }, limit: 3)
        let sent = commands(bodies().first)
        expectEqual(sent.count, 1, "exactly one command")
        expectEqual(sent.first?["type"] as? String, "recover-output-sender", "the operator reset")
        expectEqual(sent.first?["destination"] as? String, "srt", "for that destination")
        expectEqual(sent.first?["reason"] as? String, "operator-retry", "with a reason")
        model.retryDestination("ndi")
        spin(until: { false }, limit: 0.2)
        expectEqual(bodies().count, 1, "Retry is only for the three stream destinations")
    }

    @MainActor
    static func testSnapshotProjectsRowsAndQuitStatus() {
        let (model, _, _) = streamingModel()
        let healthy: JSONObject = ["healthy": true, "gaveUp": false, "consecutiveFailures": 0,
                                   "nextAttemptInMs": 0, "lastProgressAgeMs": 200, "reason": ""]
        let gaveUp: JSONObject = ["healthy": false, "gaveUp": true, "consecutiveFailures": 5,
                                  "nextAttemptInMs": 0, "lastProgressAgeMs": -1, "reason": "gave up after 5 failures"]
        func lifecycle(_ state: String) -> JSONObject {
            ["sessionId": "s", "desiredActive": true, "state": state, "health": "healthy", "finalized": false]
        }
        model.applySnapshotForTesting(["outputSenderSession": [
            "senders": [
                ["destination": "rtmp", "status": "live", "lifecycle": lifecycle("producing"),
                 "supervisor": healthy, "runtimeDetail": "ffmpeg:/Applications/CoreVideo Pro.app/Contents/Resources/ffmpeg"],
                ["destination": "srt", "status": "failed", "lastResultCode": "supervisor-gave-up",
                 "warning": "gave up after 5 failures", "lifecycle": lifecycle("failed"), "supervisor": gaveUp],
            ] as [JSONObject]] as JSONObject])
        expectEqual(model.destinationRows["rtmp"]?.label, "Live", "rtmp row")
        expectEqual(model.destinationRows["srt"]?.label, "Stopped retrying", "srt row")
        expect(model.destinationRows["srt"]?.showsRetry == true, "srt offers Retry")
        expectEqual(model.destinationRows["hls"]?.label, "Off", "absent and not desired")
        expectEqual(model.streamStatus, "live", "the quit guard reads the first non-terminal sender (rtmp)")
        expectEqual(model.streamRuntimeDetail, "ffmpeg:/Applications/CoreVideo Pro.app/Contents/Resources/ffmpeg",
                    "the resolved ffmpeg reaches the UI")
        // An SRT-only live stream still guards a quit.
        model.applySnapshotForTesting(["outputSenderSession": [
            "senders": [
                ["destination": "rtmp", "status": "stopped", "lifecycle": lifecycle("completed"), "supervisor": healthy],
                ["destination": "srt", "status": "live", "lifecycle": lifecycle("producing"), "supervisor": healthy],
            ] as [JSONObject]] as JSONObject])
        expectEqual(model.streamStatus, "live", "a completed RTMP does not hide a live SRT")
        expect(QuitGuardPolicy.streamLive(QuitOutputs(streamStatus: model.streamStatus)), "quit guard sees it")
    }

    @MainActor
    static func testPrefsKeepRowsNeverThePassphrase() {
        let model = AppModel()
        model.streamDestinations.srtEnabled = true
        model.streamDestinations.srtHost = "venue.example.net"
        model.streamDestinations.srtPort = "7001"
        model.streamDestinations.srtStreamId = "program"
        model.streamDestinations.hlsEnabled = true
        model.streamDestinations.hlsPlaylistUrl = "https://origin.example/live/program.m3u8"
        model.srtPassphrase = "never-in-prefs-123"
        guard let data = try? JSONEncoder().encode(model.currentPrefs()) else {
            expect(false, "prefs encode"); return
        }
        let text = String(decoding: data, as: UTF8.self)
        expect(!text.contains("never-in-prefs-123"), "the SRT passphrase never reaches the prefs file")
        guard let after = relaunched(model) else { expect(false, "relaunch"); return }
        expectEqual(after.streamDestinations.srtHost, "venue.example.net", "srt host survives a relaunch")
        expectEqual(after.streamDestinations.srtPort, "7001", "srt port survives")
        expectEqual(after.streamDestinations.srtStreamId, "program", "srt stream id survives")
        expect(after.streamDestinations.hlsEnabled, "hls toggle survives")
        expectEqual(after.streamDestinations.hlsPlaylistUrl, "https://origin.example/live/program.m3u8", "hls url survives")
        // An older prefs file without the key still loads (additive-safe decode).
        let old = Data(#"{"streamUrl":"rtmp://example/live"}"#.utf8)
        let decoded = try? JSONDecoder().decode(ShellPrefs.self, from: old)
        expect(decoded != nil && decoded?.streamDestinations == nil, "an older prefs file decodes")
    }

    /// The footer Stream stat used to print the published status verbatim.
    @MainActor
    static func testFooterHealthReadsTheProjection() {
        let (model, _, _) = streamingModel()
        model.streamKey = "rtmp-key-1"
        model.streamDestinations.srtEnabled = true
        model.streamDestinations.srtHost = "receiver.example.com"
        model.streamingDesired = true
        model.streamStatus = "live"                        // the adapter's launch word
        model.destinationRows = ["rtmp": DestinationStatus(label: "Connecting", tone: .neutral),
                                 "srt": DestinationStatus(label: "Connecting", tone: .neutral)]
        expectEqual(model.streamHealthLabel, "Connecting", "a launch reads Connecting, whatever status says")
        model.destinationRows = ["rtmp": DestinationStatus(label: "Live", tone: .good, isLive: true),
                                 "srt": DestinationStatus(label: "Reconnecting in 5 s", tone: .warn)]
        expectEqual(model.streamHealthLabel, "Live 1/2", "one of two confirmed")
        expectEqual(model.streamSummary, "RTMP live, SRT reconnecting in 5 s", "the footer summary line")
        model.destinationRows["srt"] = DestinationStatus(label: "Live", tone: .good, isLive: true)
        expectEqual(model.streamHealthLabel, "Live", "all confirmed")
    }

    /// Review blocker: a pkg install runs the core from /Library/Application
    /// Support/CoreVideo Pro/, where nothing sits beside it. The shell must
    /// name the bundle's guard and ffmpeg for it, and never override an
    /// operator's own COREVIDEO_FFMPEG_BIN_DIR.
    static func testLaunchEnvNamesTheBundledGuardAndFfmpeg() {
        var paths = ShellPaths.Paths()
        paths.childGuardPath = "/Applications/CoreVideo Pro.app/Contents/Resources/corevideo-child-guard"
        paths.bundledFfmpegDirectory = "/Applications/CoreVideo Pro.app/Contents/Resources"
        let env = ShellPaths.streamingEnvironment(paths, inherited: [:])
        expectEqual(env["COREVIDEO_CHILD_GUARD_PATH"],
                    "/Applications/CoreVideo Pro.app/Contents/Resources/corevideo-child-guard", "guard path")
        expectEqual(env["COREVIDEO_FFMPEG_BIN_DIR"], "/Applications/CoreVideo Pro.app/Contents/Resources",
                    "bundled ffmpeg directory")
        let kept = ShellPaths.streamingEnvironment(paths, inherited: ["COREVIDEO_FFMPEG_BIN_DIR": "/opt/my-ffmpeg"])
        expect(kept["COREVIDEO_FFMPEG_BIN_DIR"] == nil, "an operator's own ffmpeg dir is left alone (not overridden)")
        expectEqual(kept["COREVIDEO_CHILD_GUARD_PATH"],
                    "/Applications/CoreVideo Pro.app/Contents/Resources/corevideo-child-guard",
                    "the guard is still named")
        // Through the REAL launch builder (CoreLaunchEnvironment.extras), not only the helper.
        let launch = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil)
        expectEqual(launch["COREVIDEO_CHILD_GUARD_PATH"],
                    "/Applications/CoreVideo Pro.app/Contents/Resources/corevideo-child-guard",
                    "the core launch names the bundle's guard")
        expectEqual(launch["COREVIDEO_FFMPEG_BIN_DIR"], "/Applications/CoreVideo Pro.app/Contents/Resources",
                    "the core launch names the bundle's ffmpeg directory")
        let launchKept = CoreLaunchEnvironment.extras(
            paths: paths, processEnvironment: ["COREVIDEO_FFMPEG_BIN_DIR": "/opt/my-ffmpeg"], zoomPublicAppKey: nil)
        expect(launchKept["COREVIDEO_FFMPEG_BIN_DIR"] == nil,
               "the launch leaves an operator's inherited COREVIDEO_FFMPEG_BIN_DIR to the child environment")
        let none = ShellPaths.streamingEnvironment(ShellPaths.Paths(), inherited: [:])
        expect(none.isEmpty, "nothing found, nothing invented")
        // The resolver finds the bundle's copies from the app's Resources.
        let found = ShellPaths.streamingHelpers(resources: "/X/Resources",
                                                exists: { ["/X/Resources/corevideo-child-guard",
                                                           "/X/Resources/ffmpeg"].contains($0) })
        expectEqual(found.childGuardPath, "/X/Resources/corevideo-child-guard", "guard found in Resources")
        expectEqual(found.bundledFfmpegDirectory, "/X/Resources", "ffmpeg dir found in Resources")
        let missing = ShellPaths.streamingHelpers(resources: "/X/Resources", exists: { _ in false })
        expect(missing.childGuardPath == nil && missing.bundledFfmpegDirectory == nil, "absent stays absent")
    }

    /// A helper that runs without the guard can outlive a killed core: say so.
    @MainActor
    static func testAnUnguardedHelperIsLoud() {
        let (model, _, _) = streamingModel()
        func lifecycle(_ state: String) -> JSONObject {
            ["sessionId": "s", "desiredActive": true, "state": state, "health": "healthy", "finalized": false]
        }
        model.applySnapshotForTesting(["outputSenderSession": ["senders": [
            ["destination": "rtmp", "status": "live", "lifecycle": lifecycle("producing"), "childGuard": true],
        ] as [JSONObject]] as JSONObject])
        expectEqual(model.streamChildGuard, true, "guarded is reported")
        expect(!model.warnings.contains { $0.contains("child guard") }, "no warning while guarded")
        model.applySnapshotForTesting(["outputSenderSession": ["senders": [
            ["destination": "rtmp", "status": "live", "lifecycle": lifecycle("producing"), "childGuard": false],
        ] as [JSONObject]] as JSONObject])
        expectEqual(model.streamChildGuard, false, "unguarded is reported")
        expectEqual(model.warnings.filter { $0.contains("child guard") }.count, 1, "warned once")
        model.applySnapshotForTesting(["outputSenderSession": ["senders": [
            ["destination": "rtmp", "status": "live", "lifecycle": lifecycle("producing"), "childGuard": false],
        ] as [JSONObject]] as JSONObject])
        expectEqual(model.warnings.filter { $0.contains("child guard") }.count, 1, "and only once per core")
        expect(StreamHealthLine.childGuard(false).contains("can outlive"), "Health says what it means")
        expectEqual(StreamHealthLine.childGuard(nil), "not reported yet", "absent is not claimed")
    }

    /// Review finding 4: an SRT edit mid-stream must not reach the core. A
    /// recording toggle resends the FULL destination set (start-program-output),
    /// which used to carry the half-typed host and restart the SRT sender.
    @MainActor
    static func testDestinationsAreFrozenWhileStreaming() {
        let (model, bodies, _) = streamingModel()
        model.streamKey = "rtmp-key-1"
        model.streamDestinations.srtEnabled = true
        model.streamDestinations.srtHost = "receiver.example.com"
        model.srtPassphrase = ""
        expect(!model.streamDestinationsLocked, "the editor is open before Start")
        model.toggleStreaming()
        spin(until: { !bodies().isEmpty }, limit: 3)
        expect(model.streamDestinationsLocked, "the editor locks once streaming")
        model.streamDestinations.srtHost = "half-typ"
        model.streamDestinations.hlsEnabled = true
        model.streamDestinations.hlsPlaylistUrl = "https://origin.example.com/live/playlist.m3u8"
        let resent = model.startProgramOutput(recording: true, streaming: true)
        expectEqual(resent["destinations"] as? [String], ["recording", "rtmp", "srt"],
                    "the streamed set is the one frozen at Start (no HLS added mid-stream)")
        let srt = (resent["destinationSettings"] as? [JSONObject])?.first { $0["id"] as? String == "srt" }
        expectEqual(srt?["host"] as? String, "receiver.example.com", "the SRT host is the Start-time host")
        expectEqual(model.streamSummary.components(separatedBy: ", ").count, 2,
                    "the footer summarises what is streaming, not the editor")
        model.toggleStreaming()   // stop
        expect(!model.streamDestinationsLocked, "stopping unlocks the editor")
        let next = model.startProgramOutput(recording: false, streaming: true)
        let srtNext = (next["destinationSettings"] as? [JSONObject])?.first { $0["id"] as? String == "srt" }
        expectEqual(srtNext?["host"] as? String, "half-typ", "the edit applies to the NEXT stream")
    }
}
