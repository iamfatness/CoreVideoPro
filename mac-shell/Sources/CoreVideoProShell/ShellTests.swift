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

    // ── runner ───────────────────────────────────────────────────────────────

    @MainActor
    static func runAndExit() -> Never {
        failures = []
        checks = 0

        let cases: [(String, () -> Void)] = [
            ("prefs/older-file", testPrefsSurviveAnOlderFile),
            ("prefs/round-trip", testPrefsRoundTripKeepsEveryField),
            ("prefs/garbage", testPrefsToleratesGarbage),
            ("roster/engine-shape", testRosterParsesTheEngineWireShape),
            ("roster/legacy-shape", testRosterParsesTheLegacyShape),
            ("roster/no-id", testRosterDropsEntriesWithoutAnId),
            ("roster/name-fallback", testRosterNameFallsBackToId),
            ("redaction/secrets", testRedactionRemovesSecretsAndKeepsDiagnostics),
            ("grade/neutral", testColorGradeNeutralDetection),
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
