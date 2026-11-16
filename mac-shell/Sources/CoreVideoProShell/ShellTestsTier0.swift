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
            ("tier0/stream-start-keeps-the-canvas", testStreamStartSendsTheCanvasAndLabelsReadTheCore),
            ("tier0/slots-survive-relaunch", testSlotAssignmentsSurviveARelaunch),
            ("tier0/slot-settings-follow-the-source", testPerSourceSettingsFollowTheSourceNotTheSlot),
            ("tier0/slot-name-resets-on-a-new-source", testASlotNameResetsWhenTheSlotGetsADifferentSource),
            ("tier0/slot-manual-name-is-session-only", testAManualSlotNameIsNotSaved),
            ("tier0/slot-name-follows-the-live-source", testAnUnrenamedSlotFollowsItsLiveSourceName),
            ("tier0/iso-survives-first-assignment", testRestoredIsoSelectionSurvivesTheFirstAssignment),
            ("tier0/iso-matrix-one-truth", testIsoMatrixAndSourcesToggleAreOneTruth),
            ("tier0/iso-records-only-assigned-video-sources", testIsoRecordsOnlyAssignedVideoBearingSources),
            ("tier0/iso-stale-intent-pruned-and-invisible", testStaleIsoIntentIsPrunedAndNeverTakesAColumn),
            ("tier0/sources-rows-fit-the-page", testSourcesRowsFitThePageAtTheMinimumWindow),
            ("tier0/recording-folder-chosen-and-persisted", testRecordingFolderIsChosenPersistedAndUsed),
            ("tier0/recording-folder-falls-back-loudly", testRecordingFolderFallsBackLoudly),
            ("tier0/record-flyout-shows-the-effective-folder", testRecordFlyoutShowsTheEffectiveFolder),
            ("tier0/recording-folder-real-write-probe", testRecordingFolderIsProbedWithARealWrite),
            ("tier0/zoom-status-reads-sdk-readiness", testZoomStatusReadsTheCoresSdkReadiness),
            ("tier0/lower-third-keys-a-program-source", testManualLowerThirdKeysAProgramSourceTheCoreGateAdmits),
            ("tier0/lower-third-status-from-core", testLowerThirdStatusComesFromTheCoresOverlayState),
            ("tier0/lower-third-resolver-is-windows", testLowerThirdResolverIsTheWindowsRule),
            ("tier0/monitor-device-chosen-persisted-sent", testMonitorDeviceIsChosenPersistedAndSent),
            ("tier0/monitor-device-falls-back-loudly", testMonitorDeviceFallsBackLoudlyAndReturns),
            ("tier0/monitor-device-every-generation", testMonitorDeviceRidesEveryCoreGeneration),
            ("tier0/monitor-projection-carries-device", testMonitorProjectionCarriesTheDevice),
            ("tier0/end-other-session-offered-only-on-collision", testEndOtherSessionIsOfferedOnlyOnTheCollision),
            ("tier0/end-other-session-join-is-explicit", testEndOtherSessionJoinIsExplicitAndNeverSticky),
            ("tier0/studio-edit-layout-edits-preview", testStudioEditLayoutEditsThePreviewSceneInPlace),
            ("tier0/studio-add-overlay", testStudioAddOverlayPlacesATopMostPreviewLayer),
            ("tier0/studio-rail-row-actions", testSceneRailRowActionsTargetTheRowWithoutCueing),
            ("tier0/studio-multiview-drag-swap", testMultiviewDragSwapsTheTwoInputSlots),
            ("tier0/drag-swap-never-moves-a-solo-cue", testDragSwapNeverMovesAnOnAirSoloCue),
            ("tier0/removing-preview-resyncs-pvw", testRemovingThePreviewSceneResyncsThePreviewBus),
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
        // No access modifier in the needle: mac-zoom-audio made onConnected()
        // internal so the harness can drive a core generation.
        guard let body = functionBody(named: "func onConnected()", in: "AppModel.swift") else {
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

    /// Review I3: the stream profile writes the same core fields as the
    /// canvas, so its hard-coded 1080p30 re-sized Program for every output
    /// when Stream was pressed, while the chip showed the Settings value.
    @MainActor
    static func testStreamStartSendsTheCanvasAndLabelsReadTheCore() {
        let model = AppModel()
        model.canvasWidth = 3840
        model.canvasHeight = 2160
        model.canvasFps = 60
        let start = model.startProgramOutput(recording: false, streaming: true)
        let profile = start["streamOutputProfile"] as? JSONObject
        expectEqual(profile?["width"] as? Int, 3840, "stream start sends the canvas width, not 1920")
        expectEqual(profile?["height"] as? Int, 2160, "and the canvas height, not 1080")
        expectEqual(profile?["fps"] as? Int, 60, "and the canvas fps, not 30")
        expectEqual(profile?["codec"] as? String, "h264", "under the key the core reads (codec)")
        let rtmp = (start["destinationSettings"] as? [JSONObject])?.first
        expectEqual(rtmp?["fps"] as? Int, 30, "the stream itself stays at the destination's 30 fps")

        // Labels show what the core renders, never the shell's setting.
        expectEqual(model.coreCanvasLabel, "\u{2014}", "no core yet: no claim")
        model.applySnapshotForTesting(["outputProfile": [
            "width": 1920, "height": 1080, "fps": 30, "resolution": "1920x1080"] as JSONObject])
        expectEqual(model.coreCanvasLabel, "1080p30",
                    "the chip reads the core's outputProfile even when Settings says 2160p60")
        guard let shell = sourceFile("ShellApp.swift"), let footer = sourceFile("StudioFooter.swift") else {
            expect(false, "sources not found next to the binary"); return
        }
        expect(shell.contains("MonoChip(model.coreCanvasLabel)"), "the multiview chip reads the core")
        expect(footer.contains("value: model.coreCanvasLabel,"), "the footer Program stat reads the core")
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
        before.assignSlot(4, kind: "capture", sourceId: "cam2", name: "Tight cam")
        before.toggleSlotInShow(4)   // parked, not in show
        expectEqual(before.currentPrefs().showInputSlots?.map(\.kind), ["capture", "capture"],
                    "only capture slots are written (plan D2)")
        guard let after = relaunched(before) else { expect(false, "relaunch"); return }
        let cam = after.slots.first { $0.id == 3 }
        expectEqual(cam?.kind, "capture", "capture assignment restored")
        expectEqual(cam?.sourceId, "cam1", "capture source restored")
        expectEqual(cam?.name, "Wide cam", "display name restored")
        expectEqual(cam?.inShow, true, "in-show restored")
        expectEqual(cam?.offline, true, "a restored source reads offline until it is seen")
        let guest = after.slots.first { $0.id == 1 }
        expectEqual(guest?.kind, "unassigned", "a Zoom slot is not restored (Zoom reuses ids; owner decision 2)")
        expectEqual(after.slots[3].inShow, false, "a parked slot stays parked")
        expect(after.captureReconnectPending.contains("cam1"),
               "a restored capture slot reconnects its device when the core lists it")
        let empty = AppModel()
        empty.applyRestoredPrefs(ShellPrefs())
        expect(empty.slots.allSatisfy { $0.kind == "unassigned" },
               "an older prefs file (no slots key) starts with empty slots, as before")
    }

    /// #1213 (owner ruling 2026-11-16): a hand-typed slot name belongs to that
    /// slot+source pairing. Live defect: a slot renamed for a guest kept the
    /// guest's name after a webcam was assigned to it.
    @MainActor
    static func testASlotNameResetsWhenTheSlotGetsADifferentSource() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "a", name: "Alice")
        model.slots[0].name = "Host"                       // the operator's rename
        model.assignSlot(1, kind: "zoom", sourceId: "a", name: "Alice")
        expectEqual(model.slots[0].name, "Host", "re-assigning the SAME source keeps the rename")
        model.assignSlot(1, kind: "capture", sourceId: "cam1", name: "Desk cam")
        expectEqual(model.slots[0].name, "Desk cam", "a different source shows its own name")
        model.slots[0].name = "Wide"
        model.assignSlot(1, kind: "capture", sourceId: "cam2", name: "Side cam")
        expectEqual(model.slots[0].name, "Side cam", "same kind, different device: still a new source")
    }

    /// #1213: a manual name lasts one session. What is saved is the source's
    /// own name, so a relaunch shows that.
    @MainActor
    static func testAManualSlotNameIsNotSaved() {
        let model = AppModel()
        // Slot 1 is a capture source: since plan D2 a Zoom slot is not saved at all.
        model.assignSlot(1, kind: "capture", sourceId: "cam0", name: "Alice")
        model.assignSlot(2, kind: "capture", sourceId: "cam1", name: "Desk cam")
        model.slots[0].name = "Host"
        let saved = model.currentPrefs().showInputSlots ?? []
        expectEqual(saved.first { $0.slotId == 1 }?.name, "Alice", "the rename is not written to disk")
        expectEqual(saved.first { $0.slotId == 2 }?.name, "Desk cam", "an untouched name saves as before")
        let relaunched = AppModel()
        var prefs = ShellPrefs(); prefs.showInputSlots = saved
        relaunched.applyRestoredPrefs(prefs)
        expectEqual(relaunched.slots[0].name, "Alice", "the next launch shows the source's own name")
    }

    /// #1213: the live source's name wins unless the operator renamed the slot
    /// this session. This is also what clears a manual name saved by an older
    /// build: it is restored as if it were the source's name, then replaced
    /// the moment the source shows up.
    static func testAnUnrenamedSlotFollowsItsLiveSourceName() {
        var r = SlotNamePolicy.withLiveName(name: "Old guest", sourceName: "Old guest", liveName: "FaceTime HD Camera")
        expectEqual(r.name, "FaceTime HD Camera", "a legacy saved name gives way to the source's own name")
        expectEqual(r.sourceName, "FaceTime HD Camera", "and the source name is learned")
        r = SlotNamePolicy.withLiveName(name: "Host", sourceName: "Alice", liveName: "Alice B")
        expectEqual(r.name, "Host", "a rename made this session is kept")
        expectEqual(r.sourceName, "Alice B", "while the source's name is still tracked for saving")
        r = SlotNamePolicy.withLiveName(name: "Alice", sourceName: "Alice", liveName: "")
        expectEqual(r.name, "Alice", "an absent source changes nothing")
        var slot = ShowInputSlot(id: 1)
        slot.kind = "zoom"; slot.sourceId = "a"; slot.name = "Host"
        expectEqual(SlotNamePolicy.persisted(slot), "Host",
                    "a slot with no known source name saves what it shows (pre-#1213 state)")
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
        expect(model.slots.allSatisfy { $0.dropoutPolicy.isEmpty },
               "a Zoom policy has no slot to follow after a relaunch")

        // Saving writes the source identity, so the next restore can do this.
        let saved = model.currentPrefs()
        expectEqual(saved.chromaKeys?.first?.sourceKey, "capture:cam1", "keys save their source")
        expectEqual(saved.capturePairings?.first?.sourceKey, "capture:cam1", "pairings save their source")
        expect((saved.slotDropoutPolicies ?? []).isEmpty, "no Zoom slot, no Zoom policy saved")
    }

    /// recomputeFromSlots rebuilt the selection from slot flags; a freshly
    /// assigned slot has iso=false, so the first assignment wiped it.
    @MainActor
    static func testRestoredIsoSelectionSurvivesTheFirstAssignment() {
        var prefs = ShellPrefs()
        prefs.isoRecordingEnabled = true
        prefs.isoSelectedSourceIds = ["capture:cam1", "capture:cam2", "zoom:7"]
        prefs.showInputSlots = [PersistedSlot(slotId: 2, kind: "capture", sourceId: "cam2",
                                              name: "Cam 2", inShow: true)]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        model.assignSlot(3, kind: "capture", sourceId: "cam1", name: "Cam")
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1", "capture:cam2"],
                    "the restored ISO selection survives the first assignment; "
                        + "last meeting's unslotted zoom:7 is pruned")
        expect(model.isoRecordingEnabled, "and the ISO master switch")
        expectEqual(model.slots.first { $0.id == 3 }?.iso, true,
                    "the assigned slot shows its source's saved ISO state")
        expectEqual(model.slots.first { $0.id == 2 }?.iso, true, "the restored slot keeps its ISO")
        model.unassignSlot(2)
        expect(model.isoSelectedSourceIds.contains("capture:cam2"),
               "unassign keeps the source's ISO intent (Windows ShowInputsCoordinator)")
    }

    /// Fix round 1, I1: intent survives unassign, but what RECORDS is only
    /// assigned, present, video-bearing sources (Windows
    /// ComputeEligiblePresentIsoSourceIds, ShowInputsCoordinator.cs:334-365).
    @MainActor
    static func testIsoRecordsOnlyAssignedVideoBearingSources() {
        let model = AppModel()
        model.roster = [RosterParticipant(id: "7", name: "Guest", hasVideo: true,
                                          muted: false, talking: false, assigned: false)]
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Cam", kind: "video", vendor: "",
                                                 connectionState: "connected", signalPresent: true,
                                                 warning: "")]
        model.assignSlot(2, kind: "zoom", sourceId: "7", name: "Guest")
        model.toggleSlotIso(2)
        expectEqual(model.peekIsoSourceIds(recording: true), ["zoom:7"], "an assigned guest records")
        expectEqual(model.resolvedIsoSourceIds(recording: true), ["zoom:7"], "the take arms it")
        model.unassignSlot(2)
        expect(model.isoSelectedSourceIds.contains("zoom:7"), "unassign keeps the ISO intent")
        expectEqual(model.peekIsoSourceIds(recording: true), [],
                    "an unassigned guest still in the meeting does NOT record")
        expectEqual(model.resolvedIsoSourceIds(recording: true), [],
                    "and the live take drops its writer")
        model.assignSlot(2, kind: "zoom", sourceId: "7", name: "Guest")
        expectEqual(model.peekIsoSourceIds(recording: true), ["zoom:7"], "re-assigning brings it back")
        model.roster = [RosterParticipant(id: "7", name: "Guest", hasVideo: false,
                                          muted: false, talking: false, assigned: true)]
        expectEqual(model.peekIsoSourceIds(recording: false), [],
                    "a video-off guest does not take an ISO encoder slot")
        model.isoSelectedSourceIds.insert("capture:cam1")   // selected, connected, NOT assigned
        expectEqual(model.peekIsoSourceIds(recording: false), [],
                    "an unassigned connected camera does not record")
    }

    /// Fix round 1, I2: Zoom ids change every meeting. Stale `zoom:` intent
    /// must not take matrix columns, starve a new pick, or hold the ISO
    /// master switch on.
    @MainActor
    static func testStaleIsoIntentIsPrunedAndNeverTakesAColumn() {
        // Eight stale guests still in the selection (as if left in memory),
        // plus one live, assigned guest the operator now ticks.
        let model = AppModel()
        model.isoSelectedSourceIds = Set((1...8).map { "zoom:old\($0)" })
        model.assignSlot(1, kind: "zoom", sourceId: "new", name: "New guest")
        model.toggleSlotIso(1)
        expect(model.isoColumn(of: "zoom:new") != nil, "a new tick gets a column despite 8 stale ids")
        expect(model.isoColumnAssignments.values.allSatisfy { $0 == "zoom:new" },
               "stale ids take no ISO cells")
        expect(model.isoRecordingEnabled, "ticking a visible ISO arms the master switch")
        model.toggleSlotIso(1)
        expect(!model.isoRecordingEnabled,
               "unticking the last VISIBLE ISO disarms it, whatever stale intent remains")

        // Meeting over: unslotted zoom intent is dropped; capture and slotted stay.
        model.isoSelectedSourceIds.formUnion(["capture:cam1", "zoom:new"])
        model.meetingState = "in_meeting"
        model.applyZoomForTesting(["meetingState": "idle", "participants": [JSONObject]()])
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1", "zoom:new"],
                    "leaving the meeting prunes the stale guests, keeps cameras and slotted guests")

        // Relaunch: last meeting's unslotted guests are not restored.
        var prefs = ShellPrefs()
        prefs.isoSelectedSourceIds = ["zoom:gone", "zoom:7", "capture:cam1"]
        prefs.showInputSlots = [PersistedSlot(slotId: 2, kind: "zoom", sourceId: "7",
                                              name: "Guest", inShow: true)]
        let relaunched = AppModel()
        relaunched.applyRestoredPrefs(prefs)
        expectEqual(relaunched.isoSelectedSourceIds, ["capture:cam1"],
                    "a relaunch restores no Zoom slot, so all zoom: intent is pruned")
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
            // A capture source: since plan D2 a saved Zoom slot is not restored.
            PersistedSlot(slotId: 2, kind: "capture", sourceId: "cam7", name: "Cam 7", inShow: true),
        ]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        expectEqual(model.isoColumn(of: "capture:cam1"), 1, "the matrix shows the restored ISO")
        model.toggleSlotIso(2)      // Sources tab
        expect(model.isoColumn(of: "capture:cam7") != nil, "a Sources-tab ISO toggle appears in the matrix")
        model.toggleIsoCell(column: 5, sourceId: "capture:cam7")   // move to ISO 5
        expectEqual(model.isoColumn(of: "capture:cam7"), 5, "the matrix move keeps its column")
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1", "capture:cam7"],
                    "a matrix click edits the selection, it does not replace it")
        model.toggleSlotIso(1)      // Sources tab: cam1 off
        expectEqual(model.isoColumn(of: "capture:cam1"), nil, "unchecking in Sources clears its matrix cell")
        expectEqual(model.slots.first { $0.id == 1 }?.iso, false, "and its Sources checkbox")
        model.toggleIsoCell(column: 5, sourceId: "capture:cam1")  // takes cam7's column
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1"], "column-exclusive: the occupant leaves")
        expectEqual(model.slots.first { $0.id == 2 }?.iso, false, "and its Sources checkbox follows")
        expectEqual(model.slots.first { $0.id == 1 }?.iso, true, "the matrix pick shows in Sources")
    }

    // ── 0.10 tab width cap ───────────────────────────────────────────────────

    /// TabPage capped every tab at 720 pt; an assigned Sources row is built
    /// from fixed-width controls of ~1000 pt, so it was compressed or clipped.
    @MainActor
    static func testSourcesRowsFitThePageAtTheMinimumWindow() {
        // The row measurement moved to sources-table/fits-the-page (SlotRow is gone,
        // sources plan Task 7); the page-width and TabPage checks stay here.
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

    // ── 0.11 recording folder chooser ────────────────────────────────────────

    static func scratchDir(_ name: String) -> String {
        let dir = NSTemporaryDirectory() + "corevideo-shell-harness-\(getpid())/\(name)-\(UUID().uuidString)"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    /// The folder was fixed to ~/Movies/CoreVideoPro: no external drive.
    @MainActor
    static func testRecordingFolderIsChosenPersistedAndUsed() {
        let chosen = scratchDir("external")
        let before = AppModel()
        expect(before.setRecordingFolder(chosen), "a writable folder is accepted")
        guard let after = relaunched(before) else { expect(false, "relaunch"); return }
        expectEqual(after.recordingFolderChoice, chosen, "the chosen folder survives a relaunch")
        expectEqual(after.recordingFolderDisplay, chosen, "Settings shows the chosen folder")

        let core = FakeTakeCore()
        after.quitRequestForTesting = { body, _ in try await core.handle(body) }
        after.toggleRecording()
        spin(until: { !core.bodies.isEmpty }, limit: 3)
        let commands = (core.bodies.first?["commands"] as? [JSONObject]) ?? []
        let targets = commands.first { $0["type"] as? String == "set-recording-targets" }
        expectEqual(targets?["targetFolder"] as? String, chosen, "Record writes to the chosen folder")

        // A folder that cannot take recordings is refused, and the choice stands.
        let locked = scratchDir("locked")
        chmod(locked, 0o555)
        defer { chmod(locked, 0o755) }
        expect(!after.setRecordingFolder(locked), "a read-only folder is refused")
        expect(after.recordingFolderWarning.contains("not writable"), "and says why")
        expectEqual(after.recordingFolderChoice, chosen, "the previous choice stands")
    }

    /// An unplugged external drive must not fail a recording silently or
    /// create a stray /Volumes folder: fall back to the default, loudly.
    @MainActor
    static func testRecordingFolderFallsBackLoudly() {
        let missing = "/Volumes/CVP-unplugged-\(UUID().uuidString)/Recordings"
        var prefs = ShellPrefs()
        prefs.recordingFolder = missing
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        let folder = model.recordingTargetFolder()
        expect(folder != missing, "an unplugged drive is not recorded to")
        expect(folder.hasPrefix(NSTemporaryDirectory()), "the harness default folder is used instead")
        expect(model.recordingFolderWarning.contains("is not connected"),
               "Settings says the drive is not connected")
        expect(model.recordingFolderWarning.contains(missing), "and names the folder")
        expect(model.warnings.first?.hasPrefix("recording: Recording folder unavailable") == true,
               "the warning strip says so too")
        expect(!FileManager.default.fileExists(atPath: missing), "nothing is created under /Volumes")
        expectEqual(model.recordingFolderChoice, missing, "the choice is kept for when the drive returns")
    }

    /// Review I4: the footer Record flyout still printed ~/Movies/CoreVideoPro
    /// after the operator chose another folder, and after a fallback.
    @MainActor
    static func testRecordFlyoutShowsTheEffectiveFolder() {
        let chosen = scratchDir("flyout")
        let model = AppModel()
        model.setRecordingFolder(chosen)
        expectEqual(model.recordFlyoutFolder, chosen, "idle: the folder the next Record uses")
        model.recordingStatus = "recording"
        model.applySnapshotForTesting(["recording": ["status": "recording",
                                                     "targetFolder": "/fallback/folder"] as JSONObject])
        expectEqual(model.recordFlyoutFolder, "/fallback/folder",
                    "recording: the core's own target folder (a fallback included)")
        guard let footer = sourceFile("StudioFooter.swift") else {
            expect(false, "StudioFooter.swift not found next to the binary"); return
        }
        expect(footer.contains("Text(model.recordFlyoutFolder)"), "the flyout shows the effective folder")
        expect(!footer.contains("StudioFooterReadout.recordingFolder"), "the fixed ~/Movies readout is gone")
    }

    /// Review minor: access(2) can report writable before macOS grants a
    /// removable volume, so the folder is probed with a real file write.
    @MainActor
    static func testRecordingFolderIsProbedWithARealWrite() {
        let folder = scratchDir("probe")
        expectEqual(RecordingFolderPolicy.problem(with: folder), nil, "a writable folder passes the probe")
        let leftovers = (try? FileManager.default.contentsOfDirectory(atPath: folder)) ?? ["?"]
        expectEqual(leftovers, [], "the probe leaves no file behind")

        let real = RecordingFolderPolicy.writeProbe
        defer { RecordingFolderPolicy.writeProbe = real }
        RecordingFolderPolicy.writeProbe = { _ in "macOS did not allow a test file to be written (denied)" }
        let model = AppModel()
        expect(!model.setRecordingFolder(scratchDir("tcc")),
               "a folder macOS refuses at the probe is refused when chosen")
        expect(model.recordingFolderWarning.contains("did not allow a test file"), "and says why")
        model.recordingFolderChoice = folder
        let used = model.recordingTargetFolder()
        expect(used != folder, "at Record, a refused probe falls back to the default")
        expect(model.recordingFolderWarning.contains("did not allow a test file"), "loudly")
    }

    // ── 0.15 Zoom tab status reflects SDK readiness ─────────────────────────

    /// The Zoom tab said "Ready to join Zoom" whatever the SDK's state; a
    /// missing engine or failed SDK showed only on Health.
    @MainActor
    static func testZoomStatusReadsTheCoresSdkReadiness() {
        // The core's exact no-engine node (CoreStateCapture::zoomReadinessState).
        let noEngine: JSONObject = ["zoom": ["readiness": [
            "status": "blocked", "mode": "no-engine", "sdkAvailable": false,
            "checks": [["id": "zoom-engine-runtime", "status": "blocked",
                        "label": "COREVIDEO_ZOOM_ENGINE_PATH is not set: no Zoom engine, so Join is refused"]],
        ] as JSONObject] as JSONObject]
        let ready: JSONObject = ["zoom": ["readiness": ["status": "ready", "mode": "runtime"] as JSONObject]
            as JSONObject]
        let blocked = ZoomOperatorStatus.readiness(snapshot: noEngine)
        expectEqual(blocked, .blocked, "the core's blocked readiness is read")
        expectEqual(ZoomOperatorStatus.chipLabel(blocked, inMeeting: false), "Zoom is unavailable",
                    "Windows wording when blocked")
        expectEqual(ZoomOperatorStatus.guidance(blocked, inMeeting: false),
                    "CoreVideo cannot join Zoom yet. Open Health for details.", "and where to look")
        expect(ZoomOperatorStatus.blockingReason(snapshot: noEngine)?.contains("no Zoom engine") == true,
               "the blocking check is named on the Zoom tab")
        let fine = ZoomOperatorStatus.readiness(snapshot: ready)
        expectEqual(ZoomOperatorStatus.chipLabel(fine, inMeeting: false), "Ready to join Zoom", "ready")
        expectEqual(ZoomOperatorStatus.chipLabel(blocked, inMeeting: true), "Connected to Zoom",
                    "Windows wording in a meeting")
        expectEqual(ZoomOperatorStatus.readiness(snapshot: [:]), .ready,
                    "no snapshot yet is not evidence of a block")
        guard let shell = sourceFile("ShellApp.swift") else {
            expect(false, "ShellApp.swift not found next to the binary"); return
        }
        expect(shell.contains("ZoomOperatorStatus.chipLabel(readiness, inMeeting: inMeeting)"),
               "the Zoom tab's status chip comes from the core's readiness")
        expect(!shell.contains(": \"Ready to join Zoom\")"), "no fixed \"Ready to join Zoom\" chip")
    }

    // ── 0.6 manual lower third ───────────────────────────────────────────────

    /// A rendered Program frame carrying one Zoom guest (the core's
    /// programFrame.videoSources shape, RenderedProgramSources.h).
    @MainActor
    static func seedProgramSource(_ model: AppModel, pid: String = "42", name: String = "Guest") {
        model.programSceneId = "s-prog"
        model.roster = [RosterParticipant(id: pid, name: name, hasVideo: true,
                                          muted: false, talking: false, assigned: true)]
        model.lastSnapshot = ["programFrame": [
            "frameNumber": 120, "sceneId": "s-prog", "renderPlanId": "s-prog:1:0",
            "videoSources": [["kind": "participant-video", "layerId": "route:s-prog-0",
                              "participantId": pid, "sourceId": "zoom:" + pid]],
        ] as JSONObject]
    }

    /// The video kinds and id comparison of the core's "bind lower thirds to
    /// rendered Program" gate, read from MediaCore.cpp (like the payload
    /// contract test reads JsonRpcServer.cpp).
    static func lowerThirdGateFromCoreSource() -> (kinds: Set<String>, comparesIds: Bool)? {
        var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        var source: String?
        for _ in 0..<8 {
            let candidate = dir.appendingPathComponent("native/src/core/MediaCore.cpp")
            if let text = try? String(contentsOf: candidate, encoding: .utf8) { source = text; break }
            dir = dir.deletingLastPathComponent()
        }
        guard let text = source,
              let start = text.range(of: "isLowerThird && !asset->sourceId.empty()") else { return nil }
        let window = String(text[start.lowerBound...].prefix(1500))
        guard let close = window.range(of: "continue;") else { return nil }
        let gate = String(window[..<close.lowerBound])
        var kinds: Set<String> = []
        var rest = gate[...]
        while let hit = rest.range(of: "video.kind == \"") {
            let after = rest[hit.upperBound...]
            guard let end = after.firstIndex(of: "\"") else { break }
            kinds.insert(String(after[..<end]))
            rest = after[end...]
        }
        let compares = gate.contains("video.sourceId == asset->sourceId")
            && gate.contains("video.participantId == asset->sourceId")
        return (kinds, compares)
    }

    /// The Mac keyed sourceId "mac:manual", which no Program layer ever has, so
    /// the core's on-Program gate dropped the key while the tab said on air.
    @MainActor
    static func testManualLowerThirdKeysAProgramSourceTheCoreGateAdmits() {
        guard let gate = lowerThirdGateFromCoreSource() else {
            expect(false, "the core's lower-third gate (MediaCore.cpp) was not found — re-read the contract")
            return
        }
        expect(gate.comparesIds, "the gate admits a key whose sourceId is a Program layer's sourceId/participantId")
        expect(ProgramLowerThirdSource.subjectKinds.isSubset(of: gate.kinds),
               "every kind the shell can key is one the gate admits (gate: \(gate.kinds.sorted()))")

        let model = AppModel()
        seedProgramSource(model, pid: "42", name: "Jordan")
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Jordan Wallace")
        var sent: [JSONObject] = []
        model.overlayCommandObserver = { sent.append($0) }
        model.showLowerThird()
        let key = sent.first { $0["keyPhase"] as? String == "building-in" }
        let keyed = key?["sourceId"] as? String ?? ""
        expect(keyed != "mac:manual", "the key never carries the synthetic mac:manual id")
        let programIds = ((model.lastSnapshot["programFrame"] as? JSONObject)?["videoSources"]
                          as? [JSONObject] ?? []).compactMap { $0["sourceId"] as? String }
        expect(programIds.contains(keyed), "the key names a source on rendered Program (\(keyed))")
        expectEqual(key?["sourceName"] as? String, "Jordan Wallace", "it keys the Sources display name")
        expectEqual(key?["text"] as? String, "Jordan Wallace", "as the keyed text")

        // Nothing on Program: refuse with the Windows wording, send nothing.
        let empty = AppModel()
        var none: [JSONObject] = []
        empty.overlayCommandObserver = { none.append($0) }
        empty.showLowerThird()
        expect(none.isEmpty, "no source on Program sends no key")
        expectEqual(empty.lowerThirdCommandStatus, "Lower third needs a program source", "and says so")

        // Key out keeps the keyed id, so the build-out animates instead of cutting.
        model.hideLowerThird()
        let out = sent.first { $0["keyPhase"] as? String == "building-out" }
        expectEqual(out?["sourceId"] as? String, keyed, "the build-out carries the keyed source id")
    }

    /// "Key on air" came from the requested phase; it must come from the
    /// core's overlayState visible flag (the gate can suppress a key).
    @MainActor
    static func testLowerThirdStatusComesFromTheCoresOverlayState() {
        func overlay(_ phase: String, visible: Bool) -> JSONObject {
            ["overlayState": ["onAirCount": 1, "overlays": [[
                "overlayId": "key:lower-third", "keyPhase": phase, "visible": visible,
                "sourceId": "zoom:42", "sourceName": "Jordan Wallace"]]] as JSONObject]
        }
        let model = AppModel()
        model.applySnapshotForTesting(overlay("on-air", visible: false))
        expectEqual(model.lowerThirdOnAirCount, 0, "a suppressed key is not counted on air")
        expectEqual(model.lowerThirdKeyStatus, LowerThirdKeyReadout.syncPending,
                    "a requested-but-undrawn key says the core has not confirmed it")
        model.applySnapshotForTesting(overlay("on-air", visible: true))
        expectEqual(model.lowerThirdOnAirCount, 1, "a drawn key is on air")
        expectEqual(model.lowerThirdKeyStatus, "Key on air — Jordan Wallace", "and names its source")
        model.applySnapshotForTesting(["overlayState": ["overlays": []] as JSONObject])
        expectEqual(model.lowerThirdKeyStatus, "Key off — no source keyed", "no key: off")
        guard let overlays = sourceFile("Overlays.swift") else {
            expect(false, "Overlays.swift not found next to the binary"); return
        }
        expect(overlays.contains("Text(model.lowerThirdKeyStatus)"), "the keyer status line reads the core's state")
        expect(!overlays.contains("Text(onAir ? \"Key on air\""), "no request-derived \"Key on air\"")
    }

    /// Port of Windows RenderedLowerThirdSourceResolver: rendered layers only,
    /// incoming side of a take, camera-on guests, sticky first.
    @MainActor
    static func testLowerThirdResolverIsTheWindowsRule() {
        func frame(_ sources: [JSONObject], plan: String = "p:1:0", scene: String = "s") -> JSONObject {
            ["frameNumber": 9, "sceneId": scene, "renderPlanId": plan, "videoSources": sources]
        }
        let a: JSONObject = ["kind": "participant-video", "layerId": "route:a", "participantId": "1", "sourceId": "zoom:1"]
        let b: JSONObject = ["kind": "participant-video", "layerId": "take-in:b", "participantId": "2", "sourceId": "zoom:2"]
        let cam: JSONObject = ["kind": "participant-video", "layerId": "route:c", "participantId": "", "sourceId": "capture:cam"]
        func pick(_ f: JSONObject, sticky: String = "", video: Set<String> = ["1", "2"],
                  devices: Set<String> = ["cam"], scene: String = "s") -> String? {
            ProgramLowerThirdSource.resolve(programFrame: f, programSceneId: scene, stickySourceId: sticky,
                                            rosterWithVideo: video, captureDeviceIds: devices)?.sourceId
        }
        expectEqual(pick(frame([a, b])), "zoom:1", "first rendered guest")
        expectEqual(pick(frame([a, b]), sticky: "zoom:2"), "zoom:2", "sticky wins while still on Program")
        expectEqual(pick(frame([a, b], plan: "take:x")), "zoom:2", "a transition keys the incoming side")
        expectEqual(pick(frame([a]), video: []), nil, "a camera-off guest is not a subject")
        expectEqual(pick(frame([cam])), "capture:cam", "a connected camera is a subject")
        expectEqual(pick(frame([cam]), devices: []), nil, "an unknown capture device is not")
        expectEqual(pick(frame([a], scene: "other")), nil, "a frame from another scene is not trusted")
    }

    // ── 0.12 monitor output device ───────────────────────────────────────────

    /// A fake core that speaks the revisioned monitor contract: every reply
    /// carries the applied monitor state, and an edit is applied as sent.
    final class MonitorFakeCore: @unchecked Sendable {
        private let lock = NSLock()
        private var _commands: [JSONObject] = []
        private var revision = 0
        private var applied: JSONObject = ["monitorEnabled": false, "monitorVolume": 0.7,
                                           "monitorDeviceId": "",
                                           "monitorDeviceName": "System default output"]
        private var lastResult: JSONObject?
        var commands: [JSONObject] { lock.lock(); defer { lock.unlock() }; return _commands }
        func handle(_ body: JSONObject) async throws -> JSONObject { apply(body) }
        private func apply(_ body: JSONObject) -> JSONObject {
            lock.lock(); defer { lock.unlock() }
            for command in body["commands"] as? [JSONObject] ?? [] {
                _commands.append(command)
                guard command["type"] as? String == "set-audio-monitor-control" else { continue }
                revision += 1
                applied = ["monitorEnabled": command["enabled"] ?? false,
                           "monitorVolume": command["volume"] ?? 0.7,
                           "monitorDeviceId": command["deviceId"] ?? "",
                           "monitorDeviceName": command["deviceName"] ?? ""]
                lastResult = ["operationId": command["operationId"] ?? "", "status": "applied"]
            }
            var mix = applied
            var control: JSONObject = ["authorityEpoch": "core-1", "revision": revision]
            if let lastResult { control["lastResult"] = lastResult }
            mix["monitorControl"] = control
            return ["snapshot": ["audioMixSession": mix]]
        }
        func monitorEdits() -> [JSONObject] {
            commands.filter { $0["type"] as? String == "set-audio-monitor-control" }
        }
    }

    static let desk = MonitorOutputDevice(id: "BuiltInSpeakerDevice", name: "MacBook Pro Speakers")
    static let phones = MonitorOutputDevice(id: "AppleUSBAudioEngine:Focusrite:1", name: "Scarlett 2i2")

    /// Monitoring followed the macOS default output: headphones on an
    /// interface meant moving every app's audio.
    @MainActor
    static func testMonitorDeviceIsChosenPersistedAndSent() {
        let before = AppModel()
        before.applyMonitorOutputDevices([desk, phones])
        before.setMonitorDevice(phones.id)
        guard let after = relaunched(before) else { expect(false, "relaunch"); return }
        expectEqual(after.monitorDeviceChoice, phones.id, "the device UID survives a relaunch")
        expectEqual(after.monitorDeviceChoiceName, "Scarlett 2i2", "and its name")

        let core = MonitorFakeCore()
        after.quitRequestForTesting = { body, _ in try await core.handle(body) }
        after.applyMonitorOutputDevices([desk, phones])
        after.setMonitor(enabled: true, volume: 0.5)
        spin(until: { !core.monitorEdits().isEmpty }, limit: 3)
        let edit = core.monitorEdits().last
        expectEqual(edit?["deviceId"] as? String, phones.id, "the core is told the chosen device UID")
        expectEqual(edit?["deviceName"] as? String, "Scarlett 2i2", "with its name")
        expectEqual(after.monitorTargetStatus, "Monitor target - Scarlett 2i2", "Windows status text")
        after.setMonitor(enabled: false, volume: 0.5)
        expectEqual(after.monitorTargetStatus, "Monitor muted", "Windows muted text")
    }

    /// An unplugged interface must not leave the operator deaf and unaware:
    /// fall back to the default output, say so, and restore on re-plug.
    @MainActor
    static func testMonitorDeviceFallsBackLoudlyAndReturns() {
        let core = MonitorFakeCore()
        let model = AppModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        var prefs = ShellPrefs()
        prefs.monitorEnabled = true
        prefs.monitorDeviceId = phones.id
        prefs.monitorDeviceName = phones.name
        model.applyRestoredPrefs(prefs)
        model.applyMonitorOutputDevices([desk])   // the interface is unplugged
        model.setMonitor(enabled: true, volume: 0.7)
        spin(until: { !core.monitorEdits().isEmpty }, limit: 3)
        expectEqual(core.monitorEdits().last?["deviceId"] as? String, "",
                    "an unplugged device falls back to the system default output")
        expect(model.monitorDeviceWarning.contains("Scarlett 2i2 is not connected"),
               "Settings names the missing device")
        expect(model.warnings.contains { $0.hasPrefix("audio: Monitor device Scarlett 2i2") },
               "the warning strip says so")
        expectEqual(model.monitorDeviceChoice, phones.id, "the choice is kept for the re-plug")
        expectEqual(model.currentPrefs().monitorDeviceId, phones.id, "and still saved")

        // Re-plug: the device list changes and the monitor moves back on its own.
        let sent = core.monitorEdits().count
        model.applyMonitorOutputDevices([desk, phones])
        spin(until: { core.monitorEdits().count > sent }, limit: 3)
        expectEqual(core.monitorEdits().last?["deviceId"] as? String, phones.id,
                    "plugging it back in re-sends the chosen device")
        expectEqual(model.monitorDeviceWarning, "", "and clears the warning")
    }

    /// A respawned core opens the system default output: the device must be
    /// re-sent on every generation, not once.
    @MainActor
    static func testMonitorDeviceRidesEveryCoreGeneration() {
        guard let body = functionBody(named: "func onConnected()", in: "AppModel.swift") else {
            expect(false, "AppModel.swift onConnected() not found next to the binary")
            return
        }
        // A statement of its own (the hot-plug watcher's closure also names it).
        let refresh = body.range(of: "\n        refreshMonitorOutputDevices()\n")
        let send = body.range(of: "setMonitor(enabled: monitorEnabled, volume: monitorVolume)")
        expect(refresh != nil && send != nil && refresh!.lowerBound < send!.lowerBound,
               "onConnected re-lists the devices, then re-sends the monitor")
        guard let setMonitor = functionBody(named: "func setMonitor(enabled: Bool, volume: Double)",
                                            in: "AppModel.swift") else {
            expect(false, "setMonitor not found"); return
        }
        expect(setMonitor.contains("monitorDeviceTarget()"),
               "every monitor send carries the resolved device")
        expect(!setMonitor.contains("\"deviceId\": \"\""), "no hard-coded system-default device id")
        guard let projection = sourceFile("MonitorControlProjection.swift") else {
            expect(false, "MonitorControlProjection.swift not found"); return
        }
        expect(!projection.contains("\"deviceId\": \"\""), "the projection sends the draft's device")
    }

    @MainActor
    static func testMonitorProjectionCarriesTheDevice() {
        var projection = MonitorControlProjection()
        func mix(_ revision: Int, device: String) -> JSONObject {
            ["monitorEnabled": true, "monitorVolume": 0.5, "monitorDeviceId": device,
             "monitorControl": ["authorityEpoch": "core-1", "revision": revision] as JSONObject]
        }
        projection.observe(mix(0, device: ""))
        projection.edit(.init(enabled: true, volume: 0.5, deviceId: phones.id, deviceName: phones.name))
        let command = projection.nextCommand()
        expectEqual(command?["deviceId"] as? String, phones.id, "a device-only edit is sent")
        expectEqual(command?["deviceName"] as? String, phones.name, "with the device name")
        var settled = MonitorControlProjection()
        settled.observe(mix(0, device: phones.id))
        settled.edit(.init(enabled: true, volume: 0.5, deviceId: phones.id, deviceName: "other words"))
        settled.observe(mix(0, device: phones.id))
        expect(settled.draft == nil, "a matching device echo settles the draft (name is display only)")
    }

    // ── 0.13 "Join and end my other Zoom session" ────────────────────────────

    static let accountBusySentence =
        "Your Zoom account is already in this meeting, for example in the Zoom app. "
        + "Leave it there and join again, or choose \"Join and end my other Zoom session\"."

    /// Windows ZoomJoinReconciliationTests: offered for the collision only.
    @MainActor
    static func testEndOtherSessionIsOfferedOnlyOnTheCollision() {
        let busy: JSONObject = ["meetingState": "error",
                                "warnings": [accountBusySentence, "Zoom meeting failed: 1"]]
        expect(ZoomJoinStatus.canEndOtherSessionAndRetry(busy), "the collision offers the takeover")
        expectEqual(ZoomJoinStatus.failure(from: busy), accountBusySentence,
                    "the actionable sentence is what the pane shows, even with a later warning")
        let other: JSONObject = ["meetingState": "error", "warnings": ["Zoom meeting failed: 504"]]
        expect(!ZoomJoinStatus.canEndOtherSessionAndRetry(other), "any other failure does not")
        let joined: JSONObject = ["meetingState": "in_meeting", "warnings": [accountBusySentence]]
        expect(!ZoomJoinStatus.canEndOtherSessionAndRetry(joined), "a joined meeting does not")

        var tracker = ZoomJoinStatusTracker()
        tracker.beginJoin(signedIn: true)
        tracker.joinAnswered(busy)
        expect(tracker.canEndOtherSession, "the failed join offers the takeover")
        tracker.beginJoin(signedIn: true)
        expect(!tracker.canEndOtherSession, "every new join clears the offer")
        tracker.joinAnswered(["meetingState": "error", "warnings": ["Timed out waiting for Zoom meeting join result."]])
        expect(!tracker.canEndOtherSession, "a timeout is not the collision")
        tracker.observe(busy)
        expect(tracker.canEndOtherSession, "the core's late verdict can still offer it")
        tracker.observe(joined)
        expect(!tracker.canEndOtherSession, "joining withdraws it")
    }

    /// The Mac could not recover when the operator's own Zoom app was in the
    /// meeting: no button, and no way to ask the core to end the other session.
    @MainActor
    static func testEndOtherSessionJoinIsExplicitAndNeverSticky() {
        let core = FakeTakeCore()
        let model = AppModel()
        var answers: [JSONObject] = [
            ["meetingState": "error", "warnings": [accountBusySentence]],
            ["meetingState": "in_meeting", "warnings": [] as [String]],
            ["meetingState": "in_meeting", "warnings": [] as [String]],
        ]
        model.quitRequestForTesting = { body, _ in
            _ = try await core.handle(body)
            return await MainActor.run { ["snapshot": answers.isEmpty ? [:] : answers.removeFirst()] }
        }
        func joins() -> [JSONObject] {
            core.bodies.filter { $0["type"] as? String == "zoom-join" }
                .compactMap { $0["payload"] as? JSONObject }
        }
        model.joinMeetingId = "97682593786"
        model.joinZoom()
        spin(until: { model.canEndOtherZoomSession }, limit: 3)
        expectEqual(joins().count, 1, "one ordinary join")
        expect(joins().first?["endOtherMeeting"] == nil, "an ordinary join never carries the takeover")
        expect(model.canEndOtherZoomSession, "the collision offers the takeover button")
        expectEqual(model.zoomJoinStatus, accountBusySentence, "the pane says why and what to do")

        model.joinZoom(endOtherMeeting: true)
        spin(until: { joins().count == 2 && !model.canEndOtherZoomSession }, limit: 3)
        expectEqual(joins().last?["endOtherMeeting"] as? Bool, true, "the button's join asks to end the other session")
        expect(!model.canEndOtherZoomSession, "the offer is withdrawn")

        spin(until: { false }, limit: 0.3)   // let the takeover join finish
        model.joinZoom()
        spin(until: { joins().count == 3 }, limit: 3)
        expect(joins().last?["endOtherMeeting"] == nil, "the takeover is never remembered for the next join")

        guard let shell = sourceFile("ShellApp.swift") else { expect(false, "ShellApp.swift"); return }
        expect(shell.contains("if model.canEndOtherZoomSession {\n"
                              + "                            Button(\"Join and end my other Zoom session\") {\n"
                              + "                                model.joinZoom(endOtherMeeting: true)"),
               "the Windows button, shown only while the takeover is offered")
    }

    // ── 0.14 Studio scene editing ────────────────────────────────────────────

    /// Mac "Edit layout" changed the multiview layout mode; Windows opens an
    /// in-place editor for the PREVIEW scene.
    @MainActor
    static func testStudioEditLayoutEditsThePreviewSceneInPlace() {
        let model = sceneEditModel()   // Program s2, Preview s1
        model.scenes[0].layers = []    // Preview follows its preset
        let programBefore = model.buildRoutes(for: "s2", isProgram: true).map { NSDictionary(dictionary: $0) }
        model.toggleStudioLayoutEditing()
        expect(model.studioLayoutEditing, "Edit layout opens the in-place editor")
        expect(!model.scenes[0].layers.isEmpty, "the Preview scene's preset is seeded as editable layers")
        let layer = model.scenes[0].layers[0]
        model.updateLayer(sceneId: "s1", layerId: layer.id) { $0.x = 0.3 }
        let programAfter = model.buildRoutes(for: "s2", isProgram: true).map { NSDictionary(dictionary: $0) }
        expectEqual(programAfter, programBefore, "Program changes exclusively on Take")
        model.toggleStudioLayoutEditing()
        expect(!model.studioLayoutEditing, "the toggle closes it (Done does the same)")

        // Preview on Program (or empty) is no edit target: refused, said why.
        model.previewSceneId = "s2"
        model.toggleStudioLayoutEditing()
        expect(!model.studioLayoutEditing, "Program is never edited in place")
        expectEqual(model.studioCommandStatus, AppModel.noPreviewSceneMessage, "and the operator is told why")

        guard let shell = sourceFile("ShellApp.swift"),
              let canvas = functionBody(named: "struct MultiviewerCanvas: View {", in: "ShellApp.swift") else {
            expect(false, "ShellApp.swift"); return
        }
        expect(canvas.contains("ActionChip(\"Edit layout\"") && canvas.contains("model.toggleStudioLayoutEditing()"),
               "the Studio header's Edit layout opens the scene editor")
        expect(!canvas.contains("configureMultiviewer(mode:"),
               "the header no longer changes the multiview layout (Settings > Multiview does)")
        expect(canvas.contains("StudioLayoutEditorOverlay()"), "the editor sits over the multiview")
        expect(shell.contains("AddOverlayMenu()"), "Add overlay is in the header")
    }

    /// Windows POS-2: no Add overlay on the Mac.
    @MainActor
    static func testStudioAddOverlayPlacesATopMostPreviewLayer() {
        // Windows OverlayLayerService rect math.
        let tr = OverlayPlacementPolicy.rect("top-right", sourceAspect: nil)
        expect(abs(tr.x - 0.80) < 1e-9 && abs(tr.y - 0.05) < 1e-9 && abs(tr.w - 0.15) < 1e-9
               && abs(tr.h - 0.15) < 1e-9, "top-right: 15% wide, 16:9, inside the 5% safe area")
        let square = OverlayPlacementPolicy.rect("bottom-left", sourceAspect: 1)
        expect(abs(square.h - 0.15 * 16 / 9) < 1e-9 && abs(square.y - (0.95 - square.h)) < 1e-9,
               "the box is aspect-locked to the source")
        let banner = OverlayPlacementPolicy.rect("center", sourceAspect: 8)
        expect(abs(banner.h - 0.08) < 1e-9, "a wide banner keeps the minimum height")
        let full = OverlayPlacementPolicy.rect("full-canvas", sourceAspect: 2)
        expect(full.x == 0 && full.y == 0 && full.w == 1 && full.h == 1, "full canvas is the whole frame")
        expectEqual(OverlayPlacementPolicy.options.map(\.label),
                    ["Full canvas (transparent overlay)", "Top left", "Top right", "Bottom left",
                     "Bottom right", "Center", "Free (drag to place)"], "Windows placement menu")

        let model = sceneEditModel()   // Program s2, Preview s1 (one layer)
        model.mediaAssets = [MediaAsset(id: "logo", name: "Logo", kind: "image", relativePath: "logo.png",
                                        filePath: "/tmp/logo.png", naturalWidth: 400, naturalHeight: 200)]
        let programBefore = model.buildRoutes(for: "s2", isProgram: true).map { NSDictionary(dictionary: $0) }
        expect(model.addOverlayLayer(placement: "top-right", source: .media("logo")) == nil, "overlay added")
        let top = model.scenes[0].layers.last
        expectEqual(top?.mediaAssetId, "logo", "the overlay is the Preview scene's top-most layer")
        expect(abs((top?.height ?? 0) - 0.15 * 16 / 9 / 2) < 1e-9, "sized from the still's aspect")
        let route = model.buildRoutes(for: "s1").last
        expectEqual(route?["mediaAssetId"] as? String, "logo", "the preview bus composites the still on top")
        expectEqual(route?["mediaAssetPath"] as? String, "/tmp/logo.png", "with its path")
        expectEqual(model.buildRoutes(for: "s2", isProgram: true).map { NSDictionary(dictionary: $0) },
                    programBefore, "Program is untouched until Take")
        expectEqual(model.studioCommandStatus,
                    "Logo overlay added to Intro (Top right) - drag or use Edit layout to fine-tune",
                    "Windows status text")
        guard let after = relaunched(model) else { expect(false, "relaunch"); return }
        expectEqual(after.scenes.first { $0.id == "s1" }?.layers.last?.mediaAssetId, "logo",
                    "the overlay survives a relaunch")

        model.previewSceneId = "s2"   // Preview on Program: no target
        expect(model.addOverlayLayer(placement: "center", source: .activeSpeaker) != nil,
               "no overlay is ever added to the on-air scene")
    }

    /// Rename/duplicate/remove worked only on the cued scene in the Scenes tab.
    @MainActor
    static func testSceneRailRowActionsTargetTheRowWithoutCueing() {
        let model = sceneEditModel()   // Program s2, Preview s1; s3 idle
        let syncs = model.sceneSyncRequestCount
        expect(model.renameScene("s3", to: "Outro") == nil, "an idle row renames by its id")
        expectEqual(model.previewSceneId, "s1", "without cueing it")
        expectEqual(model.sceneSyncRequestCount, syncs, "a rename never reaches the wire")
        expect(model.railCanRemoveScene("s3"), "an idle row offers Remove scene")
        expect(!model.railCanRemoveScene("s2"), "the on-air row does not (#1070)")
        model.duplicateScene("s3")
        expectEqual(model.scenes.last?.name, "Outro copy", "Duplicate copies that row")
        expectEqual(model.previewSceneId, model.scenes.last?.id, "and cues the copy on Preview")
        expectEqual(model.programSceneId, "s2", "Program is untouched")
        expectEqual(model.studioCommandStatus, "Outro copy duplicated from Outro and cued on preview",
                    "Windows status text")

        guard let row = functionBody(named: "struct SceneRow: View {", in: "ShellApp.swift") else {
            expect(false, "SceneRow"); return
        }
        expect(row.contains("Button(\"Rename scene\", action: rename)"), "the row's context menu renames it")
        expect(row.contains("if model.railCanRemoveScene(scene.id) {")
               && row.contains("Button(\"Remove scene\") { model.deleteScene(scene.id) }"),
               "and removes it where the rules allow")
        expect(row.contains("model.duplicateScene(scene.id)") && row.contains(".help(\"Duplicate scene\")"),
               "a Duplicate button sits beside every row")
    }

    /// No drag-swap on the Mac multiview.
    @MainActor
    static func testMultiviewDragSwapsTheTwoInputSlots() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest")
        model.assignSlot(2, kind: "capture", sourceId: "cam1", name: "Wide cam")
        model.slots[1].audioDeviceId = "mic-1"
        model.slots[1].keyEnabled = true
        model.swapMultiviewSlots(1, 2)
        expectEqual(model.slots.map(\.id), Array(1...10), "slot numbers stay in place")
        expectEqual(model.slots[0].sourceId, "cam1", "input 1 now carries the camera")
        expectEqual(model.slots[1].sourceId, "42", "input 2 now carries the guest")
        expectEqual(model.slots[0].audioDeviceId, "mic-1", "the camera keeps its microphone")
        expect(model.slots[0].keyEnabled, "and its key")
        expectEqual(model.slots[1].name, "Guest", "names follow their source")
        expect(model.slots[0].inShow && model.slots[1].inShow, "both stay in the show (nothing cleared)")
        expectEqual(model.studioCommandStatus, "Inputs 1 and 2 swapped on the multiviewer.", "Windows status text")

        func tile(_ id: String, _ role: String, _ x: Double, _ y: Double) -> MultiviewTile {
            MultiviewTile(id: id, label: id, role: role, tally: "none", activeSpeaker: false,
                          x: x, y: y, w: 0.25, h: 0.25)
        }
        let tiles = [tile("pvw", "pvw", 0, 0), tile("zoom:42", "source", 0, 0.5),
                     tile("capture:cam1", "source", 0.25, 0.5)]
        expectEqual(MultiviewDragPolicy.dropTarget(tiles: tiles, from: tiles[1],
                                                   at: CGPoint(x: 0.3, y: 0.6))?.id, "capture:cam1",
                    "a release over another source tile is a swap")
        expect(MultiviewDragPolicy.dropTarget(tiles: tiles, from: tiles[1], at: CGPoint(x: 0.1, y: 0.6)) == nil,
               "a release over its own tile is not")
        expect(MultiviewDragPolicy.dropTarget(tiles: tiles, from: tiles[1], at: CGPoint(x: 0.1, y: 0.1)) == nil,
               "nor over a bus cell")
        guard let canvas = functionBody(named: "struct MultiviewerCanvas: View {", in: "ShellApp.swift") else {
            expect(false, "MultiviewerCanvas"); return
        }
        expect(canvas.contains("MultiviewDragPolicy.dropTarget(") && canvas.contains("model.swapMultiviewSlots(fromSlot, toSlot)"),
               "the multiview tiles drag-swap")
    }

    // ── fix round 1 ──────────────────────────────────────────────────────────

    /// Review C1: a multiview solo cue must keep its SOURCE across a drag-swap
    /// (Windows BuildSoloRoute binds the participant/device, not the slot),
    /// while a slot-bound preset Program DOES follow the swap (owner ruling:
    /// Windows parity for slot-bound routes).
    @MainActor
    static func testDragSwapNeverMovesAnOnAirSoloCue() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest")
        model.assignSlot(2, kind: "capture", sourceId: "cam1", name: "Wide cam")
        model.previewMultiviewSlot(1)                 // cue slot 1 solo...
        let onAir = model.previewSceneId
        model.programSceneId = onAir                  // ...and it is on Program
        model.previewMultiviewSlot(2)                 // Preview: a solo of slot 2
        let cued = model.previewSceneId
        func wire(_ sceneId: String) -> [NSDictionary] {
            model.buildRoutes(for: sceneId, isProgram: sceneId == onAir).map { NSDictionary(dictionary: $0) }
        }
        let programBefore = wire(onAir), previewBefore = wire(cued)
        expectEqual(programBefore.first?["participantId"] as? String, "42", "Program shows the guest solo")
        model.swapMultiviewSlots(1, 2)
        expectEqual(wire(onAir), programBefore, "a swap never changes the on-air solo cue")
        expectEqual(wire(cued), previewBefore, "nor the cued one")
        expectEqual(model.busLabel(for: onAir), "Guest", "the PGM chip still names the guest")

        // Parity, documented: a preset Program bound to slot order re-resolves.
        let preset = AppModel()
        preset.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest")
        preset.assignSlot(2, kind: "capture", sourceId: "cam1", name: "Wide cam")
        preset.scenes = [SceneDef(id: "p", name: "Panel", layout: "two-up"),
                         SceneDef(id: "q", name: "Other", layout: "single")]
        preset.programSceneId = "p"
        let first = preset.buildRoutes(for: "p", isProgram: true).first?["participantId"] as? String
        preset.swapMultiviewSlots(1, 2)
        let after = preset.buildRoutes(for: "p", isProgram: true).first
        expectEqual(first, "42", "the preset's first cell was the guest")
        expectEqual(after?["captureDeviceId"] as? String, "cam1",
                    "a slot-bound preset follows the swap (Windows parity, owner ruling)")
    }

    /// Review I1: removing the Preview scene emptied Preview, so no
    /// set-preview-scene was sent and PVW kept the deleted scene.
    @MainActor
    static func testRemovingThePreviewSceneResyncsThePreviewBus() {
        let core = FakeTakeCore()
        let model = sceneEditModel()   // Program s2, Preview s1
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        expect(model.deleteScene("s1") == nil, "the Preview scene is removed")
        expectEqual(model.previewSceneId, "s2", "Preview moves to Program (Windows PreviewAfterRemoval)")
        expect(!model.canTake, "PVW == PGM: nothing to Take")
        spin(until: { !core.bodies.isEmpty }, limit: 3)
        let commands = (core.bodies.last?["commands"] as? [JSONObject]) ?? []
        let preview = commands.first { $0["type"] as? String == "set-preview-scene" }
        expectEqual(preview?["sceneId"] as? String, "s2", "the PVW bus is re-synced to a real scene")
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
