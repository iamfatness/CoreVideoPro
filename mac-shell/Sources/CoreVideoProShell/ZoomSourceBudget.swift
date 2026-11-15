// ZoomSourceBudget — the Mac shell's video-subscription budget policy.
//
// Ports ZoomSourceSetPolicy.Resolve (native-shell/CoreVideoPro.MediaCore/Services/
// ZoomSourceSetPolicy.cs, evidence E2 — the reference for the tier order this type
// mirrors) scaled to the Mac shell's model, which has no Tiles wall: Program routes
// -> Preview routes -> multiview slots in slot order -> ISO-armed sources. Camera-OFF
// sources never spend video budget (E2/E5) because the real engine path
// (native/src/modules/ZoomEngineRuntime.cpp:435-470, `Budget::resolve` at line 469,
// called for every non-audio subscription) applies NO videoOn check (E5) — that
// filter is this policy's job, mirroring Windows' ZoomSourceSetPolicy. The cap is
// DefaultMaxVideoSubscriptions = 10 (E1,
// native-shell/CoreVideoPro.MediaCore/Services/ZoomMediaSpinePayloadBuilder.cs:19),
// the number that ended the 2026-08-09 Susan Cho frozen-tile incident.
//
// CURRENT WIRE (since commit deb2689e, the 1080P kind flip — SHIPPED, not pending):
// AppModel.spineSubscriptionPayloads emits kind "participant-video" plus each
// Entry's real purpose, in budget order; the core grants 1080P to the first 8
// camera-on entries in that order (ZoomSubscriptionResolutionPolicy.h,
// kMaxConcurrentFullResolutionCameras = 8). This is safe only because of E6: for
// kind "participant-video" the engine's subscription identity
// (ZoomEngineRuntime.cpp:452-457) is "participant-video-<pid>-camera" — purpose-free
// — so Entry.purpose can ride (and change between syncs) with no renderer
// teardown/rebuild. This type's ordering/filter/cap machinery, consumed by the
// budget at ZoomEngineRuntime.cpp:435-470 above, is what makes that grant
// deterministic; Entry.purpose is what carries each source's tier to the core.

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

    /// The synthetic participant id the Zoom MEETING MIX (`meeting-audio`) is
    /// subscribed under. Windows: ZoomMediaSpinePayloadBuilder.MeetingMixParticipantId.
    /// Never a real guest's id: the mix callback carries no user id, and keying it at
    /// a guest's id made that guest's isolate strip silent (#465). The core ingests
    /// it as the "zoom-mix" source (ZoomEngineRuntime.cpp, pendingKey "zoom-mix").
    static let meetingMixParticipantId = "0"

    /// AUDIO: the SAME source set as video (#478 owner rule "sources only"), in the
    /// same tier order, but a camera-OFF source KEEPS its audio (a wall guest with
    /// the camera off can still speak) and audio is UNCAPPED. Windows:
    /// ZoomSourceSetPolicy.AudioParticipantIds over Resolve(). A pid not in the live
    /// roster (`present`) is not a source — Windows' Resolve requires the
    /// participant to be in the room. A non-source is never subscribed and is
    /// inaudible in Program: nothing is on air unless it is an input.
    static func audioParticipantIds(
        programRouted: [String], previewRouted: [String],
        multiviewAssigned: [String], isoArmed: [String], present: Set<String>
    ) -> [String] {
        var seen: Set<String> = []
        var ids: [String] = []
        for id in programRouted + previewRouted + multiviewAssigned + isoArmed
            where present.contains(id) && !seen.contains(id) {
            seen.insert(id)
            ids.append(id)
        }
        return ids
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
