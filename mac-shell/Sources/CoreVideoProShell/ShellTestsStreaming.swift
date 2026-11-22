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
}
