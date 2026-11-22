// macOS Sources, Phase 2 (docs/superpowers/plans/2026-11-21-mac-sources-phase2.md).
// Its own file so the parallel Mac branches extending ShellTests.swift do not collide.

import AppKit
import Combine
import Foundation
import SwiftUI

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var sourcesPhase2Cases: [(String, () -> Void)] {
        [
            // Task 1: polish.
            ("sources2-polish/media-status-not-repeated", testTheMediaStatusLineIsNotARepeatOfTheSummary),
            // Task 2: R1 accept rules and frame age.
            ("sources2-r1/unrevisioned-roster-is-no-fact", testAnUnrevisionedRosterIsNoRowFact),
            ("sources2-r1/stale-revision-ignored", testAStaleRosterRevisionChangesNoRow),
            ("sources2-r1/epoch-change-resets", testANewMeetingEpochResetsEveryZoomRow),
            ("sources2-r1/retired-epoch-ignored-after-core-restart", testARetiredEpochIsIgnoredAfterACoreRestart),
            ("sources2-r1/stale-format-ages-out", testAStaleFormatShowsItsAgeAndNoRate),
            // Task 3: R2 cap note and REC chip.
            ("sources2-r2/cap-note-table", testTheCapNoteFollowsTheWindowsRuleForEveryCombination),
            ("sources2-r2/cap-from-core-not-preference", testTheCapNoteReadsTheCoresAppliedCap),
            ("sources2-r2/rec-chip-four-states-wired", testTheRecChipShowsTheFourWriterStatesFromSnapshots),
            // Task 4: S4 media slots.
            ("sources2-s4/multiview-payload-media", testAMediaSlotFeedsTheMultiviewAsMedia),
            ("sources2-s4/routes-carry-media-identity-every-builder", testEveryRouteBuilderCarriesAMediaSlotsIdentity),
            ("sources2-s4/picker-media-group-order", testThePickerListsMediaAfterScreensWithoutAudioOnlyFiles),
            ("sources2-s4/persist-restore-media", testAMediaSlotIsSavedRestoredAndNeverSubstituted),
            ("sources2-s4/media-never-auto-assigned", testAutoAssignNeverPlacesOrMovesMedia),
            ("sources2-s4/media-row-facts", testAMediaRowIsLiveFromTheBusAndNeverStalled),
            // Task 6: PVW tally from the core.
            ("sources2-tally/pvw-from-core-not-routes", testThePreviewTallyIsWhatTheCoreRenderedNotTheShellsRoutes),
            // Task 8: R3 Health row.
            ("sources2-r3/health-evidence-rows", testHealthListsTheLastFiveSubscriptionEvidenceRows),
            // Task 10: S6 shell thumbnails.
            ("sources2-s6/thumbnail-identity", testARowShowsOnlyACellNamingItsOwnSource),
            ("sources2-s6/placeholder-reasons", testEachPlaceholderSaysWhyThereIsNoPicture),
            ("sources2-s6/sheet-parse", testTheSheetNodeParsesOnlyWhenActiveWithASurface),
            ("sources2-s6/demand-lease-follows-tab", testThePreviewDemandLeaseFollowsTheSourcesTab),
            ("sources2-s6/reassign-blanks-before-new-identity", testAReassignedRowBlanksUntilTheCoreNamesTheNewSource),
            ("sources2-s6/border-colours", testTheThumbnailBorderUsesTheWindowsTallyColours),
            ("sources2-s6/multiview-payload-carries-slot", testTheMultiviewPayloadCarriesTheZeroBasedSlot),
            ("sources2-s6/cell-geometry", testTheSheetLayerFrameShowsExactlyTheSlotsCell),
        ]
    }

    // ── Task 1: polish ──────────────────────────────────────────────────────

    /// D9. The orchestrator saw "Playing X on Program" twice on the Media tab: the
    /// selected-media summary and the status line said the same sentence.
    @MainActor
    static func testTheMediaStatusLineIsNotARepeatOfTheSummary() {
        expectEqual(MediaRoutePlayback.statusLine(status: "Playing Intro on Program",
                                                  summary: "Playing Intro on Program"), nil,
                    "a status identical to the summary is not drawn")
        expectEqual(MediaRoutePlayback.statusLine(status: "", summary: "Intro ready to cue"), nil,
                    "an empty status is not drawn")
        expectEqual(MediaRoutePlayback.statusLine(status: "Could not pause Intro; the core refused",
                                                  summary: "Playing Intro on Program"),
                    "Could not pause Intro; the core refused", "a different status is drawn")
        // The seen case: selecting an asset writes "<name> ready to cue or audition" under the
        // summary "<name> ready to cue" — the same sentence with a tail.
        expectEqual(MediaRoutePlayback.statusLine(status: "Show open ready to cue or audition",
                                                  summary: "Show open ready to cue"), nil,
                    "a status that only extends the summary is not drawn")
        expectEqual(MediaRoutePlayback.statusLine(status: "Removed 'Show open' (recoverable from the media .trash folder).",
                                                  summary: "No media asset selected"),
                    "Removed 'Show open' (recoverable from the media .trash folder).", "an unrelated status is drawn")
        // Through the model: the status the playback projection writes equals the summary.
        let model = AppModel()
        model.mediaStatus = "No media asset selected"
        expectEqual(MediaRoutePlayback.statusLine(status: model.mediaStatus, summary: model.selectedMediaSummary), nil,
                    "the model's own repeated pair is suppressed")
    }

    // ── Task 2: R1 ──────────────────────────────────────────────────────────

    /// A `zoom-source-format` event for participant 42 at `atMs` in `epoch`.
    static func formatEvent(_ pid: String = "42", epoch: String = "1:1:law", w: Int = 1920, h: Int = 1080,
                            fps: Int = 60, atMs: Double, generation: Int64 = 1) -> JSONObject {
        ["type": "zoom-source-format", "format": [
            "participantId": pid, "rosterEpoch": epoch, "sourceGeneration": NSNumber(value: generation),
            "width": w, "height": h, "fps": fps, "frameId": Int(atMs), "frameAtMs": atMs] as JSONObject]
    }

    /// A model with Zoom guest 42 in slot 1, live at 1920×1080@60 in epoch 1:1:law, revision 1.
    @MainActor
    static func liveGuestModel() -> AppModel {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest 42")
        model.applyZoomForTesting(lawRoster(["42"], revision: 1))
        model.handleEventForTesting(formatEvent(atMs: 100))
        return model
    }

    /// Windows MultiviewInputRowsProjection.ApplyRosterFact (:80-82): a roster fact needs
    /// rosterRevision > 0 and a non-empty epoch. Mac lets an "older core" roster through the
    /// slot law (ZoomRosterProjection.swift:34-39); it must not become a row fact. Fails on fc675541:
    /// the epoch-less roster is applied, and a format fact naming epoch "" then matches it.
    @MainActor
    static func testAnUnrevisionedRosterIsNoRowFact() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest 42")
        model.applyZoomForTesting(["meetingState": "in_meeting",
                                   "participants": [["userId": "42", "displayName": "Guest 42", "videoOn": true]]])
        model.handleEventForTesting(formatEvent(epoch: "", atMs: 100))
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.signal, "IDLE", "no revisioned roster: the row has no roster fact (Windows IDLE)")
        expectEqual(facts.format, "—", "and no format from an epoch-less fact")
    }

    /// CONFIRMATORY on fc675541 (ZoomRosterProjection already rejects it); pins the whole merge.
    @MainActor
    static func testAStaleRosterRevisionChangesNoRow() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest 42")
        model.applyZoomForTesting(lawRoster(["42"], revision: 2))
        model.handleEventForTesting(formatEvent(atMs: 100))
        model.applyZoomForTesting(lawRoster(["42"], revision: 1, videoOff: ["42"]))   // older: rejected
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.signal, "LIVE", "an older revision cannot turn the camera off")
        expectEqual(facts.formatWithRate, "1920×1080@60", "the delivered format stands")
    }

    /// Windows :86-99. CONFIRMATORY on fc675541 for the reset; independent for formatWithRate.
    @MainActor
    static func testANewMeetingEpochResetsEveryZoomRow() {
        let model = liveGuestModel()
        expectEqual(model.sourceFacts.box(1).facts.formatWithRate, "1920×1080@60", "precondition: live")
        model.applyZoomForTesting(lawRoster(["42"], revision: 1, epoch: "1:2:law"))
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.format, "—", "the new meeting clears the old format")
        expectEqual(facts.signal, "CONNECTING", "and waits for a frame of this meeting")
        model.handleEventForTesting(formatEvent(epoch: "1:1:law", atMs: 900))
        expectEqual(model.sourceFacts.box(1).facts.format, "—", "a late fact from the old meeting is refused")
    }

    /// Windows EngineStopped (:162-188) retires the epoch. Passes on fc675541; independent against the
    /// mutation that drops the retire in ZoomRosterProjection.resetForProcess (goes red: CONNECTING).
    @MainActor
    static func testARetiredEpochIsIgnoredAfterACoreRestart() {
        let model = liveGuestModel()
        model.onConnected()                                                         // a core generation
        model.handleEventForTesting(formatEvent(atMs: 500))                         // late, old epoch
        model.applyZoomForTesting(lawRoster(["42"], revision: 5))                   // old epoch, newer revision
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.format, "—", "nothing from the retired epoch repaints the row")
        expectEqual(facts.signal, "IDLE", "the row waits for the new core's roster")
    }

    /// Windows FormatDetailLabel (MultiviewInputRow.cs:69): "age {s:0.0}s stale"; FrameRateLabel "—"
    /// while stale (:40-41). The 2300 ms comes from the fixture. Fails on fc675541 (no detail).
    @MainActor
    static func testAStaleFormatShowsItsAgeAndNoRate() {
        let model = liveGuestModel()
        model.applySnapshotForTesting(["sources": [["sourceId": "42", "kind": "zoom", "width": 1920, "height": 1080,
                                                    "framesIngested": 90, "sinceLastNewFrameMs": 2300.0]] as [JSONObject]])
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.signal, "STALLED", "older than 1500 ms is stalled")
        expectEqual(facts.fps, "—", "a stale rate is never shown as live")
        expectEqual(facts.formatDetail, "age 2.3s stale", "the row says how old the picture is")
        expectEqual(facts.formatWithRate, "1920×1080", "the format keeps its size and drops the rate")
    }

    // ── Task 3: R2 ──────────────────────────────────────────────────────────

    /// Windows ApplyCameraCap (MultiviewInputRowsProjection.cs:144-159), quantified over every
    /// tier × observed size × fps cap × observed fps. The oracle is written here from the
    /// Windows source, not from the implementation (plan-authoring rule 2).
    static func testTheCapNoteFollowsTheWindowsRuleForEveryCombination() {
        let tiers: [(Int, String, Int, Int)] = [(0, "360p", 640, 360), (1, "720p", 1280, 720), (2, "1080p", 1920, 1080)]
        let sizes = [(640, 360), (1280, 720), (1920, 1080)]
        for (tier, label, capW, capH) in tiers {
            for (w, h) in sizes {
                for capFps in [30, 60] {
                    for fps in [25, 60] {
                        var parts: [String] = []
                        if w < capW || h < capH { parts.append("up to \(label) requested") }
                        if capFps < 60 && fps > capFps { parts.append("local max \(capFps) fps") }
                        let oracle = parts.joined(separator: " · ")
                        let note = SourceRowFactsProjection.capNote(width: w, height: h, fps: fps,
                                                                     cap: ZoomAppliedCap(tier: tier, fps: capFps))
                        expectEqual(note, oracle, "tier \(tier) observed \(w)×\(h)@\(fps) fps cap \(capFps)")
                    }
                }
            }
        }
        expectEqual(SourceRowFactsProjection.capNote(width: 640, height: 360, fps: 30, cap: nil), "",
                    "no applied cap from the core: no note")
    }

    /// D6: "the ceiling accepted at this meeting's join, never a pending preference" (Windows
    /// SetAppliedCameraCap :130). The Mac reads the core's applied cap from zoomSubscriptionChurn.
    @MainActor
    static func testTheCapNoteReadsTheCoresAppliedCap() {
        let model = liveGuestModel()                                 // delivers 1920×1080@60
        model.handleEventForTesting(formatEvent(w: 640, h: 360, fps: 30, atMs: 200))
        model.applySnapshotForTesting(["zoomSubscriptionChurn": ["engine": true, "cameraMaxResolution": 1,
                                                                "cameraMaxFps": 60] as JSONObject])
        expectEqual(model.sourceFacts.box(1).facts.formatDetail, "up to 720p requested",
                    "the core applied 720p; the guest delivers 360p")
        model.handleEventForTesting(formatEvent(w: 1280, h: 720, fps: 30, atMs: 300))
        expectEqual(model.sourceFacts.box(1).facts.formatDetail, "", "delivering the cap: no note")
        model.applySnapshotForTesting(["zoomSubscriptionChurn": ["engine": true, "cameraMaxResolution": 2,
                                                                "cameraMaxFps": 24] as JSONObject])
        expectEqual(model.sourceFacts.box(1).facts.formatDetail, "up to 1080p requested · local max 24 fps",
                    "a later applied cap re-labels the same observed format")
        model.applySnapshotForTesting(["zoomSubscriptionChurn": ["engine": false] as JSONObject])
        expectEqual(model.sourceFacts.box(1).facts.formatDetail, "", "no engine: no applied cap, no note")
    }

    /// Windows IsoChipUsesObservedWriterLifecycle (:413) through the real snapshot path, plus the
    /// Mac "unreported" rule (spec §4.9: never OFF). Recorded node shapes: CoreStateCapture.cpp:2001-2070.
    /// CONFIRMATORY: passes on fc675541 (Phase 1 wired the chip); it pins the wired path for R2.
    @MainActor
    static func testTheRecChipShowsTheFourWriterStatesFromSnapshots() {
        let model = liveGuestModel()
        func recording(_ state: String, _ health: String, stream: String) -> JSONObject {
            ["recording": ["sessionId": "s", "status": "recording", "active": true,
                           "lifecycle": ["state": state, "health": health],
                           "streams": [["kind": "iso", "sourceId": "zoom:42", "status": stream]]] as JSONObject]
        }
        let cases: [(JSONObject, String)] = [
            (recording("preparing", "healthy", stream: "writing"), "ARMED"),
            (recording("producing", "healthy", stream: "writing"), "REC"),
            (recording("producing", "healthy", stream: "warning"), "ERROR"),
            (recording("completed", "healthy", stream: "writing"), "OFF"),
            (recording("producing", "healthy", stream: "unreported"), "—"),
        ]
        for (snapshot, chip) in cases {
            model.applySnapshotForTesting(snapshot)
            expectEqual(model.sourceFacts.box(1).facts.writer, chip, "writer chip \(chip)")
        }
    }

    // ── Task 4: S4 media slots ──────────────────────────────────────────────

    /// FIXTURE: a still (png) "a1", a clip (mp4) "c1" and an audio bed (wav) "b1".
    static let s4Assets: [MediaAsset] = [
        MediaAsset(id: "a1", name: "Station logo", kind: "slate", relativePath: "slate/logo.png",
                   filePath: "/fixture/slate/logo.png", naturalWidth: 1920, naturalHeight: 1080),
        MediaAsset(id: "c1", name: "Show open", kind: "stinger", relativePath: "stinger/open.mp4",
                   filePath: "/fixture/stinger/open.mp4", naturalWidth: 1920, naturalHeight: 1080, durationMs: 12_000),
        MediaAsset(id: "b1", name: "Walk-on bed", kind: "audio-bed", relativePath: "audio-bed/bed.wav",
                   filePath: "/fixture/audio-bed/bed.wav", naturalWidth: 0, naturalHeight: 0, durationMs: 93_000),
    ]

    /// Windows ShowInputRosterService.cs:743-755: kind "media", sourceId "media:<id>", mediaAssetId.
    /// The path and kind ride along so the core can decode a wall-only still (plan Task 5).
    @MainActor
    static func testAMediaSlotFeedsTheMultiviewAsMedia() {
        var still = ShowInputSlot(id: 3); still.kind = "media"; still.sourceId = "a1"; still.name = "Logo"; still.inShow = true
        var cam = ShowInputSlot(id: 1); cam.kind = "capture"; cam.sourceId = "cam1"; cam.name = "Cam"; cam.inShow = true
        let payloads = AppModel.multiviewSourcePayloads([cam, still], mediaAssets: s4Assets)
        expectEqual(payloads.count, 2, "both in-show slots are on the wall")
        let media = payloads[1]
        expectEqual(media["kind"] as? String, "media", "the multiview vocabulary is media")
        expectEqual(media["sourceId"] as? String, "media:a1", "the canonical media id")
        expectEqual(media["mediaAssetId"] as? String, "a1", "the asset id")
        expectEqual(media["mediaAssetPath"] as? String, "/fixture/slate/logo.png", "the path for a wall-only decode")
        expectEqual(media["mediaAssetKind"] as? String, "slate", "and the asset kind")
        expect(media["captureDeviceId"] == nil && media["participantId"] == nil, "no capture or Zoom fields")
        expectEqual(payloads[0]["kind"] as? String, "capture", "a capture slot is still capture, never capture-input")
    }

    /// One binding for all three route builders (preset layout, an edited layer bound to the
    /// slot, and a solo cue): each must carry the media identity the core decodes from.
    @MainActor
    static func testEveryRouteBuilderCarriesAMediaSlotsIdentity() {
        let model = AppModel()
        model.mediaAssets = s4Assets
        model.assignSlot(1, kind: "media", sourceId: "a1", name: "Station logo")
        func mediaRoute(_ routes: [JSONObject], _ what: String) {
            guard let route = routes.first(where: { $0["mediaAssetId"] as? String == "a1" }) else {
                expect(false, "\(what): a route names the still"); return
            }
            expectEqual(route["mediaAssetPath"] as? String, "/fixture/slate/logo.png", "\(what): path")
            expectEqual(route["mediaAssetKind"] as? String, "slate", "\(what): kind")
            expectEqual(route["mode"] as? String, "fixed", "\(what): fixed, never capture-input")
            expect(route["captureDeviceId"] == nil, "\(what): no capture device id")
        }
        guard let preset = model.scenes.first(where: { AppModel.layoutRects[$0.layout] != nil && $0.layers.isEmpty }) else {
            expect(false, "precondition: a preset scene exists"); return
        }
        mediaRoute(model.buildRoutes(for: preset.id), "preset layout")
        model.ensureLayers(for: preset.id)
        mediaRoute(model.buildRoutes(for: preset.id), "edited layer")
        model.previewMultiviewSlot(1)
        mediaRoute(model.buildRoutes(for: model.previewSceneId), "solo cue")
    }

    /// Windows order Zoom, Camera, Screen, Media (ShowInputRosterService.cs:174-290); every bin
    /// asset with a picture is offered (BuildSourceOptions :437-443). An audio bed has no picture.
    @MainActor
    static func testThePickerListsMediaAfterScreensWithoutAudioOnlyFiles() {
        let roster = RosterParticipant.parse([["userId": "42", "displayName": "Guest", "videoOn": true]],
                                             assignedIds: [])
        let devices = [CaptureDeviceRow(id: "cam1", name: "Cam", kind: "video", vendor: "avfoundation",
                                        connectionState: "connected", signalPresent: true, warning: ""),
                       CaptureDeviceRow(id: "screen:1", name: "Display", kind: "video", vendor: "sck",
                                        connectionState: "detected", signalPresent: false, warning: "")]
        let groups = SourcePickerCatalog.groups(roster: roster, devices: devices, media: s4Assets, current: nil)
        expectEqual(groups.map(\.title), ["Zoom", "Camera", "Screen", "Media"], "the Windows group order")
        expectEqual(groups.last?.options.map(\.sourceKey), ["media:a1", "media:c1"], "stills and clips, no audio bed")
        expectEqual(groups.last?.options.map(\.label), ["Still · Station logo", "Media clip · Show open"],
                    "each labelled with its type")
        expectEqual(SourceType.of(kind: "media", sourceId: "a1", vendor: nil, media: s4Assets), .still, "a png is a still")
        expectEqual(SourceType.of(kind: "media", sourceId: "c1", vendor: nil, media: s4Assets), .mediaClip, "an mp4 is a clip")
        expectEqual(SourceType.still.category, "Media", "the Media filter category")
    }

    /// Media slots persist like capture (the asset id is stable, unlike a Zoom id). A restored
    /// slot reads offline until the bin lists it; a missing asset is never substituted.
    @MainActor
    static func testAMediaSlotIsSavedRestoredAndNeverSubstituted() {
        let model = AppModel()
        model.mediaAssets = s4Assets
        model.assignSlot(2, kind: "media", sourceId: "c1", name: "Show open")
        let saved = model.currentPrefs().showInputSlots ?? []
        expectEqual(saved.map(\.kind), ["media"], "the media slot is saved")
        expectEqual(saved.first?.sourceId, "c1", "by asset id")
        let next = AppModel()
        next.restoreSlotAssignments(saved)
        expectEqual(next.slots[1].kind, "media", "restored")
        expectEqual(next.slots[1].sourceKey, "media:c1", "with its media source key")
        expect(next.slots[1].offline, "offline until the bin lists the asset")
        next.mediaAssets = s4Assets
        next.refreshSlotHealthForTesting()
        expect(!next.slots[1].offline, "the bin has it: online")
        next.mediaAssets = s4Assets.filter { $0.id != "c1" }
        next.refreshSlotHealthForTesting()
        expect(next.slots[1].offline, "the asset left the bin: offline")
        expectEqual(next.slots[1].sourceId, "c1", "never substituted with another asset")
        next.mediaAssets = s4Assets
        next.removeMediaAsset("c1")
        expect(next.mediaStatus.contains("input 2"), "a slot holding the asset blocks its removal: \(next.mediaStatus)")
    }

    /// THE SLOT LAW: media appears in a slot only by operator action.
    @MainActor
    static func testAutoAssignNeverPlacesOrMovesMedia() {
        let model = AppModel()
        model.mediaAssets = s4Assets
        model.assignSlot(1, kind: "media", sourceId: "a1", name: "Station logo")
        model.applyZoomForTesting(lawRoster(["A", "B"], revision: 1))
        expectEqual(model.slots[0].sourceKey, "media:a1", "a roster sync leaves the media slot alone")
        expectEqual(model.slots.filter { $0.kind == "media" }.count, 1, "and never fills a slot from the bin")
        model.applyZoomForTesting(lawRoster([], revision: 2))
        expectEqual(model.slots[0].sourceKey, "media:a1", "an empty roster frees Zoom slots only")
    }

    /// Plan D5 (departs from Windows, which reads media as a capture device and shows NO INCOMING,
    /// MultiviewInputRowsProjection.cs:367-384): a still never advances, so it is never STALLED.
    @MainActor
    static func testAMediaRowIsLiveFromTheBusAndNeverStalled() {
        let model = AppModel()
        model.mediaAssets = s4Assets
        model.assignSlot(1, kind: "media", sourceId: "a1", name: "Station logo")
        expectEqual(model.sourceFacts.box(1).facts.signal, "NO INCOMING", "no bus row yet: nothing decoded")
        model.applySnapshotForTesting(["sources": [["sourceId": "media:a1", "kind": "still", "width": 1920,
                                                    "height": 1080, "framesIngested": 1,
                                                    "sinceLastNewFrameMs": 9000.0]] as [JSONObject]])
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.signal, "LIVE", "a decoded still is live, 9 s after its only frame")
        expectEqual(facts.format, "1920×1080", "its size from the bus")
        model.applySnapshotForTesting(["programFrame": ["frameNumber": 3, "videoSources": [["sourceId": "media:a1"]]]])
        expectEqual(model.sourceFacts.box(1).facts.chip, "PGM", "on Program it reads PGM")
    }

    // ── Task 8: R3 ──────────────────────────────────────────────────────────

    /// Comms spec C2: Health shows the last few `sdkSubscriptionEvidence` rows. FIXTURE: seven
    /// events (sequence 1..7) shaped like ZoomSubscriptionSdkEvidence::observe; only 3..7 show,
    /// newest first. The platform names the SDK error space (ZoomSDKError on macOS).
    static func testHealthListsTheLastFiveSubscriptionEvidenceRows() {
        var events: [JSONObject] = []
        for seq in 1...7 {
            events.append(["sequence": seq, "stage": "video_subscribe", "participantId": "4\(seq)",
                           "sdkCode": 0, "resolution": 2])
        }
        events[6] = ["sequence": 7, "stage": "video_resolution_downgraded", "participantId": "47",
                     "requestedResolution": 2, "actualResolution": 1]
        let snapshot: JSONObject = ["zoomSubscriptionChurn": ["engine": true, "sdkSubscriptionEvidence": [
            "platform": "macos", "capacity": 64, "dropped": 0, "events": events] as JSONObject] as JSONObject]
        let rows = SubscriptionEvidenceReadout.rows(snapshot)
        expectEqual(rows.count, 6, "a header row and the last five events")
        expectEqual(rows.first?.label, "sdk evidence", "the header")
        expectEqual(rows.first?.value, "7 events · macOS codes (ZoomSDKError)", "count and error space")
        expectEqual(rows[1].value, "participant 47 · video_resolution_downgraded · requested 2 → actual 1",
                    "newest first, with the downgrade")
        expectEqual(rows[2].value, "participant 46 · video_subscribe · code 0 · resolution 2", "then the next")
        expectEqual(rows.last?.value, "participant 43 · video_subscribe · code 0 · resolution 2", "five shown")
        expectEqual(SubscriptionEvidenceReadout.rows(["zoomSubscriptionChurn": ["engine": false] as JSONObject]).first?.value,
                    "no Zoom engine", "no engine is said, not blank")
        expect(SubscriptionEvidenceReadout.rows([:]).isEmpty, "no node: no row")
    }

    // ── Task 6: PVW from the core ───────────────────────────────────────────

    /// Plan D4 ("tally must reflect what is actually on Program/Preview from core evidence, not
    /// shell intent"). The shell's Preview routes name guest 42 throughout; only the core's
    /// previewFrame.videoSources decides PVW. Fails on fc675541 (PVW came from buildRoutes).
    @MainActor
    static func testThePreviewTallyIsWhatTheCoreRenderedNotTheShellsRoutes() {
        let model = liveGuestModel()
        model.previewMultiviewSlot(1)                                        // shell intent: 42 cued
        expect(model.buildRoutes(for: model.previewSceneId).contains { $0["participantId"] as? String == "42" },
               "precondition: the shell's Preview routes name guest 42")
        model.applySnapshotForTesting(["programFrame": ["frameNumber": 1, "videoSources": [JSONObject]()],
                                       "previewFrame": ["sceneId": "", "videoSources": [JSONObject]()]])
        expectEqual(model.sourceFacts.box(1).facts.tally, "none", "the core has not rendered 42 on Preview")
        model.applySnapshotForTesting(["previewFrame": ["sceneId": "solo-a", "videoSources": [
            ["sourceId": "zoom:42", "participantId": "42", "kind": "participant-video", "layerId": "l"]]]])
        expectEqual(model.sourceFacts.box(1).facts.tally, "pvw", "the core rendered 42 on Preview")
        model.applySnapshotForTesting(["programFrame": ["frameNumber": 2, "videoSources": [
            ["sourceId": "zoom:42", "participantId": "42", "kind": "participant-video", "layerId": "l"]]]])
        expectEqual(model.sourceFacts.box(1).facts.tally, "pgm", "Program outranks Preview, and the Preview fact is kept")
    }

    // ── Task 10: S6 shell ───────────────────────────────────────────────────

    static func s6Slot(_ id: Int, kind: String = "zoom", source: String, inShow: Bool = true) -> ShowInputSlot {
        var slot = ShowInputSlot(id: id); slot.kind = kind; slot.sourceId = source; slot.name = source; slot.inShow = inShow
        return slot
    }

    static func s6Sheet(_ cells: [Int: (String, String)], surface: UInt32 = 77) -> SourceSheetState {
        SourceSheetState(surfaceId: surface, columns: 5, rows: 2,
                         cells: cells.mapValues { SheetCell(slot: 0, sourceId: $0.0, state: $0.1) })
    }

    static func liveFacts(_ signal: String = "LIVE") -> SourceRowFacts {
        var facts = SourceRowFacts(); facts.signal = signal; return facts
    }

    /// Oracle: Windows PatchPreview (:264-300) — a row exposes a picture only for its own
    /// identity, only while in show, only for a live or held cell. Quantified over the cell's
    /// name (A / B / none) x the slot's source (A / B) x state (live / held / empty) x in show,
    /// for slots 1, 5, 6 and 10 (both grid rows, both edges).
    @MainActor
    static func testARowShowsOnlyACellNamingItsOwnSource() {
        for slotId in [1, 5, 6, 10] {
            for cellName in ["zoom:A", "zoom:B", ""] {
                for slotSource in ["A", "B"] {
                    for state in ["live", "held", "empty"] {
                        for inShow in [true, false] {
                            let slot = s6Slot(slotId, source: slotSource, inShow: inShow)
                            let sheet = s6Sheet([slotId - 1: (cellName, state)])
                            let result = SourceThumbnailPolicy.resolve(slot: slot, facts: liveFacts(), sheet: sheet)
                            let shows = inShow && cellName == "zoom:" + slotSource && state != "empty"
                            let expected: SourceThumbnailPolicy.Result = shows
                                ? .cell(surfaceId: 77, column: (slotId - 1) % 5, row: (slotId - 1) / 5, columns: 5, rows: 2)
                                : .placeholder(inShow ? SourceThumbnailPolicy.waiting : SourceThumbnailPolicy.notInMultiview)
                            expectEqual(result, expected,
                                        "slot \(slotId) source \(slotSource) cell '\(cellName)' \(state) inShow \(inShow)")
                        }
                    }
                }
            }
        }
        // A cell naming the row's source in ANOTHER slot's position is never borrowed.
        let wrongCell = SourceThumbnailPolicy.resolve(slot: s6Slot(3, source: "A"), facts: liveFacts(),
                                                      sheet: s6Sheet([0: ("zoom:A", "live")]))
        expectEqual(wrongCell, .placeholder(SourceThumbnailPolicy.waiting), "slot 3 never shows cell 0's picture")
    }

    @MainActor
    static func testEachPlaceholderSaysWhyThereIsNoPicture() {
        let sheet = s6Sheet([0: ("zoom:A", "live"), 1: ("capture:cam", "live"), 2: ("media:m", "held")])
        typealias P = SourceThumbnailPolicy
        expectEqual(P.resolve(slot: s6Slot(1, source: "A"), facts: liveFacts("VIDEO OFF"), sheet: sheet),
                    .placeholder(P.videoOff), "a camera-off guest says Video off")
        expectEqual(P.resolve(slot: s6Slot(1, source: "A"), facts: liveFacts("NO INCOMING"), sheet: sheet),
                    .placeholder(P.noSignal), "a Zoom row with no incoming video says No signal")
        expectEqual(P.resolve(slot: s6Slot(2, kind: "capture", source: "cam"), facts: liveFacts("STALLED"), sheet: sheet),
                    .placeholder(P.noSignal), "capture shows a picture only while LIVE (Windows)")
        if case .cell = P.resolve(slot: s6Slot(3, kind: "media", source: "m"), facts: liveFacts("NO INCOMING"), sheet: sheet) {
        } else { expect(false, "media is always eligible (Windows): a held cell shows") }
        expectEqual(P.resolve(slot: s6Slot(1, source: "A"), facts: liveFacts(), sheet: nil),
                    .placeholder(P.unavailable), "no sheet (older core, no demand) says Preview unavailable")
        expectEqual(P.resolve(slot: ShowInputSlot(id: 4), facts: liveFacts(), sheet: sheet),
                    .placeholder(P.notInMultiview), "an unassigned slot")
        var offline = s6Slot(1, source: "A"); offline.offline = true
        expectEqual(P.resolve(slot: offline, facts: liveFacts(), sheet: sheet),
                    .placeholder(P.notInMultiview), "an offline slot is not on the wall")
    }

    @MainActor
    static func testTheSheetNodeParsesOnlyWhenActiveWithASurface() {
        func node(active: Bool, surface: Int) -> JSONObject {
            ["sourceSheet": ["active": active, "texture": ["iosurfaceId": surface, "width": 3200, "height": 720],
                             "columns": 5, "rows": 2, "cellWidth": 640, "cellHeight": 360,
                             "cells": [["slot": 4, "sourceId": "zoom:101", "state": "live"] as JSONObject,
                                       ["slot": 0, "sourceId": "", "state": "empty"] as JSONObject]] as JSONObject]
        }
        let sheet = SourceSheetState.parse(node(active: true, surface: 9))
        expectEqual(sheet?.surfaceId, 9, "the surface id")
        expectEqual(sheet?.cells[4], SheetCell(slot: 4, sourceId: "zoom:101", state: "live"), "cells keyed by slot")
        expect(SourceSheetState.parse(node(active: false, surface: 9)) == nil, "demand off: no sheet")
        expect(SourceSheetState.parse(node(active: true, surface: 0)) == nil, "no exported surface: no sheet")
        expect(SourceSheetState.parse([:]) == nil, "an older core with no node: no sheet")
    }

    /// The core's lease lasts 3 s (core/SourcePreviewDemand.h): tab enter sends true now and
    /// on every tick; leave sends false ONCE; a new core generation while visible re-sends true.
    @MainActor
    static func testThePreviewDemandLeaseFollowsTheSourcesTab() {
        let model = AppModel()
        var sent: [Bool] = []
        model.coreCommandCaptureForTesting = { body in
            for command in body["commands"] as? [JSONObject] ?? []
            where command["type"] as? String == "set-source-preview-demand" {
                sent.append(command["enabled"] as? Bool ?? false)
            }
        }
        model.selectedTab = .studio
        expectEqual(sent, [], "never asked for while another tab is up")
        model.selectedTab = .sources
        expectEqual(sent, [true], "entering the tab asks at once")
        model.sourcePreviewDemand.tick(); model.sourcePreviewDemand.tick()
        expectEqual(sent, [true, true, true], "renewed every second (the lease is 3 s)")
        model.onConnected()
        expectEqual(sent.last, true, "a respawned core is re-asked while the tab is visible")
        let beforeLeave = sent.count
        model.selectedTab = .audio
        expectEqual(Array(sent.dropFirst(beforeLeave)), [false], "leaving releases once")
        model.sourcePreviewDemand.tick(); model.onConnected()
        expectEqual(Array(sent.dropFirst(beforeLeave)), [false], "nothing more while hidden")
        model.updateSourcePreviewDemand()
    }

    @MainActor
    static func testAReassignedRowBlanksUntilTheCoreNamesTheNewSource() {
        let model = liveGuestModel()                                         // guest 42 in slot 1
        model.assignSlot(2, kind: "zoom", sourceId: "43", name: "Guest 43")
        model.applyZoomForTesting(lawRoster(["42", "43"], revision: 2))
        func sheetSnapshot(_ cell0: String) -> JSONObject {
            ["sourceSheet": ["active": true, "texture": ["iosurfaceId": 77], "columns": 5, "rows": 2,
                             "cells": [["slot": 0, "sourceId": cell0, "state": "live"] as JSONObject]] as JSONObject]
        }
        func row1() -> SourceThumbnailPolicy.Result {
            SourceThumbnailPolicy.resolve(slot: model.slots[0], facts: model.sourceFacts.box(1).facts,
                                          sheet: model.sourceSheet)
        }
        model.applySnapshotForTesting(sheetSnapshot("zoom:42"))
        if case .cell = row1() {} else { expect(false, "precondition: slot 1 shows guest 42's cell") }
        model.assignSlot(1, kind: "zoom", sourceId: "43", name: "Guest 43")
        model.handleEventForTesting(formatEvent("43", atMs: 200))
        expect(row1() != .placeholder(SourceThumbnailPolicy.notInMultiview) && {
            if case .cell = row1() { return false } else { return true } }(),
            "reassigned: the sheet still names 42, so the row blanks at once")
        model.applySnapshotForTesting(sheetSnapshot("zoom:43"))
        if case .cell = row1() {} else { expect(false, "the core now names 43 in cell 0: the row shows it") }
    }

    @MainActor
    static func testTheThumbnailBorderUsesTheWindowsTallyColours() {
        typealias P = SourceThumbnailPolicy
        expectEqual(P.borderColor(tally: "pgm", signal: "LIVE"), Studio.amber, "Program is amber (255,159,10)")
        expectEqual(P.borderColor(tally: "hold", signal: "STALLED"), Studio.amber, "hold is amber")
        expectEqual(P.borderColor(tally: "pvw", signal: "LIVE"), Studio.accent, "Preview is green (48,209,88)")
        expectEqual(P.borderColor(tally: "talking", signal: "TALKING"), Studio.textPrimary, "talking is white")
        expectEqual(P.borderColor(tally: "none", signal: "LIVE"), Studio.border, "no tally is the quiet border")
        expectEqual(P.borderColor(tally: "pgm", signal: "VIDEO OFF"), Studio.border, "VIDEO OFF never tallies (:298)")
    }

    /// Spec correction 2: the core numbers an entry without `slot` by list position, so a
    /// filtered list (slot 2 out of show) put slot 3's source in cell 1 of the sheet.
    @MainActor
    static func testTheMultiviewPayloadCarriesTheZeroBasedSlot() {
        let payloads = AppModel.multiviewSourcePayloads(
            [s6Slot(1, source: "A"), s6Slot(2, source: "B", inShow: false),
             s6Slot(3, kind: "capture", source: "cam"), s6Slot(7, kind: "media", source: "a1")],
            mediaAssets: s4Assets)
        expectEqual(payloads.map { $0["slot"] as? Int }, [0, 2, 6], "slot - 1 for zoom, capture and media")
    }

    @MainActor
    static func testTheSheetLayerFrameShowsExactlyTheSlotsCell() {
        let cell = CGSize(width: 96, height: 54)
        for index in 0..<10 {
            let frame = SourceThumbnailPolicy.sheetFrame(column: index % 5, row: index / 5, columns: 5, rows: 2, cell: cell)
            expectEqual(frame.size, CGSize(width: 480, height: 108), "the whole sheet at cell scale")
            // In the container's flipped (top-down) space, the visible window (0,0,96,54) maps to
            // sheet pixels starting at (col*96, row*54): exactly that cell.
            expectEqual(CGPoint(x: -frame.minX, y: -frame.minY),
                        CGPoint(x: CGFloat(index % 5) * 96, y: CGFloat(index / 5) * 54), "slot \(index + 1)")
        }
        let view = SourceThumbnailNSView(frame: NSRect(x: 0, y: 0, width: 96, height: 54))
        expect(view.layer?.isGeometryFlipped == true, "the container is top-down, so row 0 is the top row")
        expect(view.layer?.masksToBounds == true, "and clips the other nine cells")
    }
}
