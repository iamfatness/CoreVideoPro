// CoreVideo Tiles on the AppModel: state and actions, kept out of AppModel.swift
// so that file only changes at call sites (docs/superpowers/plans/2026-11-22-mac-tiles.md).

import Foundation

extension AppModel {
    /// The Windows layout discriminator for a Tiles scene (`Scene.Layout`).
    static let tilesLayout = "dynamic-gallery"
    /// Windows `StarterSceneFactory` name for the Tiles starter.
    static let tilesStarterName = "CoreVideo Tiles"
    /// Mac-only copy (owner decision D5): Windows can stack layers over a wall;
    /// the Mac cannot until S6's layer model, so it refuses rather than sending
    /// routes beside the wall (the core warns about and interleaves those).
    static let tilesLayersRefusal = "Layers over a CoreVideo Tiles wall are not on Mac yet. Use Edit Tiles."

    func isTilesScene(_ sceneId: String) -> Bool {
        scenes.first { $0.id == sceneId }?.tiles != nil
    }

    /// The `tiles` node for a scene's graph, from the current roster, slots and
    /// devices; nil for a scene that is not a Tiles scene. The identity roster
    /// is the live roster (Windows passes `RoomParticipantsForInputs`).
    func tilesPayload(for sceneId: String) -> JSONObject? {
        guard let settings = scenes.first(where: { $0.id == sceneId })?.tiles else { return nil }
        return TilesLayerPayload.build(
            sceneId: sceneId, settings: settings, roster: roster,
            localVideoSourceIds: TilesLocalSourcePolicy.localVideoSources(slots, devices: captureDevices).map(\.id),
            routedSourceIds: TilesLocalSourcePolicy.routedSourceIds(slots),
            identityRoster: tilesIdentityRoster, meetingId: currentTilesMeetingId)
    }

    /// The FULL roster, camera-off included (Windows `RoomParticipantsForInputs`):
    /// saved identities resolve against everyone in the room.
    var tilesIdentityRoster: [TilesIdentityPolicy.Person] { roster.map(TilesIdentityPolicy.person(from:)) }

    /// Windows `CurrentTilesMeetingId`: the meeting number being joined, nil when blank.
    var currentTilesMeetingId: String? {
        let id = Self.parseZoomTarget(joinMeetingId, fallbackPasscode: joinPasscode).meetingId
        return id.isEmpty ? nil : id
    }

    /// Windows `MakeUniqueSceneName`: the base name, else " 2", " 3"…, compared
    /// case-insensitively.
    static func uniqueSceneName(_ base: String, existing: [String]) -> String {
        let taken = Set(existing.map { $0.lowercased() })
        if !taken.contains(base.lowercased()) { return base }
        var suffix = 2
        while taken.contains("\(base) \(suffix)".lowercased()) { suffix += 1 }
        return "\(base) \(suffix)"
    }

    /// The Tiles starter: a new wall with the Windows defaults, cued to Preview
    /// only. Program never changes without a Take.
    func newTilesScene() {
        var newId = "scene-\(Int(Date().timeIntervalSince1970))"
        while scenes.contains(where: { $0.id == newId }) { newId += "-t" }
        let name = Self.uniqueSceneName(Self.tilesStarterName, existing: scenes.map(\.name))
        scenes.append(SceneDef(id: newId, name: name, layout: Self.tilesLayout, tiles: TilesSettings()))
        previewSceneId = newId
        studioCommandStatus = "\(name) created and cued on preview"
        syncScenes()
        saveScenes()
    }

    /// True (and the refusal shown) when a layer edit targets a Tiles scene.
    func refuseLayerEditOnTiles(_ sceneId: String) -> Bool {
        guard isTilesScene(sceneId) else { return false }
        studioCommandStatus = Self.tilesLayersRefusal
        return true
    }

    /// The latch's meeting state: nil until a Zoom snapshot carrying a non-empty
    /// `meetingState` arrived on THIS core generation, else whether it is
    /// "in_meeting" (Windows reads `_bridge.LastSnapshot?.MeetingState`, null
    /// for a synthesized snapshot).
    var tilesLatchMeetingState: Bool? {
        tilesMeetingStateObserved ? meetingState == "in_meeting" : nil
    }

    /// Windows `MultiviewOverlayFormatting.WithBusNotice`: "PROGRAM"/"PREVIEW",
    /// plus " · <text after the first ' · '>" when the core appended a notice.
    nonisolated static func busCaption(role: String, label: String) -> String {
        let caption = role == "pgm" ? "PROGRAM" : role == "pvw" ? "PREVIEW" : label
        let separator = " \u{00B7} "
        guard role == "pgm" || role == "pvw", let range = label.range(of: separator) else { return caption }
        return caption + separator + String(label[range.upperBound...])
    }
}
