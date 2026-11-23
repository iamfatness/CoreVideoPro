// Phase 4 leftovers (docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md,
// spec C5 rows). Own file so the parallel lanes that extend ShellTests.swift do not collide.
// Each test names the mutation it must turn red (plan-authoring rule 7).

import Foundation
import SwiftUI

extension ShellTests {

    @MainActor
    static var leftoverCases: [(String, () -> Void)] {
        [
            ("leftovers/multiview-overlay-defaults", testMultiviewOverlayDefaults),
            ("leftovers/multiview-overlay-command", testMultiviewOverlayCommand),
            ("leftovers/multiview-program-meter", testMultiviewProgramMeter),
            ("leftovers/starter-removed-set-persists", testStarterRemovedSetPersists),
            ("leftovers/multiview-chrome-pixels", testMultiviewChromePixels),
            ("leftovers/multiview-overlay-saved-to-file", testMultiviewOverlaySavedToTheFile),
            ("leftovers/multiview-overlay-tolerant-decode", testMultiviewOverlayTolerantDecode),
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
        // The core's words are "pgm" | "pvw" | "none" (MediaCore.cpp buildMultiviewTiles); Windows
        // NormalizeTally also accepts "program"/"preview", trimmed, any case.
        expect(MultiviewOverlayFormatting.isProgramTile(role: "source", tally: "pgm"), "a source on Program (core tally pgm) counts")
        expect(MultiviewOverlayFormatting.isProgramTile(role: "source", tally: " Program "), "Windows' alias, trimmed, any case")
        expect(!MultiviewOverlayFormatting.isProgramTile(role: "pvw", tally: "pgm"), "a PVW cell is PVW whatever its tally (ResolveTally)")
        expect(!MultiviewOverlayFormatting.isProgramTile(role: "source", tally: "pvw"), "a source on Preview is not")
        expect(!MultiviewOverlayFormatting.isProgramTile(role: "source", tally: "none"), "a plain source is not")
        expectEqual(MultiviewOverlayFormatting.normalizeTally("PVW"), "pvw", "pvw normalised")
        expectEqual(MultiviewOverlayFormatting.normalizeTally("preview"), "pvw", "preview alias")
        expectEqual(MultiviewOverlayFormatting.normalizeTally("live"), "none", "anything else is none")
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

    /// `view` rendered offscreen at scale 1 as RGBA bytes, rows top to bottom.
    @MainActor
    static func renderedBitmap<V: View>(_ view: V, size: CGSize) -> (bytes: [UInt8], width: Int, height: Int)? {
        let renderer = ImageRenderer(content: view.frame(width: size.width, height: size.height))
        renderer.scale = 1
        guard let image = renderer.cgImage else { return nil }
        let width = image.width, height = image.height
        var bytes = [UInt8](repeating: 0, count: width * height * 4)
        guard let context = CGContext(data: &bytes, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return (bytes, width, height)
    }

    /// RGBA of one pixel of `view` rendered offscreen at scale 1 (top-left origin).
    @MainActor
    static func renderedPixel<V: View>(_ view: V, size: CGSize, at point: (Int, Int)) -> (r: Int, g: Int, b: Int, a: Int)? {
        guard let bitmap = renderedBitmap(view, size: size) else { return nil }
        let (x, y) = point
        guard x >= 0, y >= 0, x < bitmap.width, y < bitmap.height else { return nil }
        let offset = (y * bitmap.width + x) * 4
        let b = bitmap.bytes
        return (Int(b[offset]), Int(b[offset + 1]), Int(b[offset + 2]), Int(b[offset + 3]))
    }

    /// Finding 3 of the Task 3 review: the REAL tile chrome view, rendered offscreen, draws
    /// the tally border, the label bar and the Program meter only when their overlay is on.
    /// A 200x120 source tile on Program: border probe at x=0 (the 2 px stroke straddles the
    /// edge), label bar probe at the bottom centre, meter probe near the right edge middle.
    /// Mutations: draw the border with Tally off; draw labels with Labels off; drop the meter.
    @MainActor
    static func testMultiviewChromePixels() {
        let tile = MultiviewTile(id: "s1", label: "Guest", role: "source", tally: "pgm", activeSpeaker: false,
                                 x: 0, y: 0, w: 1, h: 1)
        let size = CGSize(width: 200, height: 120)
        func alpha(_ overlays: MultiviewOverlays, _ point: (Int, Int)) -> Int {
            renderedPixel(MultiviewTileChrome(tile: tile, overlays: overlays, masterLeft: 0.8, masterRight: 0.8),
                          size: size, at: point)?.a ?? -1
        }
        /// Max alpha over the meter column (right edge, middle band): segments have gaps,
        /// so one pixel can miss a drawn segment.
        func meterAlpha(_ overlays: MultiviewOverlays) -> Int {
            guard let bitmap = renderedBitmap(
                MultiviewTileChrome(tile: tile, overlays: overlays, masterLeft: 0.8, masterRight: 0.8), size: size) else { return -1 }
            var best = 0
            for x in 172..<min(198, bitmap.width) {
                for y in 30..<min(90, bitmap.height) {
                    best = max(best, Int(bitmap.bytes[(y * bitmap.width + x) * 4 + 3]))
                }
            }
            return best
        }
        let on = MultiviewOverlays()
        let border = (0, 60), label = (100, 115)
        expect(alpha(on, border) > 0, "tally on: the border is drawn (alpha \(alpha(on, border)))")
        expectEqual(alpha(MultiviewOverlays(showTally: false), border), 0, "tally off: no border")
        expect(alpha(on, label) > 0, "labels on: the label bar is drawn")
        expectEqual(alpha(MultiviewOverlays(showLabels: false), label), 0, "labels off: no label bar")
        expect(meterAlpha(on) > 0, "meters on: a source on Program (core tally pgm) carries the meter")
        expectEqual(meterAlpha(MultiviewOverlays(showMeters: false)), 0, "meters off: no meter")
        if let probe = renderedPixel(MultiviewTileChrome(tile: tile, overlays: on), size: size, at: border) {
            expect(probe.r > probe.g && probe.r > probe.b, "a source on Program gets the red tally border (\(probe))")
        } else { expect(false, "the chrome renders offscreen") }
        expect(MultiviewerCanvas.showsCanvasClock(isPopout: true, overlays: MultiviewOverlays(showClock: true)),
               "the pop-out's canvas clock follows Clock")
        expect(!MultiviewerCanvas.showsCanvasClock(isPopout: true, overlays: on), "Clock off: no canvas clock")
        expect(!MultiviewerCanvas.showsCanvasClock(isPopout: false, overlays: MultiviewOverlays(showClock: true)),
               "the main view's clock is the header's, not the canvas's")
    }

    /// Finding 4: a flag change is SAVED (persistPrefsNow), not only held in memory until the
    /// 5 s autosave. Reads the harness prefs file back. Mutation: drop persistPrefsNow().
    @MainActor
    static func testMultiviewOverlaySavedToTheFile() {
        let model = AppModel()
        model.applyRestoredPrefs(ShellPrefs())   // the operator's file is "restored": saves are allowed
        model.setMultiviewOverlay(\.showClock, true)
        expectEqual(ShellPrefs.load().multiviewOverlays?.showClock, true, "the flag reached the prefs file at once")
        model.setMultiviewOverlay(\.showLabels, false)
        expectEqual(ShellPrefs.load().multiviewOverlays,
                    MultiviewOverlays(showLabels: false, showTally: true, showMeters: true, showClock: true),
                    "every change is saved")
        // Finding 5: a respawned core is told the flags too, not only layout and tile count.
        let multiview = model.productionSettingsCommands().first { $0["type"] as? String == "configure-multiviewer" }
        expectEqual(multiview?["showClock"] as? Bool, true, "a core respawn re-sends showClock")
        expectEqual(multiview?["showLabels"] as? Bool, false, "a core respawn re-sends showLabels")
    }

    /// Finding 7: one bad key keeps the other three saved flags.
    static func testMultiviewOverlayTolerantDecode() {
        func decode(_ json: String) -> MultiviewOverlays? {
            try? JSONDecoder().decode(MultiviewOverlays.self, from: Data(json.utf8))
        }
        expectEqual(decode(#"{"showClock":true}"#), MultiviewOverlays(showClock: true), "absent keys keep Windows defaults")
        expectEqual(decode(#"{"showLabels":false,"showTally":"yes","showClock":true}"#),
                    MultiviewOverlays(showLabels: false, showTally: true, showMeters: true, showClock: true),
                    "an unreadable key falls back alone")
    }
}
