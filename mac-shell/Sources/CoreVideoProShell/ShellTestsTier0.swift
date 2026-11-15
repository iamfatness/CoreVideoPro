// Tier 0 mac-vs-Windows parity tests (2026-11-15 consolidated audit, Tier 0
// table). Kept in their own file so the parallel audio / multiview branches
// that also extend ShellTests.swift do not collide line-for-line.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var tier0Cases: [(String, () -> Void)] {
        [
            ("tier0/production-settings-relaunch", testProductionSettingsSurviveARelaunch),
            ("tier0/production-settings-every-generation",
             testProductionSettingsAreReassertedOnEveryCoreGeneration),
            ("tier0/production-labels-and-bitrate", testProductionLabelsAndStreamBitrateAreReal),
        ]
    }

    // ── 0.7 production settings persist + re-assert ─────────────────────────

    /// A 4K / 30 fps / grid show came back as 1080p60 P/P-top on every launch:
    /// none of these values were in ShellPrefs.
    @MainActor
    static func testProductionSettingsSurviveARelaunch() {
        let before = AppModel()
        before.canvasWidth = 3840
        before.canvasHeight = 2160
        before.canvasFps = 30
        before.streamBitrateMbps = 12.5
        before.recordFormat = "mov"
        before.recordPrefix = "gala"
        before.multiviewLayoutMode = "grid"
        before.multiviewTileCount = 6
        guard let data = try? JSONEncoder().encode(before.currentPrefs()),
              let saved = try? JSONDecoder().decode(ShellPrefs.self, from: data) else {
            expect(false, "prefs encode/decode")
            return
        }
        let after = AppModel()
        after.applyRestoredPrefs(saved)
        expectEqual(after.canvasWidth, 3840, "canvas width restored")
        expectEqual(after.canvasHeight, 2160, "canvas height restored")
        expectEqual(after.canvasFps, 30, "canvas fps restored")
        expectEqual(after.streamBitrateMbps, 12.5, "stream bitrate restored")
        expectEqual(after.recordFormat, "mov", "recording format restored")
        expectEqual(after.recordPrefix, "gala", "recording prefix restored")
        expectEqual(after.multiviewLayoutMode, "grid", "multiview layout restored")
        expectEqual(after.multiviewTileCount, 6, "multiview tile count restored")

        // An older file (no production keys) keeps the built-in defaults.
        let older = AppModel()
        older.applyRestoredPrefs(ShellPrefs())
        expectEqual(older.canvasLabel, "1080p60", "absent keys keep the 1080p60 default")
        expectEqual(older.multiviewLayoutMode, "pgmPvwTop", "absent keys keep P/P top")

        // A hand-edited file cannot put an unpickable value on the wire.
        var bad = ShellPrefs()
        bad.canvasWidth = 1234; bad.canvasHeight = 567; bad.canvasFps = 59
        bad.recordFormat = "avi"; bad.multiviewLayoutMode = "nonsense"
        bad.multiviewTileCount = 99; bad.streamBitrateMbps = 500
        let guarded = AppModel()
        guarded.applyRestoredPrefs(bad)
        expectEqual(guarded.canvasWidth, 1920, "an unpickable canvas is refused")
        expectEqual(guarded.canvasFps, 60, "an unpickable fps is refused")
        expectEqual(guarded.recordFormat, "mp4", "an unknown format is refused")
        expectEqual(guarded.multiviewLayoutMode, "pgmPvwTop", "an unknown layout is refused")
        expectEqual(guarded.multiviewTileCount, 16, "tile count clamps to the stepper range")
        expectEqual(guarded.streamBitrateMbps, 20, "bitrate clamps to the slider range")
    }

    /// A respawned core starts at 1920x1080@60 / grid. The canvas and the
    /// multiview config must ride every core generation, not one launch.
    @MainActor
    static func testProductionSettingsAreReassertedOnEveryCoreGeneration() {
        let core = FakeTakeCore()
        let model = AppModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        var saved = ShellPrefs()
        saved.canvasWidth = 3840; saved.canvasHeight = 2160; saved.canvasFps = 30
        saved.multiviewLayoutMode = "grid"; saved.multiviewTileCount = 6
        model.applyRestoredPrefs(saved)
        model.reassertProductionSettings()
        spin(until: { !core.bodies.isEmpty }, limit: 3)
        let commands = (core.bodies.first?["commands"] as? [JSONObject]) ?? []
        let profile = commands.first { $0["type"] as? String == "set-output-profile" }
        let multiview = commands.first { $0["type"] as? String == "configure-multiviewer" }
        expectEqual(profile?["width"] as? Int, 3840, "the restored canvas width is re-sent")
        expectEqual(profile?["fps"] as? Int, 30, "the restored canvas fps is re-sent")
        expectEqual(multiview?["layoutMode"] as? String, "grid", "the restored layout is re-sent")
        expectEqual(multiview?["tileCount"] as? Int, 6, "the restored tile count is re-sent")

        // onConnected runs on EVERY generation (initial handshake and respawn);
        // it must call the re-assert, not the old multiview-only one-shot.
        guard let body = functionBody(named: "private func onConnected()", in: "AppModel.swift") else {
            expect(false, "AppModel.swift onConnected() not found next to the binary")
            return
        }
        expect(body.contains("reassertProductionSettings()"),
               "onConnected re-asserts the production settings on every core generation")
    }

    /// The labels follow the real values; the stream recipe sends the
    /// operator's bitrate rather than a hard-coded 6 Mbps.
    @MainActor
    static func testProductionLabelsAndStreamBitrateAreReal() {
        let model = AppModel()
        model.canvasHeight = 2160
        model.canvasWidth = 3840
        model.canvasFps = 30
        expectEqual(model.canvasLabel, "2160p30", "multiview chip reflects the real canvas")
        model.streamBitrateMbps = 12.5
        let start = model.startProgramOutput(recording: false, streaming: true)
        let profile = start["streamOutputProfile"] as? JSONObject
        expectEqual((profile?["targetBitrateMbps"] as? NSNumber)?.doubleValue, 12.5,
                    "the stream profile carries the operator's bitrate")
        let rtmp = (start["destinationSettings"] as? [JSONObject])?.first
        expectEqual((rtmp?["targetBitrateMbps"] as? NSNumber)?.doubleValue, 12.5,
                    "the RTMP destination's explicit bitrate (which wins core-side) is the operator's")
        expect(ProductionSettingsPolicy.streamRecipeLabel(bitrateMbps: 12.5).contains("12.5 Mbps"),
               "the stream flyout states the bitrate that is sent")
        guard let shell = sourceFile("ShellApp.swift") else {
            expect(false, "ShellApp.swift not found next to the binary")
            return
        }
        expect(!shell.contains("MonoChip(\"1080p60\")"), "no hard-coded 1080p60 multiview chip")
    }

    // ── source-reading helpers ───────────────────────────────────────────────

    static func sourceFile(_ name: String) -> String? {
        let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try? String(contentsOf: dir.appendingPathComponent(name), encoding: .utf8)
    }

    /// The text of one function: from its signature to the next line that
    /// closes a member at the declaration's indentation.
    static func functionBody(named signature: String, in file: String) -> String? {
        guard let source = sourceFile(file), let start = source.range(of: signature) else {
            return nil
        }
        let lineStart = source[..<start.lowerBound].lastIndex(of: "\n")
            .map { source.index(after: $0) } ?? source.startIndex
        let indent = String(source[lineStart..<start.lowerBound])
        let tail = source[start.upperBound...]
        guard let end = tail.range(of: "\n" + indent + "}\n") else { return String(tail) }
        return String(tail[..<end.upperBound])
    }
}
