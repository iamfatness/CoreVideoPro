// Phase 4 leftovers (docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md,
// spec C5 rows). Own file so the parallel lanes that extend ShellTests.swift do not collide.
// Each test names the mutation it must turn red (plan-authoring rule 7).

import Foundation

extension ShellTests {

    @MainActor
    static var leftoverCases: [(String, () -> Void)] {
        [
            ("leftovers/multiview-overlay-defaults", testMultiviewOverlayDefaults),
            ("leftovers/multiview-overlay-command", testMultiviewOverlayCommand),
            ("leftovers/multiview-program-meter", testMultiviewProgramMeter),
            ("leftovers/starter-removed-set-persists", testStarterRemovedSetPersists),
        ]
    }

    /// B24/E5/G21: Windows defaults (ProductionOutputPreferencesStore.cs:160-163) and the
    /// main-view header clock hidden by default, which changes the old always-on clock.
    /// Mutation: default showClock = true.
    @MainActor
    static func testMultiviewOverlayDefaults() {
        let model = AppModel()
        expectEqual(model.multiviewOverlays,
                    MultiviewOverlays(showLabels: true, showTally: true, showMeters: true, showClock: false),
                    "fresh overlays are labels, tally, meters on; clock off")
        expect(!MultiviewerCanvas.showsHeaderClock(isPopout: false, overlays: model.multiviewOverlays),
               "the main header clock is hidden by default")
        expect(MultiviewerCanvas.showsHeaderClock(isPopout: false, overlays: MultiviewOverlays(showClock: true)),
               "Clock on shows it")
        expect(!MultiviewerCanvas.showsHeaderClock(isPopout: true, overlays: MultiviewOverlays(showClock: true)),
               "the pop-out draws its clock on the canvas, not in the header")
        let restored = AppModel()
        var prefs = ShellPrefs()
        prefs.multiviewOverlays = MultiviewOverlays(showLabels: false, showTally: true, showMeters: false, showClock: true)
        restored.applyRestoredPrefs(prefs)
        expectEqual(restored.multiviewConfigCommand()["showLabels"] as? Bool, false,
                    "restored prefs reach the first configure-multiviewer")
        expectEqual(restored.multiviewConfigCommand()["showClock"] as? Bool, true, "restored clock")
    }

    /// The Mac command equals the Windows builder's (section multiview-overlays) key for key.
    /// Mutation: send the command without the flags.
    @MainActor
    static func testMultiviewOverlayCommand() {
        for vector in loadActionVectors(section: "multiview-overlays") {
            guard let expected = vector.effects["command"] as? [String: Any] else {
                expect(false, "\(vector.name): effects.command"); continue
            }
            let after = MultiviewOverlays(showLabels: expected["showLabels"] as? Bool ?? false,
                                          showTally: expected["showTally"] as? Bool ?? false,
                                          showMeters: expected["showMeters"] as? Bool ?? false,
                                          showClock: expected["showClock"] as? Bool ?? false)
            let command = CoreCommands.configureMultiviewer(
                layoutMode: vector.given["layoutMode"] as? String ?? "", tileCount: vector.given["tileCount"] as? Int ?? 0,
                overlays: after)
            expect(NSDictionary(dictionary: command).isEqual(to: expected),
                   "\(vector.name): the Mac configure-multiviewer equals Windows' (\(command) vs \(expected))")
        }
    }

    /// Windows ShouldShowMeter: Program tile only, meters on only; levels pass through.
    /// Mutation: draw the meter on every tile.
    static func testMultiviewProgramMeter() {
        let program = MultiviewOverlayFormatting.programMeter(left: 0.4, right: 0.7, showMeters: true, isProgramTile: true)
        expectEqual(program?.0, 0.4, "left passes through"); expectEqual(program?.1, 0.7, "right passes through")
        expect(MultiviewOverlayFormatting.programMeter(left: 0.4, right: 0.7, showMeters: false, isProgramTile: true) == nil,
               "meters off: none")
        expect(MultiviewOverlayFormatting.programMeter(left: 0.4, right: 0.7, showMeters: true, isProgramTile: false) == nil,
               "a source tile: none")
        expect(MultiviewOverlayFormatting.isProgramTile(role: "pgm", tally: ""), "the PGM cell is the program tile")
        expect(MultiviewOverlayFormatting.isProgramTile(role: "source", tally: "program"), "program tally counts")
        expect(!MultiviewOverlayFormatting.isProgramTile(role: "pvw", tally: "preview"), "PVW is not")
        expect(!MultiviewOverlayFormatting.isProgramTile(role: "source", tally: ""), "a plain source is not")
    }

    /// D-C3: a deleted starter id survives a relaunch until Restore clears it.
    /// Mutation: forget to record the removed id on delete.
    @MainActor
    static func testStarterRemovedSetPersists() {
        let model = AppModel()
        model.programSceneId = "fullscreen"
        expect(model.deleteScene("two-up") == nil, "a starter that is not on air is removable")
        expectEqual(model.currentPrefs().removedStarterSceneIds, ["two-up"], "the removed starter id is persisted")
        let relaunched = AppModel()
        relaunched.applyRestoredPrefs(model.currentPrefs())
        expectEqual(relaunched.removedStarterSceneIds, ["two-up"], "a relaunch keeps the removed id")
        expect(!relaunched.scenes.contains { $0.id == "two-up" }, "and the starter stays deleted")
        relaunched.restoreStarterScenes()
        expectEqual(relaunched.currentPrefs().removedStarterSceneIds, nil, "Restore clears the persisted set")
        let custom = AppModel()
        custom.programSceneId = "fullscreen"
        custom.newScene(layout: "single")
        let customId = custom.scenes.last?.id ?? ""
        expect(custom.deleteScene(customId) == nil, "a custom scene is removable")
        expectEqual(custom.removedStarterSceneIds, [], "a custom scene is never recorded as a starter")
    }
}
