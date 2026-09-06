// Shell test suite — run with COREVIDEO_SHELL_TESTS=1.
//
// Why it lives in the app binary rather than an XCTest target: this machine has
// Command Line Tools only, no Xcode, so `swift test` cannot find XCTest. A suite
// that runs in CI but not on the owner's machine is worse than none — the bugs
// get found on the rig. This mirrors the C++ side, where the suite is a plain
// executable that prints "511 tests passed", and extends the existing
// COREVIDEO_SHELL_SELFCHECK pattern. It also gets internal access for free, so
// nothing has to be made public just to be tested.
//
// The suite is deliberately biased toward code that HAS BROKEN in production
// rather than toward coverage percentage. Every case below maps to a real defect
// that reached the owner in a live session or shipped to main.

import Foundation

enum ShellTests {
    // ── tiny harness ─────────────────────────────────────────────────────────
    private static var failures: [String] = []
    private static var checks = 0

    private static func expect(_ condition: Bool, _ what: String,
                               _ file: StaticString = #file, _ line: UInt = #line) {
        checks += 1
        if !condition {
            failures.append("\(what)  (\(URL(fileURLWithPath: "\(file)").lastPathComponent):\(line))")
        }
    }

    private static func expectEqual<T: Equatable>(_ actual: T, _ expected: T, _ what: String,
                                                  _ file: StaticString = #file,
                                                  _ line: UInt = #line) {
        checks += 1
        if actual != expected {
            failures.append("\(what): expected \(expected), got \(actual)  "
                            + "(\(URL(fileURLWithPath: "\(file)").lastPathComponent):\(line))")
        }
    }

    // ── prefs: the data-loss class ───────────────────────────────────────────

    private static func testRecordingCommandRetriesAndSupersession() {
        var policy = RecordingCommandPolicy()
        policy.observe("recording")
        let stop = policy.begin()
        expect(stop.stop, "observed live recording selects Stop")
        expect(!policy.desired, "pending Stop expresses stopped intent")
        policy.observe("recording")
        expect(!policy.desired, "live polls do not undo pending Stop intent")
        expect(policy.finish(stop, failed: true), "failed Stop owns its completion")
        expect(policy.desired, "failed Stop restores still-live recording intent")
        let retry = policy.begin()
        expect(retry.stop, "retry of failed Stop sends Stop, matching Stop Rec label")
        policy.observe("finalizing")
        policy.finish(retry, failed: true)
        expect(!policy.desired, "lost Stop reply after observed finalization stays stopped")

        policy.observe("completed")
        let firstStart = policy.begin()
        expect(!firstStart.stop, "completed recording selects Start")
        let cancelStart = policy.begin()
        expect(cancelStart.stop, "pending Start can be cancelled")
        let newestStart = policy.begin()
        expect(!newestStart.stop, "new Start after cancellation expresses fresh intent")
        expect(!policy.finish(firstStart, failed: true), "old Start failure is ignored")
        expect(!policy.finish(cancelStart, failed: false), "old Stop success is ignored")
        expect(policy.desired, "old completions cannot clear newest Start intent")
        policy.interrupted()
        expect(!policy.finish(newestStart, failed: true), "process exit invalidates pending completion")
        expect(!policy.desired, "process exit clears recording intent")

        policy.observe("idle")
        let failedStart = policy.begin()
        policy.finish(failedStart, failed: true)
        expect(!policy.desired, "failed Start while idle permits retrying Start")
        let acceptedStart = policy.begin()
        policy.observe("recording")
        policy.finish(acceptedStart, failed: true)
        expect(policy.desired, "lost Start reply preserves observed recording")

        policy.observe("completed")
        let acknowledgedStart = policy.begin()
        policy.finish(acknowledgedStart, failed: false)
        policy.observe("idle")
        expect(policy.desired, "old idle poll cannot undo acknowledged Start")
        policy.observe("completed")
        expect(policy.desired, "old completed poll cannot undo acknowledged Start")
        policy.observe("starting")
        expect(policy.desired, "fresh starting progress keeps Start intent")
        let stopWhileStarting = policy.begin()
        policy.finish(stopWhileStarting, failed: true)
        expect(!policy.desired, "failed Stop while starting does not re-arm unproven media")
        policy.observe("starting")
        expect(!policy.desired, "starting polls cannot re-arm stopped intent")
        policy.observe("recording")
        expect(policy.begin().stop, "observed media still permits explicit Stop retry")
    }

    private static func testBridgeGenerationRejectsStaleWork() {
        var policy = BridgeGenerationPolicy()
        expect(!policy.canWrite(policy.generation), "stopped bridge rejects writes")
        let first = policy.begin()
        expect(policy.isCurrent(first), "launched child owns current generation")
        expect(!policy.canWrite(first), "unvalidated handshake cannot receive commands")
        expect(policy.acceptHandshake(first), "current handshake is accepted")
        expect(policy.canWrite(first), "validated current child accepts commands")

        let recovery = policy.invalidate(stopped: false)
        expect(!policy.isCurrent(first), "old stdout and exit callbacks are stale after exit")
        expect(!policy.canWrite(first), "queued old writes cannot cross process exit")
        expect(policy.canRelaunch(recovery), "current recovery token can relaunch")
        let second = policy.begin()
        expect(!policy.canRelaunch(recovery), "new child invalidates old relaunch timer")
        expect(!policy.acceptHandshake(first), "late old handshake cannot mark new child ready")
        expect(!policy.canWrite(second), "new child still requires its own handshake")
        expect(policy.acceptHandshake(second), "replacement handshake is independent")
        expect(!policy.canWrite(first), "old queued writes cannot enter ready replacement")
        expect(policy.canWrite(second), "fresh commands can use ready replacement")

        let pendingRecovery = policy.invalidate(stopped: false)
        let stopped = policy.invalidate(stopped: true)
        expect(!policy.canRelaunch(pendingRecovery), "stop invalidates scheduled relaunch")
        expect(!policy.canRelaunch(stopped), "stop never schedules a new process")
        expect(!policy.acceptHandshake(second), "late handshake cannot revive stopped bridge")

        let incompatible = policy.begin()
        _ = policy.invalidate(stopped: true)
        expect(policy.stopped, "rejected handshake leaves bridge stopped")
        expect(!policy.canWrite(incompatible), "rejected handshake blocks captured handles immediately")
        expect(!policy.canRelaunch(policy.generation), "incompatible protocol does not auto-restart")
    }

    private static func testSharedLifecycleContracts() {
        expectEqual(RecordingLifecycleReadModel.status(["status": "recording", "lifecycle": NSNull()]),
                    "unknown", "malformed lifecycle never falls back to legacy live status")
        for health in ["healthy", "degraded", "unknown", "failed"] {
            let lifecycle: [String: Any] = ["sessionId": "test", "desiredActive": true,
                "state": "live", "health": health, "finalized": false]
            let expected = health == "healthy" || health == "degraded" ? "recording" : health
            expectEqual(RecordingLifecycleReadModel.status(["lifecycle": lifecycle]), expected,
                        "live state requires observed healthy or degraded media")
        }
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        do {
            let data = try Data(contentsOf: root.appendingPathComponent("contracts/lifecycle.fixtures.json"))
            guard let fixtures = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
                expect(false, "lifecycle fixtures must be an array"); return
            }
            expect(!fixtures.isEmpty, "lifecycle fixtures are not empty")
            for fixture in fixtures {
                guard let id = fixture["id"] as? String, let contract = fixture["contract"] as? String,
                      let accepted = fixture["accepted"] as? Bool, let json = fixture["json"] as? String,
                      let payloadData = json.data(using: .utf8) else {
                    expect(false, "malformed lifecycle fixture"); continue
                }
                let value = try JSONSerialization.jsonObject(with: payloadData, options: [.fragmentsAllowed])
                let validate: ([String: Any]) -> Bool
                switch contract {
                case "ProtocolVersion": validate = validateProtocolVersion
                case "OutputLifecycle": validate = validateOutputLifecycle
                case "OperationStatus": validate = validateOperationStatus
                case "ProtocolFailure": validate = validateProtocolFailure
                default: expect(false, "unknown contract \(contract)"); continue
                }
                expectEqual((value as? [String: Any]).map(validate) ?? false, accepted, id)
                if accepted {
                    let encoded: Data
                    switch contract {
                    case "ProtocolVersion": encoded = try JSONEncoder().encode(JSONDecoder().decode(ProtocolVersion.self, from: payloadData))
                    case "OutputLifecycle": encoded = try JSONEncoder().encode(JSONDecoder().decode(OutputLifecycle.self, from: payloadData))
                    case "OperationStatus": encoded = try JSONEncoder().encode(JSONDecoder().decode(OperationStatus.self, from: payloadData))
                    default: encoded = try JSONEncoder().encode(JSONDecoder().decode(ProtocolFailure.self, from: payloadData))
                    }
                    let roundTrip = try JSONSerialization.jsonObject(with: encoded) as? [String: Any]
                    expect(roundTrip.map(validate) ?? false, "\(id) round trip")
                }
            }
        } catch { expect(false, "lifecycle fixtures: \(error)") }
    }

    /// Shipping `colorGrade` as a non-optional field silently reset EVERY saved
    /// setting, because synthesized Decodable ignores property defaults and
    /// load() swallows the throw with `try?`.
    private static func testPrefsSurviveAnOlderFile() {
        let older = """
        {"version":1,"displayName":"operator","streamUrl":"rtmp://custom/live",
         "joinMeetingId":"8675309","webinar":true}
        """.data(using: .utf8)!
        guard let prefs = try? JSONDecoder().decode(ShellPrefs.self, from: older) else {
            expect(false, "a prefs file written before newer fields existed must still decode")
            return
        }
        expectEqual(prefs.displayName, "operator", "existing value survives")
        expectEqual(prefs.streamUrl, "rtmp://custom/live", "existing value survives")
        expectEqual(prefs.joinMeetingId, "8675309", "existing value survives")
        expectEqual(prefs.webinar, true, "existing value survives")
        expectEqual(prefs.colorGrade, [], "a field absent from the file takes its default")
        expectEqual(prefs.autoHoldSeconds, 4.0, "an untouched default stays correct")
    }

    /// Guards the OTHER half: a field added to the struct but forgotten in the
    /// hand-written decoder encodes fine and never restores. Caught a real one
    /// during the VST merge (`vstChannelSelections`).
    private static func testPrefsRoundTripKeepsEveryField() {
        let roundTrip = ShellPrefs.roundTripSelfCheck()
        let detail = roundTrip ?? "ok"
        expect(roundTrip == nil, "every ShellPrefs field must survive encode->decode: \(detail)")
    }

    /// A corrupt or truncated file must fall back to defaults, not crash.
    private static func testPrefsToleratesGarbage() {
        let garbage = "{not json at all".data(using: .utf8)!
        expect((try? JSONDecoder().decode(ShellPrefs.self, from: garbage)) == nil,
               "garbage must fail to decode rather than produce nonsense")
    }

    // ── roster: the empty-meeting class ──────────────────────────────────────

    /// The shell read id/name/hasVideo; both engine paths emit
    /// userId/displayName/videoOn. The roster was empty in a live meeting.
    private static func testRosterParsesTheEngineWireShape() {
        let wire: [JSONObject] = [[
            "userId": 101, "displayName": "Producer", "videoOn": true,
            "muted": false, "talking": true, "sharingScreen": true,
        ]]
        let roster = RosterParticipant.parse(wire, assignedIds: ["101"])
        expectEqual(roster.count, 1, "engine-shaped participant must parse")
        guard let p = roster.first else { return }
        expectEqual(p.id, "101", "numeric userId becomes a string id")
        expectEqual(p.name, "Producer", "displayName is the name")
        expectEqual(p.hasVideo, true, "videoOn maps to hasVideo")
        expectEqual(p.talking, true, "talking maps through")
        expectEqual(p.sharingScreen, true, "sharingScreen maps through")
        expectEqual(p.assigned, true, "assignedIds marks the slot as taken")
    }

    /// The legacy spelling must keep working — the mock/stub core emits it.
    private static func testRosterParsesTheLegacyShape() {
        let wire: [JSONObject] = [[
            "id": "guest-1", "name": "Guest", "hasVideo": true,
            "isMuted": true, "isTalking": false,
        ]]
        let roster = RosterParticipant.parse(wire)
        expectEqual(roster.count, 1, "legacy-shaped participant must parse")
        expectEqual(roster.first?.name ?? "", "Guest", "legacy name key")
        expectEqual(roster.first?.hasVideo ?? false, true, "legacy hasVideo key")
        expectEqual(roster.first?.muted ?? false, true, "legacy isMuted key")
    }

    /// A participant with no usable id is dropped, not turned into a ghost tile.
    private static func testRosterDropsEntriesWithoutAnId() {
        let roster = RosterParticipant.parse([["displayName": "No Id"], ["userId": 7]])
        expectEqual(roster.count, 1, "an entry with no id must be dropped")
        expectEqual(roster.first?.id ?? "", "7", "the valid entry survives")
    }

    /// A missing name must fall back to the id, never to an empty label — a
    /// blank tile in the multiview is indistinguishable from a broken one.
    private static func testRosterNameFallsBackToId() {
        let roster = RosterParticipant.parse([["userId": 42, "videoOn": true]])
        expectEqual(roster.first?.name ?? "", "42", "name falls back to the id")
    }

    // ── redaction: the secret-leak class ─────────────────────────────────────

    /// A support bundle gets emailed to strangers. Secrets must not survive it,
    /// and diagnostics must: over-redaction makes the bundle useless.
    private static func testRedactionRemovesSecretsAndKeepsDiagnostics() {
        let sample = """
        streamKey=NOTAREALKEY0000aaaaBBBBccccDDDDeeeeFFFF
        userZak=ZAK_aBcDeF1234567890abcdefGHIJ
        Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxIn0.sig
        keyframeIntervalSeconds=2 renderPlanSignature=991817
        artifact /Users/operator/Movies/CoreVideoPro/Program.mp4
        """
        let cleaned = Redactor.redactLines(sample)
        expect(!cleaned.contains("NOTAREALKEY0000aaaaBBBBccccDDDDeeeeFFFF"), "stream key must not survive")
        expect(!cleaned.contains("ZAK_aBcDeF1234567890abcdefGHIJ"), "userZak must not survive")
        expect(!cleaned.contains("eyJhbGciOiJIUzI1NiIs"), "a JWT must not survive")
        expect(cleaned.contains("keyframeIntervalSeconds=2"),
               "a benign 'key'-containing field must NOT be redacted")
        expect(cleaned.contains("renderPlanSignature=991817"),
               "a signature is diagnostics, not a secret")
        expect(cleaned.contains("Program.mp4"), "file paths must stay readable")
    }

    // ── colour grade: the dead-travel class ──────────────────────────────────

    /// The UI range must match the core's clamp, or the operator drags into a
    /// region where nothing happens.
    @MainActor
    private static func testColorGradeNeutralDetection() {
        let model = AppModel()
        expect(model.gradeIsNeutral, "a fresh model is neutral")
        model.gradeSaturation = -10
        expect(!model.gradeIsNeutral, "a non-zero axis is not neutral")
        model.resetColorGrade()
        expect(model.gradeIsNeutral, "reset returns to neutral")
    }


    // ── wire contract: the silently-discarded-command class ──────────────────

    /// JsonRpcServer discards a payload-wrapped command that arrives without the
    /// wrapper, answering "<type> requires a payload." connect-capture-device
    /// shipped exactly that way: the button did nothing and the UI still showed
    /// the device connected.
    private static func testPayloadWrappedCommandsCarryTheWrapper() {
        let connect = CoreCommands.connectCaptureDevice(deviceId: "cam-1",
                                                        outputSourceId: "cam-1")
        expectEqual(connect["type"] as? String ?? "", "connect-capture-device", "type")
        guard let payload = connect["payload"] as? JSONObject else {
            expect(false, "connect-capture-device MUST carry a payload wrapper")
            return
        }
        expectEqual(payload["deviceId"] as? String ?? "", "cam-1", "deviceId in payload")
        // Without outputSourceId the core keys frames by its own id and the
        // multiview lookup misses — the "pink tiles" failure.
        expectEqual(payload["outputSourceId"] as? String ?? "", "cam-1",
                    "outputSourceId must be forwarded")

        let disconnect = CoreCommands.disconnectCaptureDevice(deviceId: "cam-1")
        expect(disconnect["payload"] is JSONObject,
               "disconnect-capture-device MUST carry a payload wrapper")
    }

    /// The shell's idea of which commands need a wrapper is re-derived from the
    /// CORE's source, so a guard added in C++ cannot drift away from Swift.
    private static func testPayloadContractMatchesTheCoreSource() {
        guard let fromCore = CoreCommands.payloadWrappedTypesFromCoreSource() else {
            FileHandle.standardError.write(
                "  SKIPPED payload-contract cross-check (core source not alongside binary)\n"
                    .data(using: .utf8)!)
            return
        }
        let missing = fromCore.subtracting(CoreCommands.payloadWrappedTypes)
        let extra = CoreCommands.payloadWrappedTypes.subtracting(fromCore)
        expect(missing.isEmpty,
               "the core requires a payload for \(missing.sorted()) but the shell does not list it")
        expect(extra.isEmpty,
               "the shell lists \(extra.sorted()) as payload-wrapped but the core does not")
    }

    /// Root-level commands must NOT be wrapped — a payload here is just as wrong.
    private static func testRootLevelCommandsAreNotWrapped() {
        let grade = CoreCommands.setColorGrade(exposure: 1, contrast: -2,
                                               saturation: 3, temperature: -4)
        expect(grade["payload"] == nil, "set-color-grade must NOT be payload-wrapped")
        expectEqual(grade["saturation"] as? Double ?? 0, 3, "axis at the command root")
        let sync = CoreCommands.mediaCoreSync(elapsedMs: 12, commands: [grade])
        expectEqual((sync["commands"] as? [JSONObject])?.count ?? 0, 1, "sync carries commands")
    }

    /// A ZAK is only sent when OAuth produced one; an empty string must never be
    /// put on the wire in its place.
    private static func testJoinOmitsAnAbsentZak() {
        let guest = CoreCommands.zoomJoin(meetingId: "123", displayName: "Op",
                                          passcode: "", webinar: false)
        let guestPayload = guest["payload"] as? JSONObject ?? [:]
        expect(guestPayload["userZak"] == nil, "no ZAK key when OAuth returned none")
        let host = CoreCommands.zoomJoin(meetingId: "123", displayName: "Op",
                                         passcode: "", webinar: false, userZak: "ZAK")
        let hostPayload = host["payload"] as? JSONObject ?? [:]
        expectEqual(hostPayload["userZak"] as? String ?? "", "ZAK", "ZAK forwarded when present")
        expectEqual(hostPayload["meetingNumber"] as? String ?? "", "123",
                    "both meeting spellings are sent")
    }

    // ── transport framing: the everything-times-out class ────────────────────

    /// The core emits large snapshots, so a JSON object IS routinely split
    /// across stdout reads. Dropping a partial tail loses responses at random
    /// and every command looks like a timeout.
    private static func testBridgeReassemblesSplitLines() {
        var buffer: [UInt8] = []
        let whole = "{\"id\":\"a\",\"ok\":true}\n"
        let half = whole.index(whole.startIndex, offsetBy: 9)

        buffer.append(contentsOf: Array(whole[whole.startIndex..<half].utf8))
        let firstPass = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(firstPass.count, 0, "a partial line yields nothing yet")
        expect(!buffer.isEmpty, "the partial tail is retained for the next read")

        buffer.append(contentsOf: Array(whole[half...].utf8))
        let secondPass = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(secondPass.count, 1, "the completed line is delivered")
        expectEqual(secondPass.first?["id"] as? String ?? "", "a", "and parses correctly")
        expectEqual(buffer.count, 0, "buffer drains once consumed")
    }

    /// Several objects can arrive in one read; all must be delivered in order.
    private static func testBridgeDeliversMultipleLinesFromOneRead() {
        var buffer = Array("{\"id\":\"1\"}\n{\"id\":\"2\"}\n{\"id\":\"3\"}\n".utf8)
        let objects = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(objects.count, 3, "all three objects delivered")
        expectEqual(objects.map { $0["id"] as? String ?? "" }, ["1", "2", "3"], "in order")
    }

    /// One malformed event must not poison the stream — the session has to
    /// survive a bad line, not go silent.
    private static func testBridgeSkipsMalformedLinesWithoutStalling() {
        var buffer = Array("{oops\n{\"id\":\"good\"}\n".utf8)
        let objects = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(objects.count, 1, "the malformed line is skipped")
        expectEqual(objects.first?["id"] as? String ?? "", "good", "the good line still arrives")
    }


    // ── coalescing: the RPC-flood class ──────────────────────────────────────

    /// A drag emits a change per frame. Sending one request per delta held the
    /// core lock ~100% of wall time and every shell command timed out — joins,
    /// scene syncs, assigns. Debouncing is not a nicety here, so assert it
    /// rather than trusting that the pattern was copied correctly (it has now
    /// been written three times: scenes, colour grade, lower thirds).
    @MainActor
    private static func testColorGradePushesAreCoalesced() {
        let model = AppModel()
        let before = model.colorGradePushCount
        for step in 0..<20 {                      // a fast drag
            model.gradeSaturation = Double(step) * 0.1
            model.applyColorGrade()
        }
        expectEqual(model.colorGradePushCount, before,
                    "nothing is sent while the gesture is still moving")
        // Spin the main run loop past the debounce window so the surviving task
        // can fire (Task.sleep needs the loop to turn).
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        expectEqual(model.colorGradePushCount, before + 1,
                    "20 rapid changes collapse into exactly ONE push")
    }


    // ── OAuth: the signs-you-out-of-OBS class ────────────────────────────────

    /// Zoom ROTATES refresh tokens on use. A refresh response that omits
    /// `refresh_token` means "no rotation, keep what you have" — writing an
    /// empty string over the stored one leaves nothing to present on the next
    /// refresh and signs the account out. The owner's OBS plugin authenticates
    /// the SAME Zoom account, so a bad write here logs them out of a tool they
    /// use in production.
    private static func testRefreshWithoutRotationKeepsTheStoredToken() {
        let merged = ZoomOAuth.mergeTokenResponse(
            ["access_token": "new-access", "expires_in": NSNumber(value: 3600)],
            existingRefreshToken: "STORED-REFRESH", now: 1_000_000)
        expectEqual(merged.accessToken, "new-access", "the new access token is taken")
        expectEqual(merged.refreshToken, "STORED-REFRESH",
                    "a response with no rotation must PRESERVE the stored refresh token")
    }

    /// When Zoom DOES rotate, the new token must replace the old one — keeping
    /// a stale refresh token is the same failure from the other direction.
    private static func testRefreshWithRotationTakesTheNewToken() {
        let merged = ZoomOAuth.mergeTokenResponse(
            ["access_token": "a", "refresh_token": "ROTATED", "expires_in": NSNumber(value: 3600)],
            existingRefreshToken: "STORED-REFRESH", now: 1_000_000)
        expectEqual(merged.refreshToken, "ROTATED", "a rotated token replaces the stored one")
    }

    /// An empty response must not fabricate a token, and must still not destroy
    /// the stored one.
    private static func testEmptyResponseDoesNotDestroyTheStoredToken() {
        let merged = ZoomOAuth.mergeTokenResponse([:], existingRefreshToken: "STORED",
                                                  now: 1_000_000)
        expectEqual(merged.refreshToken, "STORED", "an empty response preserves the refresh token")
        expectEqual(merged.accessToken, "", "but does not invent an access token")
    }

    /// Expiry carries 60s of slack: a token that expires mid-join is a failed
    /// show, and the default must be conservative when the broker omits it.
    private static func testExpiryLeavesSlackBeforeTheRealDeadline() {
        let merged = ZoomOAuth.mergeTokenResponse(
            ["access_token": "a", "expires_in": NSNumber(value: 3600)],
            existingRefreshToken: "r", now: 1_000_000)
        expectEqual(merged.expiresAt, 1_000_000 + 3600 - 60, "expiry is shortened by 60s")
        let noExpiry = ZoomOAuth.mergeTokenResponse(["access_token": "a"],
                                                    existingRefreshToken: "r", now: 1_000_000)
        expectEqual(noExpiry.expiresAt, 1_000_000 + 3600 - 60,
                    "a missing expires_in falls back to one hour, still with slack")
    }

    /// HARD RULE: this app owns its own Keychain item. Reading or writing the
    /// OBS plugin's item makes a second reader of a rotating token and signs the
    /// plugin out.
    private static func testKeychainServiceIsNeverTheObsPluginItem() {
        expectEqual(ZoomOAuth.service, "us.iamfatness.corevideopro.zoom-oauth",
                    "this app uses its OWN Keychain service")
        expect(!ZoomOAuth.service.contains("OBS"),
               "the OBS plugin's Keychain item must never be touched")
    }

    /// The broker pins the app return URI; changing the scheme 400s every sign-in.
    private static func testOAuthReturnUriMatchesTheBrokerAllowlist() {
        expectEqual(ZoomOAuth.appReturnUri, "corevideo://oauth/callback",
                    "the broker allowlists exactly this return uri")
    }


    // ── encoder policy: the legal-exposure class ─────────────────────────────

    /// The shell must never offer a codec this build cannot legally or
    /// technically encode. HEVC sits under multiple patent pools that charge
    /// encoder royalties, and the GPL software encoders (x264/x265) cannot ship
    /// in a proprietary product at all — so the product ships HARDWARE encoders
    /// only (NVENC on Windows, VideoToolbox on Apple), where licensing rides
    /// with the OS and silicon vendor. macOS therefore encodes H.264 and nothing
    /// else (native/src/modules/EncoderPolicy.h: "HEVC encode is not shipped on
    /// any platform"; AV1 is the non-Apple branch).
    ///
    /// A picker offering HEVC is not merely a dead control here — it invites the
    /// operator to select something the product deliberately does not license.
    @MainActor
    private static func testShellOffersOnlyEncodableCodecs() {
        let offered = Set(AppModel.supportedStreamCodecs.map(\.id))
        expect(!offered.contains("h265"),
               "HEVC must not be offered — it is royalty-encumbered and not shipped")
        expect(!offered.contains("av1"),
               "AV1 is the non-Apple branch of EncoderPolicy; macOS cannot encode it")
        expectEqual(offered, ["h264"], "macOS ships H.264 via VideoToolbox only")
        expect(offered.contains(AppModel().streamCodec),
               "the default codec must itself be one the build can encode")
    }


    // ── telemetry: the silently-degraded-recording class ─────────────────────

    /// The core has always published totalDroppedFrames; nothing in the shell
    /// read it, so a recording could be losing frames with no sign anywhere in
    /// the operator's view. The percentage is of ATTEMPTED frames so it stays
    /// meaningful whether a show ran four minutes or four hours.
    @MainActor
    private static func testDroppedFrameReadout() {
        let model = AppModel()
        expectEqual(model.droppedFramesLabel, "0", "no frames attempted reads as a plain zero")

        model.recordingFramesWritten = 1000
        model.recordingDroppedFrames = 0
        expectEqual(model.droppedFramesLabel, "0 (0.0%)", "a clean recording reads 0 (0.0%)")

        model.recordingDroppedFrames = 10          // 10 of 1010 attempted
        expectEqual(model.droppedFramesLabel, "10 (1.0%)",
                    "the percentage is of ATTEMPTED frames, not written ones")

        model.recordingFramesWritten = 0
        model.recordingDroppedFrames = 5
        expectEqual(model.droppedFramesLabel, "5 (100.0%)",
                    "every frame dropped must not divide by zero")
    }


    // ── chroma key: the wrong-colour-keys-the-wrong-thing class ──────────────

    /// The key node is sent ONLY when enabled. The core treats a chromaKey
    /// object without an explicit enabled:false as "key this", so shipping
    /// settings for a switched-off key would punch holes in a live program.
    @MainActor
    private static func testChromaKeyNodeOnlySentWhenEnabled() {
        let model = AppModel()
        var slot = ShowInputSlot(id: 1)
        slot.kind = "capture"
        slot.sourceId = "cam-1"

        slot.keyEnabled = false
        expect(model.chromaKeyNode(for: slot) == nil,
               "a disabled key must send NO node at all")

        slot.keyEnabled = true
        guard let node = model.chromaKeyNode(for: slot) else {
            expect(false, "an enabled key must produce a node")
            return
        }
        expectEqual(node["enabled"] as? Bool ?? false, true, "node is explicitly enabled")
        expectEqual(node["similarity"] as? Double ?? 0, 0.4, "default similarity")
    }

    /// A hex that fails to parse must fall back to GREEN, never to black —
    /// keying on black would key the shadows out of every source.
    @MainActor
    private static func testKeyColourParsing() {
        let green = AppModel.rgbComponents("#00ff00")
        expectEqual(green.g, 1.0, "green channel full")
        expectEqual(green.r, 0.0, "red channel empty")

        let blue = AppModel.rgbComponents("0000ff")
        expectEqual(blue.b, 1.0, "a hex without # still parses")

        let broken = AppModel.rgbComponents("nonsense")
        expectEqual(broken.g, 1.0, "an unparseable colour falls back to GREEN")
        expectEqual(broken.r, 0.0, "and never to black, which would key shadows")
    }

    // ── runner ───────────────────────────────────────────────────────────────

    @MainActor
    static func runAndExit() -> Never {
        failures = []
        checks = 0

        let cases: [(String, () -> Void)] = [
            ("recording/command-retry", testRecordingCommandRetriesAndSupersession),
            ("bridge/generation-lifecycle", testBridgeGenerationRejectsStaleWork),
            ("wire/shared-lifecycle-contracts", testSharedLifecycleContracts),
            ("prefs/older-file", testPrefsSurviveAnOlderFile),
            ("prefs/round-trip", testPrefsRoundTripKeepsEveryField),
            ("prefs/garbage", testPrefsToleratesGarbage),
            ("roster/engine-shape", testRosterParsesTheEngineWireShape),
            ("roster/legacy-shape", testRosterParsesTheLegacyShape),
            ("roster/no-id", testRosterDropsEntriesWithoutAnId),
            ("roster/name-fallback", testRosterNameFallsBackToId),
            ("redaction/secrets", testRedactionRemovesSecretsAndKeepsDiagnostics),
            ("grade/neutral", testColorGradeNeutralDetection),
            ("wire/payload-wrapper", testPayloadWrappedCommandsCarryTheWrapper),
            ("wire/contract-vs-core", testPayloadContractMatchesTheCoreSource),
            ("wire/root-level", testRootLevelCommandsAreNotWrapped),
            ("wire/join-zak", testJoinOmitsAnAbsentZak),
            ("bridge/split-line", testBridgeReassemblesSplitLines),
            ("bridge/multi-line", testBridgeDeliversMultipleLinesFromOneRead),
            ("bridge/malformed", testBridgeSkipsMalformedLinesWithoutStalling),
            ("coalescing/color-grade", testColorGradePushesAreCoalesced),
            ("oauth/no-rotation-keeps-token", testRefreshWithoutRotationKeepsTheStoredToken),
            ("oauth/rotation-replaces", testRefreshWithRotationTakesTheNewToken),
            ("oauth/empty-response", testEmptyResponseDoesNotDestroyTheStoredToken),
            ("oauth/expiry-slack", testExpiryLeavesSlackBeforeTheRealDeadline),
            ("oauth/keychain-identity", testKeychainServiceIsNeverTheObsPluginItem),
            ("oauth/return-uri", testOAuthReturnUriMatchesTheBrokerAllowlist),
            ("encoder/no-unlicensed-codecs", testShellOffersOnlyEncodableCodecs),
            ("telemetry/dropped-frames", testDroppedFrameReadout),
            ("chromakey/enabled-only", testChromaKeyNodeOnlySentWhenEnabled),
            ("chromakey/colour-parsing", testKeyColourParsing),
        ]
        for (name, body) in cases {
            FileHandle.standardError.write("  running \(name)\n".data(using: .utf8)!)
            body()
        }

        if failures.isEmpty {
            print("\(checks) shell checks passed, 0 failed.")
            exit(0)
        }
        for failure in failures {
            print("FAIL: \(failure)")
        }
        print("\(checks) shell checks run, \(failures.count) failed.")
        exit(1)
    }
}
