// ZoomSourceBudget — the Mac shell's video-subscription budget policy.
//
// Ports ZoomSourceSetPolicy.Resolve (native-shell/CoreVideoPro.MediaCore/Services,
// evidence E2) scaled to the Mac shell's model, which has no Tiles wall: Program
// routes -> Preview routes -> multiview slots in slot order -> ISO-armed sources.
// Camera-OFF sources never spend video budget (E2/E5) because the real engine
// path applies NO videoOn check (E5) — that filter is this policy's job, mirroring
// Windows' ZoomSourceSetPolicy. The cap is DefaultMaxVideoSubscriptions = 10 (E1,
// native-shell/CoreVideoPro.MediaCore/Services/ZoomMediaSpinePayloadBuilder.cs:19),
// the number that ended the 2026-08-09 Susan Cho frozen-tile incident.
//
// Deliberately wire-conservative (E4/E6): the Mac shell is fenced at kind "video"
// (never promoted past 720P — ZoomSubscriptionResolutionPolicy.h:71-72), and for
// kind "video" the purpose is part of the engine's subscription identity
// (ZoomEngineRuntime.cpp:452-457) — so AppModel.spineSubscriptionPayloads keeps
// emitting a constant "program" purpose for every entry until the 1080P flip task
// moves Mac onto kind "participant-video" (purpose-free identity) and activates
// Entry.purpose for real. This type's ordering/filter/cap machinery is what that
// task needs; Entry.purpose is carried now so nothing has to be re-derived later.

struct ZoomSourceBudget {
    struct Entry: Equatable {
        let participantId: String
        let purpose: String
    }

    static let maxVideoSubscriptions = 10

    /// Program -> Preview -> multiview (slot order) -> ISO-armed, camera-on only,
    /// keep-first deduped (a pid's purpose is its HIGHEST tier), capped at
    /// `maxVideoSubscriptions`. Pure and deterministic for identical inputs.
    static func videoEntries(
        programRouted: [String], previewRouted: [String],
        multiviewAssigned: [String], isoArmed: [String], cameraOn: Set<String>
    ) -> [Entry] {
        var seen: Set<String> = []
        var entries: [Entry] = []
        func admit(_ ids: [String], purpose: String) {
            for id in ids {
                guard cameraOn.contains(id), !seen.contains(id) else { continue }
                seen.insert(id)
                entries.append(Entry(participantId: id, purpose: purpose))
            }
        }
        admit(programRouted, purpose: "program")
        admit(previewRouted, purpose: "preview")
        admit(multiviewAssigned, purpose: "multiview")
        admit(isoArmed, purpose: "iso")
        return Array(entries.prefix(maxVideoSubscriptions))
    }

    /// Ordered, de-duplicated (keep-first) participant ids of fixed zoom routes —
    /// a route with no `participantId` (an unbound active-speaker layer, or a
    /// capture-input route whose id lives under `captureDeviceId`) contributes
    /// nothing.
    static func routedZoomPids(_ routes: [JSONObject]) -> [String] {
        var seen: Set<String> = []
        var pids: [String] = []
        for route in routes {
            guard let pid = route["participantId"] as? String, !pid.isEmpty,
                  !seen.contains(pid) else { continue }
            seen.insert(pid)
            pids.append(pid)
        }
        return pids
    }
}
