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

    /// Windows `StudioViewModel.EditTiles`: only a Tiles scene; cue it on Preview
    /// (Program never changes) and open the Tiles workspace on the Scenes tab.
    func editTiles(_ sceneId: String) {
        guard let scene = scenes.first(where: { $0.id == sceneId }), scene.tiles != nil else { return }
        // A wall on Program is not edited in place (Program never changes without
        // a Take; S4 review): say so instead of cueing Program onto Preview.
        if sceneId == programSceneId {
            studioCommandStatus = "\(scene.name) is on Program. Queue it in Preview to edit it."
            return
        }
        selectPreviewScene(sceneId)
        guard previewSceneId == sceneId else { return }
        selectedTab = .scenes
        studioCommandStatus = "Editing Tiles in \(scene.name)"
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

// MARK: - Tiles membership actions (Task 14): the PREVIEW Tiles scene only

enum TilesEditError: Error, Equatable {
    case notTilesPreview, slotOutOfRange, unavailableSource, slotsFull

    /// The Windows strings (StudioViewModel.Tiles.cs / SceneWorkspace.cs).
    var message: String {
        switch self {
        case .notTilesPreview: return "Queue a CoreVideo Tiles scene in Preview first."
        case .slotOutOfRange: return "Tiles slot must be 1–64."
        case .unavailableSource: return "Choose an available source for this Tiles slot."
        case .slotsFull: return "Clear a manual slot before adding another member; a wall supports 64 slots."
        }
    }
}

extension AppModel {
    /// The Preview scene when it is an editable Tiles scene (never Program).
    var tilesEditTargetId: String? {
        guard let id = sceneEditTargetId, isTilesScene(id) else { return nil }
        return id
    }

    /// Windows `GalleryMemberChoices`: video-on guests, then in-show local sources.
    var tilesMemberChoices: [TilesMemberChoice] {
        TilesMemberChoicePolicy.build(roster: roster,
                                      local: TilesLocalSourcePolicy.localVideoSources(slots, devices: captureDevices))
    }

    /// Edits the Preview wall, then syncs scenes. Refuses (status + throw) when
    /// Preview is not a Tiles scene; nothing is synced then.
    /// `save: .now` (discrete actions: membership intent is saved at once, S3
    /// review); `.debounced` for continuous controls, so a 40-tick slider drag
    /// writes the prefs file once, after the drag settles.
    enum TilesSave { case now, debounced }

    func updateTiles(save: TilesSave = .now, _ edit: (inout TilesSettings) -> Void) throws {
        guard let id = tilesEditTargetId, let index = scenes.firstIndex(where: { $0.id == id }),
              var settings = scenes[index].tiles else { throw refuseTiles(.notTilesPreview) }
        // Windows hit a stack overflow from a two-way binding echo re-entering the
        // gallery setter (StudioViewModel `_galleryEditGuard`). A re-entrant edit is
        // dropped here, never recursed into; the depth is observable for the test.
        guard tilesEditDepth == 0 else { return }
        tilesEditDepth += 1
        defer { tilesEditDepth -= 1 }
        tilesEditMaxDepth = max(tilesEditMaxDepth, tilesEditDepth)
        edit(&settings)
        scenes[index].tiles = settings
        syncScenes()   // debounced (120 ms): a drag sends one sync with the last value
        switch save {
        case .now: saveScenes()
        case .debounced: scheduleTilesSave()
        }
    }

    private func scheduleTilesSave() {
        tilesSaveTask?.cancel()
        tilesSaveTask = Task { [weak self] in
            await self?.timerSleep(nanoseconds: 500_000_000)
            guard !Task.isCancelled else { return }
            self?.saveScenes()
        }
    }

    // MARK: Layout tab setters (Windows Gallery* setters, clamp for clamp)

    /// C# `Math.Round` (banker's rounding) then clamp.
    private static func windowsRound(_ v: Double) -> Double { v.rounded(.toNearestOrEven) }

    func setTilesMaxTiles(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.maxTiles = Int(min(max(Self.windowsRound(v), 1), 64)) }
    }
    func setTilesAspect(_ v: String) throws { try updateTiles { $0.tileAspect = TilesLayerPayload.normalizeAspect(v) } }
    func setTilesCustomAspect(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.customAspectRatio = min(max(v, 0.25), 4) }
    }
    func setTilesGutter(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.gutterPercent = min(max(v, 0), 10) }
    }
    func setTilesMargin(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.marginPercent = min(max(v, 0), 20) }
    }
    func setTilesAnimateLayout(_ on: Bool) throws { try updateTiles { $0.animateLayout = on } }
    func setTilesAnimationDuration(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.animationDurationMs = Int(min(max(Self.windowsRound(v), 100), 2000)) }
    }

    /// Windows `GalleryTileAspectOptions`.
    static let tilesAspectOptions: [(value: String, label: String)] = [
        ("16:9", "16:9"), ("4:3", "4:3"), ("5:4", "5:4"), ("1:1", "Square"),
        ("3:4", "Portrait 3:4"), ("9:16", "Portrait 9:16"), ("custom", "Custom"),
    ]

    @discardableResult
    private func refuseTiles(_ error: TilesEditError) -> TilesEditError {
        studioCommandStatus = error.message
        return error
    }

    /// `GalleryMembershipMode` setter: also `autoFill = mode != "manual"`.
    func setTilesMembershipMode(_ mode: String) throws {
        try updateTiles { s in
            s.membershipMode = TilesMembershipPolicy.normalizeMode(mode)
            s.autoFill = s.membershipMode != "manual"
        }
    }

    /// `AssignTilesSlot` (StudioViewModel.Tiles.cs): slot 1-64, "" clears; the
    /// guest MOVES (any slot already holding them is cleared), saved as a stable
    /// identity, and the wall is bound to the current meeting.
    func assignTilesSlot(_ slot: Int, sourceId: String) throws {
        guard tilesEditTargetId != nil else { throw refuseTiles(.notTilesPreview) }
        guard (1...64).contains(slot) else { throw refuseTiles(.slotOutOfRange) }
        if !sourceId.isEmpty, !TilesMemberChoicePolicy.contains(tilesMemberChoices, sourceId) { throw refuseTiles(.unavailableSource) }
        let roster = tilesIdentityRoster, meetingId = currentTilesMeetingId
        try updateTiles { s in
            let persisted: String? = sourceId.isEmpty ? nil : TilesIdentityPolicy.persist(sourceId, roster: roster)
            if let meetingId { s.boundMeetingId = meetingId }
            while s.manualSlots.count < slot { s.manualSlots.append(nil) }
            for index in s.manualSlots.indices {
                let existing = s.manualSlots[index]
                if existing == sourceId || existing == persisted
                    || TilesIdentityPolicy.resolveLiveSourceId(existing, roster: roster, currentMeetingId: meetingId,
                                                               boundMeetingId: s.boundMeetingId) == sourceId {
                    s.manualSlots[index] = nil
                }
            }
            s.manualSlots[slot - 1] = persisted
        }
    }

    /// `SetTilesExclusion` (never show).
    func setTilesExclusion(_ sourceId: String, excluded: Bool) throws {
        guard tilesEditTargetId != nil else { throw refuseTiles(.notTilesPreview) }
        // Windows: a blank id is "Choose a source to exclude or allow."; an
        // unavailable one to exclude is "Choose an available source to exclude."
        if sourceId.trimmingCharacters(in: .whitespaces).isEmpty {
            studioCommandStatus = "Choose a source to exclude or allow."
            throw TilesEditError.unavailableSource
        }
        if excluded, !TilesMemberChoicePolicy.contains(tilesMemberChoices, sourceId) {
            studioCommandStatus = "Choose an available source to exclude."
            throw TilesEditError.unavailableSource
        }
        let roster = tilesIdentityRoster, meetingId = currentTilesMeetingId
        try updateTiles { s in
            let persisted = TilesIdentityPolicy.persist(sourceId, roster: roster)
            if let meetingId { s.boundMeetingId = meetingId }
            s.excludedSourceIds.removeAll { id in
                id == sourceId || id == persisted
                    || TilesIdentityPolicy.resolveLiveSourceId(id, roster: roster, currentMeetingId: meetingId,
                                                               boundMeetingId: s.boundMeetingId) == sourceId
            }
            if excluded { s.excludedSourceIds.append(persisted) }
        }
    }

    /// `WorkspaceIncludeMember` (StudioViewModel.SceneWorkspace.cs): eligible mode
    /// ignores it; manual mode seats a newly included member in the first empty
    /// slot (or a new one, up to 64); the omitted list follows the choice.
    func includeTilesMember(_ sourceId: String, included: Bool) throws {
        guard let id = tilesEditTargetId, let wall = scenes.first(where: { $0.id == id })?.tiles else {
            throw refuseTiles(.notTilesPreview)
        }
        let mode = TilesMembershipPolicy.normalizeMode(wall.membershipMode)
        if mode == "eligible" { return }
        if mode == "manual", included,
           !((tilesPayload(for: id)?["members"] as? [String])?.contains(sourceId) ?? false) {
            let empty = wall.manualSlots.firstIndex { ($0 ?? "").isEmpty }
            if empty == nil, wall.manualSlots.count >= 64 { throw refuseTiles(.slotsFull) }
            try assignTilesSlot(empty.map { $0 + 1 } ?? wall.manualSlots.count + 1, sourceId: sourceId)
        }
        let roster = tilesIdentityRoster, meetingId = currentTilesMeetingId
        try updateTiles { s in
            if let meetingId { s.boundMeetingId = meetingId }
            s.omittedSourceIds.removeAll { saved in
                saved == sourceId || TilesIdentityPolicy.resolveLiveSourceId(saved, roster: roster, currentMeetingId: meetingId,
                                                                             boundMeetingId: s.boundMeetingId) == sourceId
            }
            if !included { s.omittedSourceIds.append(TilesIdentityPolicy.persist(sourceId, roster: roster)) }
        }
    }

    /// `ClearStaleTilesEntries`.
    func clearStaleTiles() throws {
        let roster = tilesIdentityRoster, meetingId = currentTilesMeetingId, live = tilesLiveLocalSourceIds
        try updateTiles { s in TilesIdentityPolicy.clearStale(&s, roster: roster, meetingId: meetingId, liveLocal: live) }
    }

    /// Windows `TilesLiveLocalSourceIds`: the in-show local sources the wall can seat now.
    var tilesLiveLocalSourceIds: Set<String> {
        Set(TilesLocalSourcePolicy.localVideoSources(slots, devices: captureDevices).map(\.id))
    }

    /// `GalleryMembershipSummary`: "Manual slots: … · Never show: …" (+ " · Stale: …").
    var tilesMembershipSummary: String {
        guard let id = tilesEditTargetId, let s = scenes.first(where: { $0.id == id })?.tiles else { return "" }
        let roster = tilesIdentityRoster, choices = tilesMemberChoices
        func name(_ id: String) -> String { choices.first { $0.id == id }?.name ?? TilesIdentityPolicy.label(id, roster: roster) }
        let slots = s.manualSlots.enumerated().compactMap { index, id -> String? in
            guard let id, !id.isEmpty else { return nil }
            return "\(index + 1): \(name(id))"
        }
        let summary = "Manual slots: " + slots.joined(separator: ", ") + " · Never show: " + s.excludedSourceIds.map(name).joined(separator: ", ")
        let stale = TilesIdentityPolicy.describeStale(s, roster: roster, meetingId: currentTilesMeetingId, liveLocal: tilesLiveLocalSourceIds)
        return stale.isEmpty ? summary : summary + " · Stale: " + stale.map(\.label).joined(separator: ", ")
    }

    /// Windows `WorkspaceMemberStates`.
    var tilesMemberStates: [String: TilesMemberState] {
        guard let id = tilesEditTargetId else { return [:] }
        return TilesWorkspaceMembership.build(scenes.first { $0.id == id }?.tiles, members: tilesMemberChoices.map(\.id),
                                              routed: TilesLocalSourcePolicy.routedSourceIds(slots),
                                              roster: tilesIdentityRoster, meetingId: currentTilesMeetingId)
    }
}

// MARK: - Style tab (Task 17): Windows Gallery* style setters, presets and reset

enum TilesStylePreset: String, CaseIterable { case clean, rounded, broadcast }

/// Which colour a `TilesColorField` edits, and the picker's fallback when the hex
/// does not parse (Windows `HexColor.ParseOrDefault`: black for background and
/// border, white for glow).
enum TilesColorRole {
    case background, border, glow
    var fallback: (r: UInt8, g: UInt8, b: UInt8) { self == .glow ? (255, 255, 255) : (0, 0, 0) }
}

extension AppModel {
    func setTilesBorderShape(_ v: String) throws { try updateTiles { $0.borderShape = v == "rounded" ? "rounded" : "square" } }
    func setTilesCornerRadius(_ v: Double) throws {
        try updateTiles(save: .debounced) { $0.cornerRadius = v.isFinite ? min(max(v, 0), 100) : 16 }
    }
    func setTilesBorderThickness(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.borderThickness = min(max(v, 0), 32) }
    }
    func setTilesGlowSize(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.glowSize = min(max(v, 0), 64) }
    }
    func setTilesGlowIntensity(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.glowIntensity = min(max(v, 0), 100) }
    }
    func setTilesGlowSoftness(_ v: Double) throws {
        guard v.isFinite else { return }
        try updateTiles(save: .debounced) { $0.glowSoftness = min(max(v, 0), 100) }
    }
    /// `OnTilesTransparency` sets it from the checkbox.
    func setTilesTransparentBackground(_ on: Bool) throws { try updateTiles { $0.transparentBackground = on } }
    /// `WallOpacityPercent` (SourcesPage.Workspace.cs): 0-100 only, stored as v/100 (D7).
    func setTilesWallOpacityPercent(_ v: Double) throws {
        guard v.isFinite, (0...100).contains(v) else { return }
        try updateTiles(save: .debounced) { $0.wallOpacity = v / 100 }
    }

    // The hex string is the ONLY colour truth: the text field commits through the
    // Windows normalizer, and the picker reads/writes through TilesHex.
    func tilesColor(_ role: TilesColorRole) -> String {
        let s = tilesEditSettings
        switch role {
        case .background: return s.backgroundColor
        case .border: return s.borderColor
        case .glow: return s.glowColor
        }
    }
    /// The text field's commit (Windows setters normalize every colour).
    func setTilesColor(_ role: TilesColorRole, hex: String, save: TilesSave = .now) throws {
        let value = TilesHex.normalize(hex)
        try updateTiles(save: save) { s in
            switch role {
            case .background: s.backgroundColor = value
            case .border: s.borderColor = value
            case .glow: s.glowColor = value
            }
        }
    }
    /// What the picker shows: the parsed hex, else the role's fallback.
    func tilesPickerColor(_ role: TilesColorRole) -> (r: UInt8, g: UInt8, b: UInt8) {
        TilesHex.parse(tilesColor(role)) ?? role.fallback
    }
    /// The picker's write (Windows `HexColor.ToHex`).
    func setTilesPickerColor(_ role: TilesColorRole, r: UInt8, g: UInt8, b: UInt8) throws {
        // The colour panel streams changes while dragged: save once it settles.
        try setTilesColor(role, hex: TilesHex.format(r: r, g: g, b: b), save: .debounced)
    }

    /// `OnTilesPreset` (SourcesPage.Workspace.cs) verbatim. Membership, crops,
    /// overrides and capacity are untouched.
    func applyTilesPreset(_ preset: TilesStylePreset) throws {
        try updateTiles { w in
            w.glowSize = 0; w.gutterPercent = 0.741; w.marginPercent = 0.741
            if preset == .clean {
                w.borderShape = "square"; w.borderThickness = 0; w.transparentBackground = true
            } else {
                w.borderShape = "rounded"; w.transparentBackground = false
                w.cornerRadius = preset == .broadcast ? 12 : 24
                w.borderThickness = preset == .broadcast ? 5 : 2
                w.borderColor = preset == .broadcast ? "#8AE0CA" : "#657D87"
                w.gutterPercent = preset == .broadcast ? 1.8 : 1.2; w.marginPercent = w.gutterPercent
                w.glowSize = preset == .broadcast ? 12 : 0
            }
        }
    }

    /// `OnTilesResetStyle` verbatim.
    func resetTilesStyle() throws {
        try updateTiles { w in
            w.borderShape = "rounded"; w.borderColor = "#657D87"; w.borderThickness = 2; w.cornerRadius = 16
            w.glowColor = "#8AE0CA"; w.glowSize = 0; w.glowIntensity = 100; w.glowSoftness = 0
            w.backgroundColor = "#101820"; w.transparentBackground = false; w.wallOpacity = 1
        }
    }
}
