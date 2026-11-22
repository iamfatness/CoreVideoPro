// Graphics G3 Task 9 — F-keys. Port of GraphicsHotkeyRouterTests as a table,
// plus model transcripts through the real AppModel key path.

import Foundation

extension ShellTests {
    static var graphicsHotkeyCases: [(String, () -> Void)] {
        [
            ("graphics-keys/router-table", testGraphicsHotkeyRouterTable),
            ("graphics-keys/f2-shows-and-shift-f12-clears", testGraphicsHotkeysDriveTheModel),
            ("graphics-keys/guest-row-plays-program-subject", testGraphicsGuestHotkeyPlaysProgramSubject),
            ("graphics-keys/monitor-on-graphics-tab", testGraphicsMonitorOnGraphicsTab),
        ]
    }

    static func testGraphicsHotkeyRouterTable() {
        let rows = [GraphicsRow(json: ["id": "guest", "kind": "guest", "name": "G", "hotkey": "F1"]),
                    GraphicsRow(json: ["id": "title", "kind": "title", "name": "T", "hotkey": "F2"]),
                    GraphicsRow(json: ["id": "nokey", "kind": "logo", "name": "L"])]
        let layouts = ["Shift+F3": "open"]
        typealias Mods = TransportKeyEvent.Modifiers
        let modifierSets: [(String, Mods)] = [("none", []), ("shift", [.shift]), ("command", [.command]), ("option", [.option]),
                                               ("control", [.control]), ("shift+command", [.shift, .command])]
        var checked = 0
        for number in 1...12 {
            for (name, mods) in modifierSets {
                for focus in [false, true] {
                    let got = GraphicsHotkeys.route(function: number, modifiers: mods, textFocus: focus, rows: rows, layoutHotkeys: layouts)
                    let want: GraphicsHotkeyRoute?
                    if focus { want = nil }
                    else if mods == [.shift] { want = number == 12 ? .playOutAll : number == 3 ? .layoutCue(layoutId: "open") : nil }
                    else if mods.isEmpty { want = number == 1 ? .playIn(rowId: "guest", guest: true) : number == 2 ? .playIn(rowId: "title", guest: false) : nil }
                    else { want = nil }
                    expectEqual(got, want, "F\(number) \(name) focus=\(focus)")
                    checked += 1
                }
            }
        }
        expectEqual(checked, 144, "every key x modifier x focus")
        expect(GraphicsHotkeys.route(function: 12, modifiers: [.shift], textFocus: false, rows: [], layoutHotkeys: ["Shift+F12": "x"]) == .playOutAll,
               "Shift+F12 is always Play Out all, never a layout")
        expect(GraphicsHotkeys.route(function: 13, modifiers: [], textFocus: false, rows: rows) == nil, "F13 routes nowhere")
        expectEqual(GraphicsHotkeys.keyCodes.count, 12, "twelve key codes")
        expectEqual(Set(GraphicsHotkeys.keyCodes.values), Set(1...12), "mapped to F1-F12")
    }

    static func fKey(_ n: Int, _ mods: TransportKeyEvent.Modifiers = [], focus: Bool = false, repeated: Bool = false) -> TransportKeyEvent {
        TransportKeyEvent(kind: .function(n), modifiers: mods, isRepeat: repeated, isStudioTab: true, isTextEntryFocus: focus)
    }

    /// An AppModel whose graphics talk to a scripted core with the real catalog.
    @MainActor
    static func hotkeyApp(_ rows: [GraphicsRow], instances: [JSONObject] = []) -> (AppModel, ScriptedGraphicsCore) {
        let app = AppModel()
        let core = ScriptedGraphicsCore(instances: instances)
        core.node["templates"] = realCatalogTemplates
        core.react = ScriptedGraphicsCore.instancesReaction()
        app.quitRequestForTesting = { body, _ in try await core.send(body) }
        app.status = .connected(renderer: "test")
        app.graphics.saveDelayNs = 0
        spin(until: { app.graphics.documentLoaded }, limit: 3)   // the async load must not land on top of the test rows
        app.graphics.observe(snapshot: ["graphics": core.node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]])
        app.graphics.document.playlist = rows
        app.graphics.playout.sleep = { [weak core, weak app] _ in
            core?.advancePreparation()
            if let core { app?.graphics.observe(snapshot: ["graphics": core.node]) }
        }
        return (app, core)
    }

    @MainActor
    static func testGraphicsHotkeysDriveTheModel() {
        let (app, core) = hotkeyApp(interviewRows(), instances: [ScriptedGraphicsCore.instance("other", program: 3, visibility: "on-air")])
        app.selectedTab = .overlays
        expect(!app.handleGraphicsKey(fKey(2, focus: true)), "F2 while typing does nothing")
        expect(core.commands.isEmpty, "nothing sent while typing")
        expect(app.handleGraphicsKey(fKey(2)), "F2 is consumed")
        spin(until: { core.transcript.last == "graphics-instance-trigger:show:playlist-title" }, limit: 3)
        expectEqual(core.transcript, ["graphics-instance-create:playlist-title", "graphics-instance-cue:playlist-title",
                                      "graphics-instance-trigger:show:playlist-title"], "F2 Shows the F2 row")
        expect(app.handleGraphicsKey(fKey(2, repeated: true)), "a held F2 is swallowed")
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        expectEqual(core.commands.count, 3, "and never re-Shows")
        expect(!app.handleGraphicsKey(fKey(9)), "an unassigned key passes through")
        expect(app.handleGraphicsKey(fKey(12, [.shift])), "Shift+F12 is consumed")
        spin(until: { core.transcript.filter { $0.hasPrefix("graphics-instance-trigger:hide") }.count == 2 }, limit: 3)
        expectEqual(Set(core.transcript.filter { $0.hasPrefix("graphics-instance-trigger:hide") }),
                    ["graphics-instance-trigger:hide:other", "graphics-instance-trigger:hide:playlist-title"],
                    "Shift+F12 hides everything on Program")
        app.selectedTab = .audio
        expect(!app.handleGraphicsKey(fKey(2)), "other tabs: graphics keys do nothing")
    }

    @MainActor
    static func testGraphicsGuestHotkeyPlaysProgramSubject() {
        let (app, _) = hotkeyApp(interviewRows())
        app.graphics.rosterSlots = {
            [GraphicsRosterSlot(slotNumber: 1, sourceId: "zoom:100", derivedName: "Ada"),
             GraphicsRosterSlot(slotNumber: 2, sourceId: "zoom:200", derivedName: "Bo")]
        }
        expectEqual(app.graphics.resolveEntry(rowId: "guest", subjectSourceId: "zoom:200")?.label, "Bo", "the Program subject's line")
        expectEqual(app.graphics.resolveEntry(rowId: "guest", subjectSourceId: nil)?.label, "Ada", "no subject: the first line (Windows)")
        expectEqual(app.graphics.resolveEntry(rowId: "title", subjectSourceId: "zoom:200")?.sourceId, "", "a plain row is its own entry")
        app.graphics.rosterSlots = { [] }
        app.graphics.perform(.playIn(rowId: "guest", guest: true), subjectSourceId: nil)
        expectEqual(app.graphics.status, "Playlist row 'guest' has nothing to play. Add a source in Sources or choose another row.",
                    "an empty Guest row says why")
    }

    @MainActor
    static func testGraphicsMonitorOnGraphicsTab() {
        let model = AppModel()
        model.selectedTab = .overlays
        model.syncTransportHotkeys()
        expect(model.transportHotkeys.isInstalled, "the key monitor runs on the Graphics tab (F-keys)")
        expectEqual(TransportKeys.decide(TransportKeyEvent(kind: .space, modifiers: [], isRepeat: false, isStudioTab: false,
                                                           isTextEntryFocus: false)), .ignore,
                    "Space never Takes from the Graphics tab")
        model.selectedTab = .audio
        model.syncTransportHotkeys()
        expect(!model.transportHotkeys.isInstalled, "removed elsewhere")
    }
}
