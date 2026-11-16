// macOS media clip playback, Phase 1 (plan 2026-11-16-mac-media-phase1). Kept in
// its own file, like ShellTestsTier0.swift and ShellTestsVcamUpdate.swift, so
// parallel branches extending ShellTests.swift do not collide line-for-line.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var mediaCases: [(String, () -> Void)] {
        [
            ("media/background-picker-options", testBackgroundPickerListsStillsAndPlayableVideo),
        ]
    }

    @MainActor
    private static func testBackgroundPickerListsStillsAndPlayableVideo() {
        func asset(_ id: String, _ file: String, _ kind: String) -> MediaAsset {
            MediaAsset(id: id, name: id, kind: kind, relativePath: kind + "/" + file,
                       filePath: "/bin/" + kind + "/" + file, naturalWidth: 0, naturalHeight: 0)
        }
        // Fixture: one of every class the bin can hold, in bin order.
        let bin = [asset("sting", "open.mp4", "stinger"), asset("mov", "loop.mov", "stinger"),
                   asset("web", "x.webm", "stinger"), asset("logo", "logo.png", "lower-third"),
                   asset("jpg", "card.jpg", "lower-third"), asset("bed", "bed.wav", "audio-bed"),
                   asset("slate", "slate-break.mp4", "slate")]
        let ids = MediaBin.backgroundOptions(bin).map(\.id)
        expectEqual(ids, ["sting", "mov", "logo", "jpg", "slate"],
                    "stills + mp4/mov in bin order; no webm (D1), no audio")
    }
}
