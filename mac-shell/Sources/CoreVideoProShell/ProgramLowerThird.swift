// ProgramLowerThird — the manual lower third keys the PROGRAM SOURCE, as on
// Windows (Tier 0.6; StudioViewModel.LegacyLowerThirdCommands.cs:7-45,
// RenderedLowerThirdSourceResolver.cs, StudioViewModel.cs:13875-13909).
//
// The Mac used to key a typed name under sourceId "mac:manual". Since
// 2026-09-06 (d6b5f740) the core drops any lower third whose non-empty sourceId
// matches no video layer on Program (MediaCore.cpp, the "bind lower thirds to
// rendered Program" gate), and no layer is ever "mac:manual" — so the key was
// never drawn while the Overlays tab said "Key on air". Now the key carries the
// id of a source the core itself reports as rendered on Program
// (programFrame.videoSources), and the on-air readout comes from the core's
// overlayState `visible` flag, not from the request.

import Foundation

enum ProgramLowerThirdSource {
    struct Source: Equatable {
        let sourceId: String
        let kind: String
        let participantId: String
    }

    /// Port of Windows RenderedLowerThirdSourceResolver.Resolve: the subject is
    /// chosen from the layers the core attributes to the frame Program actually
    /// rendered, never from route intent.
    static func resolve(programFrame: JSONObject?, programSceneId: String,
                        stickySourceId: String, rosterWithVideo: Set<String>,
                        captureDeviceIds: Set<String>) -> Source? {
        guard let frame = programFrame,
              ((frame["frameNumber"] as? NSNumber)?.int64Value ?? 0) > 0,
              (frame["sceneId"] as? String ?? "") == programSceneId,
              let sources = frame["videoSources"] as? [JSONObject] else { return nil }
        // A transition's attribution contains both pictures; only the incoming
        // one is the subject.
        let incomingTake = (frame["renderPlanId"] as? String ?? "").hasPrefix("take:")
        let eligible: [Source] = sources.compactMap { entry in
            let source = Source(sourceId: entry["sourceId"] as? String ?? "",
                                kind: entry["kind"] as? String ?? "",
                                participantId: entry["participantId"] as? String ?? "")
            if incomingTake, !(entry["layerId"] as? String ?? "").hasPrefix("take-in:") { return nil }
            if source.kind == "media-video", source.sourceId.hasPrefix("media:") { return source }
            guard source.kind == "participant-video" || source.kind == "screen-share" else { return nil }
            if source.sourceId.hasPrefix("capture:") {
                return captureDeviceIds.contains(String(source.sourceId.dropFirst(8))) ? source : nil
            }
            return rosterWithVideo.contains(source.participantId)
                && source.sourceId == "zoom:" + source.participantId ? source : nil
        }
        return eligible.first { $0.sourceId == stickySourceId }
            ?? eligible.first { $0.kind == "participant-video" }
            ?? eligible.first { $0.kind == "media-video" }
            ?? eligible.first
    }

    /// The video kinds the core's on-Program gate accepts as a lower third's
    /// subject. Must stay a superset of what `resolve` can return (pinned by a
    /// test that reads the gate out of MediaCore.cpp).
    static let subjectKinds: Set<String> = ["participant-video", "screen-share", "media-video"]
}

/// What the Overlays tab says about the key, from the core's overlayState.
enum LowerThirdKeyReadout {
    static let syncPending = "Lower third sync pending — media core has not confirmed the key"
    static let needsSource = "Lower third needs a program source"

    static func status(phase: String, visible: Bool, sourceName: String) -> String {
        switch phase {
        case "hidden", "": return "Key off — no source keyed"
        case "building-out": return visible ? "Building out — \(sourceName)" : "Key off — no source keyed"
        default:
            guard visible else { return syncPending }
            return (phase == "building-in" ? "Building in — " : "Key on air — ") + sourceName
        }
    }
}

extension AppModel {
    /// The Program source the key would name now, with its display name — the
    /// Sources name for an assigned slot (Windows ResolveSourceDisplayName),
    /// else the roster / device / media name.
    func programLowerThirdSubject() -> (source: ProgramLowerThirdSource.Source, name: String)? {
        guard let source = ProgramLowerThirdSource.resolve(
            programFrame: lastSnapshot["programFrame"] as? JSONObject,
            programSceneId: programSceneId,
            stickySourceId: lowerThirdKeyedSourceId,
            rosterWithVideo: Set(roster.filter(\.hasVideo).map(\.id)),
            captureDeviceIds: Set(captureDevices.map(\.id))) else { return nil }
        return (source, lowerThirdDisplayName(for: source))
    }

    func lowerThirdDisplayName(for source: ProgramLowerThirdSource.Source) -> String {
        if let slot = slots.first(where: { $0.sourceKey == source.sourceId }), !slot.name.isEmpty {
            return slot.name
        }
        if source.sourceId.hasPrefix("capture:"),
           let device = captureDevices.first(where: { "capture:" + $0.id == source.sourceId }) {
            return device.name
        }
        if source.sourceId.hasPrefix("media:") {
            let assetId = String(source.sourceId.dropFirst(6))
            return mediaAssets.first { $0.id == assetId }?.name ?? "Media"
        }
        return roster.first { $0.id == source.participantId }?.name ?? source.participantId
    }

    /// The Overlays tab / footer readout: what the core reports, not the request.
    var lowerThirdKeyStatus: String {
        LowerThirdKeyReadout.status(phase: lowerThirdPhase, visible: lowerThirdVisible,
                                    sourceName: lowerThirdKeyedName)
    }
}
