// LowerThirdAuthority — the ONE lower-third entry (graphics G3, spec 5.5, plan D8).
// Port of W/ViewModels/StudioViewModel.StudioLowerThird.cs:49-72
// (ApplyStudioLowerThirdIntentAsync) minus the "assigned studio look" branch,
// which has no Mac UI until G8:
//
//   * A Guest row and (Show, or authority already `instances`) -> the playlist
//     (GraphicsPlayoutService.ApplyLowerThirdIntentAsync, :146-167). Lower-third
//     authority is adopted (`graphics-authority-set lower-third instances`) only
//     AFTER an accepted Show: flipping earlier erases the legacy key with nothing
//     to replace it.
//   * Authority `instances` without a Guest row -> the managed lower thirds
//     (ApplyManagedLowerThirdIntentAsync, StudioViewModel.GraphicsCompatibility.cs:49):
//     hide them, or show a cued one. Hide never goes to the legacy key while
//     instances own Program (#1166).
//   * Otherwise -> the legacy key (set-overlay-asset key:lower-third).
//
// Every accepted Hide sets the automation-suppression flag and every accepted
// Show clears it. Nothing reads the flag while Auto is retired on Mac; it is
// recorded so the routing is right the day Auto returns (spec 5.5).
// Callers: the footer L3 button, the drawer's Key in/out, and the control
// actions graphics.lowerThird.toggle / .set.

import Foundation

enum LowerThirdTarget: Equatable { case playlist, managed, legacy }

enum LowerThirdAuthority {
    static func target(visible: Bool, hasGuestRow: Bool, authorityInstances: Bool) -> LowerThirdTarget {
        if hasGuestRow && (visible || authorityInstances) { return .playlist }
        if authorityInstances { return .managed }
        return .legacy
    }

    /// LowerThirdIntentVisible: the toggle follows whoever renders Program now.
    static func intentVisible(authorityInstances: Bool, hasGuestRow: Bool, playlistOnAir: Bool,
                              managedOnAir: Bool, legacyOnAir: Bool) -> Bool {
        authorityInstances ? (hasGuestRow ? playlistOnAir : managedOnAir) : legacyOnAir
    }

    static let curatedLowerThirds: Set<String> = ["compact-solid", "minimal-accent", "broadcast"]

    /// GraphicsCompatibilityPolicy.LowerThirdsForScene.
    static func lowerThirds(_ state: GraphicsState?, sceneId: String) -> [GraphicsInstance] {
        guard let state else { return [] }
        return state.instances.filter { i in
            (i.scope == "global" || i.sceneId == sceneId) && (curatedLowerThirds.contains(i.templateId)
                || state.templates.contains { $0.templateId == i.templateId && $0.templateVersion == i.templateVersion && $0.family == "lower-third" })
        }
    }
}

extension GraphicsPlayout {
    /// GraphicsPlayoutService.ApplyLowerThirdIntentAsync (:146-167).
    func applyLowerThirdIntent(visible: Bool, subject: GraphicsPlaylistEntry?, authorityIsInstances: Bool,
                               adopt: @escaping () async -> Bool) async -> GraphicsOperationResult {
        let stripRows = rows().map(\.stripRow)
        if !visible {
            let targets = (observe()?.instances ?? []).filter {
                $0.isOnProgram && GraphicsStrip.row(of: $0.instanceId, in: stripRows)?.kind == GraphicsRow.guest
            }.map(\.instanceId)
            if targets.isEmpty { return GraphicsOperationResult(accepted: true, reason: "No playlist lower third is on Program.") }
            var result = GraphicsOperationResult(accepted: true)
            for id in targets {
                result = await hide(id)
                if !result.accepted { return result }
            }
            result.reason = "Lower third hidden."
            return result
        }
        guard let subject else {
            return refuse("Lower third needs a Program subject bound to a Show Input. Choose a guest in the playlist instead.")
        }
        if let current = observe()?.instance(subject.instanceId), current.isOnProgram {
            return GraphicsOperationResult(instanceId: subject.instanceId, currentRevision: current.revision, accepted: true,
                                           reason: "The lower third is already on air.")
        }
        var played = await show(subject)
        if !played.accepted || authorityIsInstances { return played }
        let adopted = await adopt()
        played.reason = adopted ? "Shown; lower thirds now follow the Graphics playlist."
            : played.reason + " Lower-third authority is still legacy; retry to adopt it."
        return played
    }

    /// ApplyManagedLowerThirdIntentAsync without the rebuild path (no Mac UI for it).
    func applyManagedLowerThirdIntent(visible: Bool, sceneId: String) async -> GraphicsOperationResult {
        await run {
            let state = try self.available()
            self.epoch = state.coreEpoch
            let targets = LowerThirdAuthority.lowerThirds(state, sceneId: sceneId)
            if !visible {
                var result = GraphicsOperationResult(accepted: true, reason: "Managed lower thirds are hidden.")
                for item in targets where item.programRevision > 0 || item.showPending {
                    result = try await self.sendAs("legacy-lower-third", "graphics.instance.trigger",
                                                   ["instanceId": item.instanceId, "expectedRevision": item.revision, "action": "hide"])
                    if !result.accepted { break }
                }
                return result
            }
            if targets.contains(where: \.isOnProgram) {
                return GraphicsOperationResult(accepted: true, reason: "Managed lower third is already on air.")
            }
            guard let item = targets.sorted(by: { $0.cueRevision > $1.cueRevision }).first, item.cueRevision > 0 else {
                return self.refuse("Prepare a lower-third Cue before using the existing Show control.")
            }
            return try await self.sendAs("legacy-lower-third", "graphics.instance.trigger",
                                         ["instanceId": item.instanceId, "expectedRevision": item.cueRevision, "target": "cue", "action": "show"])
        }
    }
}

extension GraphicsModel {
    var authorityInstances: Bool { state?.familyAuthorities["lower-third"] == "instances" }
    /// GraphicsPlayoutService.IsLowerThirdOnAir: a Program chip that is a Guest-row
    /// instance, or any other instance whose template is a lower third (curated, or
    /// family lower-third). Strip truth, never local intent.
    var lowerThirdOnAir: Bool {
        let stripRows = rows.map(\.stripRow)
        return strip.onProgram.contains { item in
            if let row = GraphicsStrip.row(of: item.instanceId, in: stripRows) { return row.kind == GraphicsRow.guest }
            guard let state, let instance = state.instance(item.instanceId) else { return false }
            return LowerThirdAuthority.curatedLowerThirds.contains(instance.templateId)
                || state.templates.contains { $0.templateId == instance.templateId && $0.templateVersion == instance.templateVersion && $0.family == "lower-third" }
        }
    }

    /// graphics-authority-set lower-third instances, as Windows' workspace client.
    func adoptInstancesAuthority() async -> Bool {
        guard let epoch = state?.coreEpoch, !epoch.isEmpty else { return false }
        let result = await gateway.invoke("graphics.authority.set", [
            "schemaVersion": 1, "clientId": "graphics-workspace", "coreEpoch": epoch,
            "requestId": "workspace-" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: ""),
            "family": "lower-third", "authority": "instances",
        ])
        return result.accepted
    }
}

extension AppModel {
    /// The toggle's current answer (Windows LowerThirdIntentVisible).
    var lowerThirdIntentVisible: Bool {
        let g = graphics
        return LowerThirdAuthority.intentVisible(
            authorityInstances: g.authorityInstances, hasGuestRow: g.hasGuestRow, playlistOnAir: g.lowerThirdOnAir,
            managedOnAir: LowerThirdAuthority.lowerThirds(g.state, sceneId: programSceneId).contains(where: \.isOnProgram),
            legacyOnAir: lowerThirdPhase == "on-air" || lowerThirdPhase == "building-in")
    }

    func toggleLowerThirdIntent() {
        let target = !lowerThirdIntentVisible
        Task { @MainActor in _ = await self.applyLowerThirdIntent(visible: target) }
    }

    /// THE lower-third entry (spec 5.5). Returns accepted intent, never "the pixels landed".
    @discardableResult
    func applyLowerThirdIntent(visible: Bool) async -> GraphicsOperationResult {
        let g = graphics
        let authority = g.available && g.authorityInstances
        switch LowerThirdAuthority.target(visible: visible, hasGuestRow: g.available && g.hasGuestRow, authorityInstances: authority) {
        case .playlist:
            let subject = g.guestEntry(for: programLowerThirdSubject()?.source.sourceId)
            let result = await g.playout.applyLowerThirdIntent(visible: visible, subject: subject, authorityIsInstances: authority) {
                await g.adoptInstancesAuthority()
            }
            g.report(result)
            lowerThirdCommandStatus = result.reason
            if result.accepted { g.lowerThirdAutomationSuppressed = !visible }
            return result
        case .managed:
            let result = await g.playout.applyManagedLowerThirdIntent(visible: visible, sceneId: programSceneId)
            g.report(result)
            lowerThirdCommandStatus = result.reason
            if result.accepted { g.lowerThirdAutomationSuppressed = !visible }
            return result
        case .legacy:
            // Show only when the key is not already up (a second building-in would
            // re-animate an on-air key). Hide always: hideLowerThird is idempotent
            // while a retire is in flight, and the Mac has no shell-side "enabled"
            // flag (Windows ProgramLowerThirdEnabled) to tell a just-sent key-in
            // from nothing, so a Hide right after a Show must still go out.
            let current = lowerThirdPhase == "on-air" || lowerThirdPhase == "building-in"
            if visible { if !current { showLowerThird() } } else { hideLowerThird() }
            let applied = visible ? lowerThirdCommandStatus != LowerThirdKeyReadout.needsSource : true
            if applied { g.lowerThirdAutomationSuppressed = !visible }
            return GraphicsOperationResult(accepted: applied, reason: lowerThirdCommandStatus)
        }
    }
}
