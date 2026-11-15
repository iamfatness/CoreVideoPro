// Tier 0 mac-vs-Windows parity tests (2026-11-15 consolidated audit, Tier 0
// table). Kept in their own file so the parallel audio / multiview branches
// that also extend ShellTests.swift do not collide line-for-line.

import AppKit
import Foundation
import SwiftUI

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var tier0Cases: [(String, () -> Void)] {
        [
            ("tier0/production-settings-relaunch", testProductionSettingsSurviveARelaunch),
            ("tier0/production-settings-every-generation",
             testProductionSettingsAreReassertedOnEveryCoreGeneration),
            ("tier0/production-labels-and-bitrate", testProductionLabelsAndStreamBitrateAreReal),
            ("tier0/slots-survive-relaunch", testSlotAssignmentsSurviveARelaunch),
            ("tier0/slot-settings-follow-the-source", testPerSourceSettingsFollowTheSourceNotTheSlot),
            ("tier0/iso-survives-first-assignment", testRestoredIsoSelectionSurvivesTheFirstAssignment),
            ("tier0/iso-matrix-one-truth", testIsoMatrixAndSourcesToggleAreOneTruth),
            ("tier0/sources-rows-fit-the-page", testSourcesRowsFitThePageAtTheMinimumWindow),
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

    // ── 0.8 slot assignments + ISO selection persist ────────────────────────

    @MainActor
    static func relaunched(_ before: AppModel) -> AppModel? {
        guard let data = try? JSONEncoder().encode(before.currentPrefs()),
              let saved = try? JSONDecoder().decode(ShellPrefs.self, from: data) else { return nil }
        let after = AppModel()
        after.applyRestoredPrefs(saved)
        return after
    }

    /// Capture rows vanished every launch (sources audit row 75).
    @MainActor
    static func testSlotAssignmentsSurviveARelaunch() {
        let before = AppModel()
        before.assignSlot(3, kind: "capture", sourceId: "cam1", name: "Wide cam")
        before.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest")
        before.toggleSlotInShow(1)   // parked, not in show
        guard let after = relaunched(before) else { expect(false, "relaunch"); return }
        let cam = after.slots.first { $0.id == 3 }
        expectEqual(cam?.kind, "capture", "capture assignment restored")
        expectEqual(cam?.sourceId, "cam1", "capture source restored")
        expectEqual(cam?.name, "Wide cam", "display name restored")
        expectEqual(cam?.inShow, true, "in-show restored")
        expectEqual(cam?.offline, true, "a restored source reads offline until it is seen")
        let guest = after.slots.first { $0.id == 1 }
        expectEqual(guest?.sourceId, "42", "zoom assignment restored")
        expectEqual(guest?.inShow, false, "a parked slot stays parked")
        expect(after.captureReconnectPending.contains("cam1"),
               "a restored capture slot reconnects its device when the core lists it")
        let empty = AppModel()
        empty.applyRestoredPrefs(ShellPrefs())
        expect(empty.slots.allSatisfy { $0.kind == "unassigned" },
               "an older prefs file (no slots key) starts with empty slots, as before")
    }

    /// Key / mic / dropout were keyed by SLOT NUMBER, so they reattached to
    /// whatever source landed in that slot.
    @MainActor
    static func testPerSourceSettingsFollowTheSourceNotTheSlot() {
        var prefs = ShellPrefs()
        // The camera now sits in slot 5; its settings were saved from slot 3.
        prefs.showInputSlots = [
            PersistedSlot(slotId: 3, kind: "capture", sourceId: "other", name: "Other", inShow: true),
            PersistedSlot(slotId: 5, kind: "capture", sourceId: "cam1", name: "Cam", inShow: true),
            PersistedSlot(slotId: 2, kind: "zoom", sourceId: "42", name: "Guest", inShow: true),
        ]
        prefs.chromaKeys = [PersistedChromaKey(slotId: 3, colorHex: "#0000ff", sourceKey: "capture:cam1")]
        prefs.capturePairings = [CapturePairing(slotId: 3, audioDeviceId: "mic", audioDeviceName: "Mic",
                                                sourceKey: "capture:cam1")]
        prefs.slotDropoutPolicies = [PersistedSlotPolicy(slotId: 7, dropoutPolicy: "black",
                                                         sourceKey: "zoom:42")]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        let cam = model.slots.first { $0.id == 5 }
        let other = model.slots.first { $0.id == 3 }
        expectEqual(cam?.keyEnabled, true, "the key reattaches to its source's slot")
        expectEqual(cam?.keyColorHex, "#0000ff", "with its own settings")
        expectEqual(other?.keyEnabled, false, "never to whatever now sits in the old slot")
        expectEqual(cam?.audioDeviceId, "mic", "the mic pairing follows the camera")
        expectEqual(other?.audioDeviceId, "", "the old slot does not inherit the mic")
        expectEqual(model.slots.first { $0.id == 2 }?.dropoutPolicy, "black",
                    "the dropout policy follows the guest")

        // Saving writes the source identity, so the next restore can do this.
        let saved = model.currentPrefs()
        expectEqual(saved.chromaKeys?.first?.sourceKey, "capture:cam1", "keys save their source")
        expectEqual(saved.capturePairings?.first?.sourceKey, "capture:cam1", "pairings save their source")
        expectEqual(saved.slotDropoutPolicies?.first?.sourceKey, "zoom:42", "policies save their source")
    }

    /// recomputeFromSlots rebuilt the selection from slot flags; a freshly
    /// assigned slot has iso=false, so the first assignment wiped it.
    @MainActor
    static func testRestoredIsoSelectionSurvivesTheFirstAssignment() {
        var prefs = ShellPrefs()
        prefs.isoRecordingEnabled = true
        prefs.isoSelectedSourceIds = ["capture:cam1", "zoom:7"]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        model.assignSlot(2, kind: "zoom", sourceId: "7", name: "Guest")
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1", "zoom:7"],
                    "the first assignment keeps the restored ISO selection")
        expect(model.isoRecordingEnabled, "and the ISO master switch")
        expectEqual(model.slots.first { $0.id == 2 }?.iso, true,
                    "the assigned slot shows its source's saved ISO state")
        model.unassignSlot(2)
        expect(model.isoSelectedSourceIds.contains("zoom:7"),
               "unassign keeps the source's ISO intent (Windows ShowInputsCoordinator)")
    }

    /// The matrix kept its own column map, never rebuilt from the selection,
    /// so after a relaunch or a Sources-tab toggle it showed no ISO dots while
    /// ISOs were armed — and the next click replaced the whole selection.
    @MainActor
    static func testIsoMatrixAndSourcesToggleAreOneTruth() {
        var prefs = ShellPrefs()
        prefs.isoRecordingEnabled = true
        prefs.isoSelectedSourceIds = ["capture:cam1"]
        prefs.showInputSlots = [
            PersistedSlot(slotId: 1, kind: "capture", sourceId: "cam1", name: "Cam", inShow: true),
            PersistedSlot(slotId: 2, kind: "zoom", sourceId: "7", name: "Guest", inShow: true),
        ]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        expectEqual(model.isoColumn(of: "capture:cam1"), 1, "the matrix shows the restored ISO")
        model.toggleSlotIso(2)      // Sources tab
        expect(model.isoColumn(of: "zoom:7") != nil, "a Sources-tab ISO toggle appears in the matrix")
        model.toggleIsoCell(column: 5, sourceId: "zoom:7")   // move to ISO 5
        expectEqual(model.isoColumn(of: "zoom:7"), 5, "the matrix move keeps its column")
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1", "zoom:7"],
                    "a matrix click edits the selection, it does not replace it")
        model.toggleSlotIso(1)      // Sources tab: cam1 off
        expectEqual(model.isoColumn(of: "capture:cam1"), nil, "unchecking in Sources clears its matrix cell")
        expectEqual(model.slots.first { $0.id == 1 }?.iso, false, "and its Sources checkbox")
        model.toggleIsoCell(column: 5, sourceId: "capture:cam1")  // takes zoom:7's column
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1"], "column-exclusive: the occupant leaves")
        expectEqual(model.slots.first { $0.id == 2 }?.iso, false, "and its Sources checkbox follows")
        expectEqual(model.slots.first { $0.id == 1 }?.iso, true, "the matrix pick shows in Sources")
    }

    // ── 0.10 tab width cap ───────────────────────────────────────────────────

    /// TabPage capped every tab at 720 pt; an assigned Sources row is built
    /// from fixed-width controls of ~1000 pt, so it was compressed or clipped.
    @MainActor
    static func testSourcesRowsFitThePageAtTheMinimumWindow() {
        let model = AppModel()
        TabSnapshotHarness.seed(model)   // zoom + capture-with-mic rows (the widest)
        var widest: CGFloat = 0
        for slot in model.slots where slot.kind != "unassigned" {
            let host = NSHostingView(rootView: SlotRow(slot: slot).environmentObject(model)
                .fixedSize())
            host.layoutSubtreeIfNeeded()
            widest = max(widest, host.fittingSize.width)
        }
        expect(widest > 600, "slot row measured (\(widest) pt)")
        // The Sources card adds 14 pt padding each side inside the page.
        let available = TabPageLayout.pageWidth(for: .sources, window: 1360) - 28
        expect(widest <= available,
               "an assigned Sources row (\(widest) pt) fits the page at the 1360 pt minimum window (\(available) pt)")
        expect(TabPageLayout.pageWidth(for: .routing, window: 1360) >= 760,
               "the Routing grid (~724 pt) fits without clipping its last column")
        for tab in StudioTab.allCases where tab != .studio {
            expect(TabPageLayout.pageWidth(for: tab, window: 1360) >= 720,
                   "\(tab.rawValue) keeps at least the old 720 pt measure")
        }
        guard let shell = sourceFile("ShellApp.swift") else {
            expect(false, "ShellApp.swift not found next to the binary"); return
        }
        expect(!shell.contains(".frame(maxWidth: 720)\n                Spacer(minLength: 0)"),
               "TabPage no longer applies a flat 720 pt cap")
        expect(shell.contains("TabPageContent(tab: model.selectedTab)"),
               "TabPage draws the shared TabPageContent (what the snapshot harness renders)")
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
