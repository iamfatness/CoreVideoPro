// ZoomSourceBudget — the Mac shell's video-subscription budget policy.
//
// Ports ZoomSourceSetPolicy.Resolve (native-shell/CoreVideoPro.MediaCore/Services/
// ZoomSourceSetPolicy.cs, evidence E2 — the reference for the tier order this type
// mirrors), Tiles tiers included (see `resolve` below). Camera-OFF
// sources never spend video budget (E2/E5) because the real engine path
// (native/src/modules/ZoomEngineRuntime.cpp:435-470, `Budget::resolve` at line 469,
// called for every non-audio subscription) applies NO videoOn check (E5) — that
// filter is this policy's job, mirroring Windows' ZoomSourceSetPolicy. The cap is
// DefaultMaxVideoSubscriptions = 8 (E1,
// native-shell/CoreVideoPro.MediaCore/Services/ZoomMediaSpinePayloadBuilder.cs),
// owner ruling 2026-11-13: Pro pulls at most 8 Zoom feeds, all inside the 1080P grant;
// the other sources (up to 16) come from CoreVideo Edge or local inputs.
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

    static let maxVideoSubscriptions = 8

    /// Thin wrapper over `resolve` for callers without Tiles: fixed Program
    /// routes -> fixed Preview routes -> multiview (slot order) -> ISO, camera-on
    /// only, capped. Purpose follows the Windows rule (a fixed bus route wins).
    static func videoEntries(
        programRouted: [String], previewRouted: [String],
        multiviewAssigned: [String], isoArmed: [String], cameraOn: Set<String>
    ) -> [Entry] {
        let input = wrapperInput(programRouted: programRouted, previewRouted: previewRouted,
                                 multiviewAssigned: multiviewAssigned, isoArmed: isoArmed) { cameraOn.contains($0) }
        return decideVideo(resolve(input), max: maxVideoSubscriptions).subscribed
            .map { Entry(participantId: $0.participantId, purpose: $0.purpose) }
    }

    private static func wrapperInput(programRouted: [String], previewRouted: [String], multiviewAssigned: [String],
                                     isoArmed: [String], videoOn: (String) -> Bool) -> Input {
        var ids: [String] = []
        for id in programRouted + previewRouted + multiviewAssigned + isoArmed where !ids.contains(id) { ids.append(id) }
        var input = Input()
        input.participants = ids.map { ($0, videoOn($0)) }
        input.programRoutes = programRouted.map { ["mode": "fixed", "participantId": $0] }
        input.previewRoutes = previewRouted.map { ["mode": "fixed", "participantId": $0] }
        input.multiview = multiviewAssigned.enumerated().map { ["kind": "zoom", "participantId": $0.element, "slot": $0.offset] }
        input.iso = isoArmed
        return input
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
        let input = wrapperInput(programRouted: programRouted.filter(present.contains),
                                 previewRouted: previewRouted.filter(present.contains),
                                 multiviewAssigned: multiviewAssigned.filter(present.contains),
                                 isoArmed: isoArmed.filter(present.contains)) { _ in true }
        return audioIds(resolve(input))
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

// MARK: - The source set with Tiles tiers (Windows ZoomSourceSetPolicy, verbatim)
//
// Program fixed routes -> Program Tiles members (+ a Zoom wall background) ->
// Preview fixed routes -> Preview Tiles -> multiview Zoom slots (slot order) ->
// ISO -> sticky Tiles audio (audio only). Purpose: `program` for a pid on a fixed
// Program route, else `preview` for a fixed Preview route, else the first tier
// that lists it. A follow-speaker route (no participant/capture/media id, mode
// `active-speaker`) adds nobody and grants nothing. Only roster-present ids count.
// Confirmed against contracts/tiles-spine.fixtures.json, generated by the real
// Windows ZoomMediaSpinePayloadBuilder (TilesSpineFixtureTests).
extension ZoomSourceBudget {
    static let programPurpose = "program", previewPurpose = "preview"
    static let programTilesPurpose = "program-tiles", previewTilesPurpose = "preview-tiles"
    static let tilesAudioPurpose = "tiles-audio", multiviewPurpose = "multiview", isoPurpose = "iso"

    struct Source: Equatable {
        let participantId: String
        let purpose: String
        /// False for a camera-off source AND for a sticky audio-only one.
        let videoOn: Bool
    }

    struct Input {
        /// The room's participants in roster order.
        var participants: [(id: String, videoOn: Bool)] = []
        var programRoutes: [JSONObject] = []
        var programTiles: JSONObject?
        var previewRoutes: [JSONObject] = []
        var previewTiles: JSONObject?
        /// Bare Zoom ids latched as Tiles members of a scene still on its bus.
        var stickyAudio: [String] = []
        /// Multiview wall sources (any kind; only Zoom ones count).
        var multiview: [JSONObject] = []
        /// Bare Zoom ids armed for ISO.
        var iso: [String] = []
    }

    static func resolve(_ input: Input) -> [Source] {
        var videoOn: [String: Bool] = [:]
        for p in input.participants where !p.id.trimmingCharacters(in: .whitespaces).isEmpty && videoOn[p.id] == nil {
            videoOn[p.id] = p.videoOn
        }
        let onProgramRoute = fixedRouteParticipants(input.programRoutes)
        let onPreviewRoute = fixedRouteParticipants(input.previewRoutes)
        var sources: [Source] = []
        var seen = Set<String>()
        func add(_ id: String?, _ tierPurpose: String, audioOnly: Bool = false) {
            guard let id, !id.trimmingCharacters(in: .whitespaces).isEmpty, let on = videoOn[id],
                  seen.insert(id).inserted else { return }
            let purpose = onProgramRoute.contains(id) ? programPurpose
                : onPreviewRoute.contains(id) ? previewPurpose : tierPurpose
            sources.append(Source(participantId: id, purpose: purpose, videoOn: on && !audioOnly))
        }
        func addFixedRoutes(_ routes: [JSONObject], _ purpose: String) {
            for route in routes where !isFollowSpeakerRoute(route) { add(bareZoomId(route["participantId"] as? String), purpose) }
        }
        func addTiles(_ tiles: JSONObject?, _ purpose: String) {
            guard let tiles else { return }
            for member in tiles["members"] as? [String] ?? [] { add(zoomMemberId(member), purpose) }
            let background = tiles["backgroundSourceId"] as? String ?? (tiles["style"] as? JSONObject)?["backgroundSourceId"] as? String
            add(zoomMemberId(background), purpose)   // the wall's live background is on air too
        }
        addFixedRoutes(input.programRoutes, programPurpose)
        addTiles(input.programTiles, programTilesPurpose)
        addFixedRoutes(input.previewRoutes, previewPurpose)
        addTiles(input.previewTiles, previewTilesPurpose)
        let wall = input.multiview.enumerated().sorted {
            let a = $0.element["slot"] as? Int ?? 0, b = $1.element["slot"] as? Int ?? 0
            return a != b ? a < b : $0.offset < $1.offset
        }
        for (_, source) in wall where isZoomMultiviewKind(source["kind"] as? String) {
            add(bareZoomId(source["participantId"] as? String), multiviewPurpose)
        }
        for id in input.iso { add(bareZoomId(id), isoPurpose) }
        // LAST: a sticky id any tier above lists keeps its video there.
        for id in input.stickyAudio { add(bareZoomId(id), tilesAudioPurpose, audioOnly: true) }
        return sources
    }

    static func decideVideo(_ sources: [Source], max: Int) -> (subscribed: [Source], overBudget: [Source]) {
        let cameraOn = sources.filter(\.videoOn)
        let budget = Swift.min(Swift.max(max, 0), cameraOn.count)
        return (Array(cameraOn.prefix(budget)), Array(cameraOn.dropFirst(budget)))
    }

    static func audioIds(_ sources: [Source]) -> [String] { sources.map(\.participantId) }
    static func speakerCandidateIds(_ sources: [Source]) -> [String] { sources.map(\.participantId) }
    static func isProgramPurpose(_ p: String) -> Bool { p == programPurpose || p == programTilesPurpose }
    static func isPreviewPurpose(_ p: String) -> Bool { p == previewPurpose || p == previewTilesPurpose }

    /// The Mac multiview names a Zoom tile `participant-video` (the core's
    /// resolveMultiviewFeed vocabulary); Windows names it `zoom`. Both count.
    static func isZoomMultiviewKind(_ kind: String?) -> Bool { kind == "zoom" || kind == "participant-video" }

    private static func isFollowSpeakerRoute(_ route: JSONObject) -> Bool {
        func blank(_ key: String) -> Bool { (route[key] as? String)?.trimmingCharacters(in: .whitespaces).isEmpty ?? true }
        return blank("participantId") && blank("captureDeviceId") && blank("mediaAssetId")
            && (route["mode"] as? String) == "active-speaker"
    }

    private static func fixedRouteParticipants(_ routes: [JSONObject]) -> Set<String> {
        Set(routes.filter { !isFollowSpeakerRoute($0) }.compactMap { bareZoomId($0["participantId"] as? String) })
    }

    /// A route/wall/ISO id: bare, or "zoom:"-qualified; other schemes are not Zoom participants.
    static func bareZoomId(_ id: String?) -> String? {
        guard let id, !id.trimmingCharacters(in: .whitespaces).isEmpty else { return nil }
        if id.hasPrefix("zoom:") { return String(id.dropFirst(5)) }
        return id.contains(":") ? nil : id
    }

    /// A Tiles member is always scheme-qualified.
    static func zoomMemberId(_ member: String?) -> String? {
        guard let member, member.hasPrefix("zoom:") else { return nil }
        return String(member.dropFirst(5))
    }

    // MARK: Spine payload parts (ZoomMediaSpinePayloadBuilder.Build)

    struct SpineParts {
        var subscriptions: [JSONObject]
        var sourceParticipantIds: [String]
        var shortfall: [JSONObject]
        var warnings: [String]
        var programNotice: String
        var previewNotice: String
        /// The multiview sources with the unsubscribed label applied.
        var multiviewSources: [JSONObject]
    }

    /// The parts of the Windows spine payload that follow from the source set.
    /// `names` maps a pid to its display name (else the pid is shown).
    static func spineParts(_ input: Input, names: [String: String], max: Int = maxVideoSubscriptions) -> SpineParts {
        let sources = resolve(input)
        let video = decideVideo(sources, max: max)
        func name(_ id: String) -> String { (names[id]).flatMap { $0.isEmpty ? nil : $0 } ?? id }
        var warnings: [String] = []
        if !video.overBudget.isEmpty {
            let n = video.overBudget.count
            warnings.append("No video: subscription limit (\(max)). \(n) routed camera\(n == 1 ? " is" : "s are") unsubscribed: "
                + video.overBudget.map { "\(name($0.participantId)) (\($0.purpose))" }.joined(separator: ", ") + ".")
        }
        func notice(_ onBus: (String) -> Bool) -> String {
            let listed = video.overBudget.filter { onBus($0.purpose) }.map { name($0.participantId) }
            return listed.isEmpty ? "" : "no video: \(listed.joined(separator: ", ")) (subscription limit \(max))"
        }
        let unsubscribed = Set(video.overBudget.map(\.participantId))
        let labelled: [JSONObject] = input.multiview.map { source in
            var s = source
            if isZoomMultiviewKind(source["kind"] as? String), let pid = source["participantId"] as? String,
               !pid.isEmpty, unsubscribed.contains(pid) {
                s["label"] = unsubscribedTileLabel(source["label"] as? String ?? "", max: max)
            }
            return s
        }
        return SpineParts(
            subscriptions: AppModel.spineSubscriptionPayloads(
                video.subscribed.map { Entry(participantId: $0.participantId, purpose: $0.purpose) },
                audioParticipantIds: audioIds(sources), meetingHasParticipants: !input.participants.isEmpty),
            sourceParticipantIds: speakerCandidateIds(sources),
            shortfall: video.overBudget.map { ["participantId": $0.participantId, "purpose": $0.purpose, "reason": "subscription-limit"] },
            warnings: warnings,
            programNotice: notice(isProgramPurpose), previewNotice: notice(isPreviewPurpose),
            multiviewSources: labelled)
    }

    /// `ZoomMediaSpinePayloadBuilder.UnsubscribedTileLabel`.
    static func unsubscribedTileLabel(_ label: String, max: Int) -> String {
        "\(label) · no video: subscription limit (\(max))"
    }
}
