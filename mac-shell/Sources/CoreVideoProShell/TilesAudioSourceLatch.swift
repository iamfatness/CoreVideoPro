// Sticky Tiles audio (Windows TilesAudioSourceLatch, MediaCore/Services, verbatim):
// a Tiles member stays an AUDIO source while their scene is on either bus, so a
// panelist who turns their camera off (the membership drops them) keeps talking
// on air, including across the Take that swaps their wall from Preview to
// Program. Forgets a scene on neither bus, a participant who leaves, and
// everything on a KNOWN "not in a meeting" (or Engine off via clear()). An
// UNKNOWN meeting state never clears it. Zoom reuses per-meeting user ids, so a
// latch outliving the meeting would make a different person audible.
//
// Main-actor only (the spine builder), so no lock is needed on Mac.

import Foundation

final class TilesAudioSourceLatch {
    /// Insertion-ordered per scene, scenes in first-seen order: stable result order.
    private var scenes: [(sceneId: String, members: [String])] = []

    /// `present` must be the FULL in-room roster, camera-off included
    /// (AVideoOnlyPresentSetForgetsACameraOffPanelist_ProductionMustNotObserveThatWay).
    func observe(inMeeting: Bool?, programSceneId: String?, programTiles: JSONObject?,
                 previewSceneId: String?, previewTiles: JSONObject?, present: Set<String>) -> [String] {
        if inMeeting == false {
            scenes.removeAll()
            return []
        }
        guard inMeeting == true else { return snapshot() }
        var onAir: [(String, JSONObject)] = []
        if let id = programSceneId, !id.isEmpty, let tiles = programTiles { onAir.append((id, tiles)) }
        if let id = previewSceneId, !id.isEmpty, let tiles = previewTiles, !onAir.contains(where: { $0.0 == id }) {
            onAir.append((id, tiles))
        }
        scenes.removeAll { entry in !onAir.contains { $0.0 == entry.sceneId } }
        for (sceneId, tiles) in onAir {
            var index = scenes.firstIndex { $0.sceneId == sceneId }
            if index == nil { scenes.append((sceneId, [])); index = scenes.count - 1 }
            var members = scenes[index!].members.filter(present.contains)   // leaving the meeting ends it
            for member in tiles["members"] as? [String] ?? [] {
                if let id = ZoomSourceBudget.zoomMemberId(member), present.contains(id), !members.contains(id) {
                    members.append(id)
                }
            }
            scenes[index!].members = members
        }
        return snapshot()
    }

    func clear() { scenes.removeAll() }

    private func snapshot() -> [String] {
        var result: [String] = []
        for (_, members) in scenes { for id in members where !result.contains(id) { result.append(id) } }
        return result
    }
}
