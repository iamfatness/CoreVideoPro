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
}
