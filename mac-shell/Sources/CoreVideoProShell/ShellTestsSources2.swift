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
}
