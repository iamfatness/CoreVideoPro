// Graphics G3 Tasks 10-11 — a cue rides the Take; lower-third authority routing.

import Foundation

extension ShellTests {
    static var graphicsTakeCases: [(String, () -> Void)] {
        [("graphics-take/cue-rides-take-with-no-graphics-command", testGraphicsCueRidesTake)]
    }

    static var graphicsAuthorityCases: [(String, () -> Void)] {
        [
            ("graphics-authority/router-table", testLowerThirdRouterTable),
            ("graphics-authority/intent-visible", testLowerThirdIntentVisible),
            ("graphics-authority/playlist-show-adopts-after-accept", testLowerThirdPlaylistShowAdoptsAfterAccept),
            ("graphics-authority/refused-show-never-adopts", testLowerThirdRefusedShowNeverAdopts),
            ("graphics-authority/hide-with-legacy-authority-stays-legacy", testLowerThirdLegacyHideStaysLegacy),
            ("graphics-authority/managed-hide-never-legacy", testLowerThirdManagedHide),
            ("graphics-authority/control-and-buttons-share-the-entry", testLowerThirdControlUsesTheEntry),
        ]
    }

    /// One fake for both paths: graphics-* bodies go to the scripted graphics core,
    /// everything else (the Take batch) is recorded.
    @MainActor
    static func takeAndGraphicsApp(_ rows: [GraphicsRow]) -> (AppModel, ScriptedGraphicsCore, FakeTakeCore) {
        let model = sceneEditModel()
        let graphicsCore = ScriptedGraphicsCore()
        graphicsCore.node["templates"] = realCatalogTemplates
        graphicsCore.react = ScriptedGraphicsCore.instancesReaction()
        let takeCore = FakeTakeCore()
        model.quitRequestForTesting = { body, _ in
            let first = (body["commands"] as? [JSONObject])?.first?["type"] as? String ?? ""
            return first.hasPrefix("graphics-") ? try await graphicsCore.send(body) : try await takeCore.handle(body)
        }
        model.status = .connected(renderer: "test")
        spin(until: { model.graphics.documentLoaded }, limit: 3)
        model.graphics.saveDelayNs = 0
        model.graphics.observe(snapshot: ["graphics": graphicsCore.node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]])
        model.graphics.document.playlist = rows
        model.graphics.playout.sleep = { [weak graphicsCore, weak model] _ in
            if let graphicsCore { model?.graphics.observe(snapshot: ["graphics": graphicsCore.node]) }
        }
        return (model, graphicsCore, takeCore)
    }

    /// Spec 5.4: cued graphics ride an ordinary Take with no shell work. The cue
    /// is rideTake:true; the Take batch leads with begin-take-transition (where the
    /// core promotes ride-Take cues) and carries no graphics command. The pixels
    /// are Task 12's live check.
    @MainActor
    static func testGraphicsCueRidesTake() {
        let rows = interviewRows()
        let (model, graphicsCore, takeCore) = takeAndGraphicsApp(rows)
        let cued = awaitMain(5) { await model.graphics.playout.cue(GraphicsPlayout.entry(for: rows.first { $0.id == "title" }!)) }
        expectEqual(cued?.accepted, true, "the title is cued")
        let cue = graphicsCore.commands.first { $0["type"] as? String == "graphics-instance-cue" }?["graphics"] as? JSONObject
        expectEqual(cue?["rideTake"] as? Bool, true, "a playlist cue is rideTake:true")
        let box = runTake(model)
        expectEqual(box.result, .success, "Take succeeds")
        expectEqual(takeCore.bodies.count, 1, "one Take batch")
        let types = takeCore.bodies.isEmpty ? [] : takeCore.types()
        expectEqual(types.first, "begin-take-transition", "the batch leads with begin-take-transition")
        expect(!types.contains { $0.hasPrefix("graphics-") }, "and carries no graphics-* command")
        expect(!graphicsCore.transcript.contains { $0.hasPrefix("graphics-instance-trigger") }, "the shell sends no trigger for the Take")
    }

    static func testLowerThirdRouterTable() {
        for authority in [false, true] {
            for guest in [false, true] {
                for visible in [false, true] {
                    let want: LowerThirdTarget = guest && (visible || authority) ? .playlist : authority ? .managed : .legacy
                    expectEqual(LowerThirdAuthority.target(visible: visible, hasGuestRow: guest, authorityInstances: authority), want,
                                "authority=\(authority ? "instances" : "legacy") guest=\(guest) \(visible ? "show" : "hide")")
                }
            }
        }
        expectEqual(LowerThirdAuthority.target(visible: false, hasGuestRow: true, authorityInstances: false), .legacy,
                    "#1166: Hide while authority is legacy goes to the legacy key, never the playlist")
    }

    static func testLowerThirdIntentVisible() {
        expect(LowerThirdAuthority.intentVisible(authorityInstances: false, hasGuestRow: true, playlistOnAir: true, managedOnAir: true, legacyOnAir: false) == false,
               "legacy authority: only the legacy key counts")
        expect(LowerThirdAuthority.intentVisible(authorityInstances: true, hasGuestRow: true, playlistOnAir: true, managedOnAir: false, legacyOnAir: false),
               "instances + Guest row: the playlist chip")
        expect(LowerThirdAuthority.intentVisible(authorityInstances: true, hasGuestRow: false, playlistOnAir: false, managedOnAir: true, legacyOnAir: false),
               "instances without a Guest row: managed instances")
    }

    @MainActor
    static func authorityApp(rows: [GraphicsRow], authority: String = "legacy", instances: [JSONObject] = [],
                             refuse: String? = nil) -> (AppModel, ScriptedGraphicsCore) {
        // Auto-out off: this harness's sleep is instant, and the 8 s Guest auto-out has its own test.
        let rows = rows.map { row -> GraphicsRow in var row = row; row.playOutAfterMs = 0; return row }
        let (app, core) = hotkeyApp(rows, instances: instances)
        core.node["familyAuthorities"] = ["lower-third": authority]
        let base = ScriptedGraphicsCore.instancesReaction()
        core.react = { command, node in
            if command["type"] as? String == "graphics-authority-set" { node["familyAuthorities"] = ["lower-third": "instances"]; return (true, "") }
            if let refuse, command["type"] as? String == "graphics-instance-trigger" { return (false, refuse) }
            return base(command, &node)
        }
        app.graphics.observe(snapshot: ["graphics": core.node])
        app.graphics.rosterSlots = { [GraphicsRosterSlot(slotNumber: 1, sourceId: "zoom:100", derivedName: "Ada")] }
        return (app, core)
    }

    /// The Program subject comes from the rendered Program frame; tests set it.
    @MainActor
    static func setProgramSubject(_ app: AppModel, zoom pid: String) {
        app.roster = [RosterParticipant(id: pid, name: "Ada", hasVideo: true, muted: false, talking: false, assigned: true)]
        app.applySnapshotForTesting(["programFrame": ["frameNumber": 5, "sceneId": app.programSceneId,
                                                      "videoSources": [["sourceId": "zoom:" + pid, "kind": "participant-video",
                                                                        "participantId": pid, "layerId": "l1"]]]])
    }

    @MainActor
    static func testLowerThirdPlaylistShowAdoptsAfterAccept() {
        let (app, core) = authorityApp(rows: interviewRows())
        setProgramSubject(app, zoom: "100")
        expectEqual(app.programLowerThirdSubject()?.source.sourceId, "zoom:100", "subject resolved from Program")
        let result = awaitMain(5) { await app.applyLowerThirdIntent(visible: true) }
        expectEqual(result?.accepted, true, "Show through the playlist")
        let id = GraphicsPlayoutIds.instanceId(rowId: "guest", sourceId: "zoom:100")
        expectEqual(core.transcript, ["graphics-instance-create:\(id)", "graphics-instance-cue:\(id)",
                                      "graphics-instance-trigger:show:\(id)", "graphics-authority-set"],
                    "authority adopted only AFTER the accepted Show")
        expectEqual(result?.reason, "Shown; lower thirds now follow the Graphics playlist.", "Windows' reason")
        expect(!app.graphics.lowerThirdAutomationSuppressed, "an accepted Show clears suppression")
        let hide = awaitMain(5) { await app.applyLowerThirdIntent(visible: false) }
        expectEqual(hide?.reason, "Lower third hidden.", "Hide (authority now instances) goes to the playlist")
        expectEqual(core.transcript.last, "graphics-instance-trigger:hide:\(id)", "hiding the Guest instance")
        expect(app.graphics.lowerThirdAutomationSuppressed, "an accepted Hide sets suppression")
    }

    @MainActor
    static func testLowerThirdRefusedShowNeverAdopts() {
        let (app, core) = authorityApp(rows: interviewRows(), refuse: "cue-not-prepared")
        setProgramSubject(app, zoom: "100")
        let result = awaitMain(5) { await app.applyLowerThirdIntent(visible: true) }
        expectEqual(result?.accepted, false, "the Show is refused")
        expect(!core.transcript.contains("graphics-authority-set"), "and authority is NOT adopted")
        expect(!app.graphics.lowerThirdAutomationSuppressed, "suppression untouched by a refusal")
    }

    @MainActor
    static func testLowerThirdLegacyHideStaysLegacy() {
        let (app, core) = authorityApp(rows: interviewRows())
        var overlays: [JSONObject] = []
        app.overlayCommandObserver = { overlays.append($0) }
        app.lowerThirdPhase = "on-air"
        app.lowerThirdKeyedSourceId = "zoom:100"
        let result = awaitMain { await app.applyLowerThirdIntent(visible: false) }
        expectEqual(result?.accepted, true, "accepted")
        expect(core.commands.isEmpty, "no graphics command: a Guest row does not own a legacy key (#1166)")
        expectEqual(overlays.first?["keyPhase"] as? String, "building-out", "the legacy key builds out")
        expect(app.graphics.lowerThirdAutomationSuppressed, "suppressed")
        app.transportHotkeys.remove()
    }

    @MainActor
    static func testLowerThirdManagedHide() {
        let managed = ScriptedGraphicsCore.instance("lt-1", revision: 4, program: 4, visibility: "on-air", template: "compact-solid")
        var global = managed; global["scope"] = "global"
        let (app, core) = authorityApp(rows: [], authority: "instances", instances: [global])
        var overlays: [JSONObject] = []
        app.overlayCommandObserver = { overlays.append($0) }
        let result = awaitMain { await app.applyLowerThirdIntent(visible: false) }
        expectEqual(result?.accepted, true, "managed hide accepted")
        expectEqual(core.transcript, ["graphics-instance-trigger:hide:lt-1"], "hides the managed lower third")
        expectEqual((core.commands.first?["graphics"] as? JSONObject)?["clientId"] as? String, "legacy-lower-third", "as Windows' client")
        expect(overlays.isEmpty, "never the legacy key while instances own Program")
        let show = awaitMain { await app.applyLowerThirdIntent(visible: true) }
        expectEqual(show?.reason, "Prepare a lower-third Cue before using the existing Show control.", "no cue: refused with Windows' text")
    }

    @MainActor
    static func testLowerThirdControlUsesTheEntry() {
        let (app, core) = authorityApp(rows: interviewRows())
        setProgramSubject(app, zoom: "100")
        let surface = AppModelControlSurface(model: app, box: app.controlSnapshotBox)
        let result = awaitMain(5) { await surface.invoke("graphics.lowerThird.set", [true]) }
        expect(result?.ok == true, "the control action succeeds")
        expect(core.transcript.contains { $0.hasPrefix("graphics-instance-trigger:show") }, "through the playlist, like the buttons")
        let toggle = awaitMain(5) { await surface.invoke("graphics.lowerThird.toggle", []) }
        expect(toggle?.ok == true, "toggle succeeds")
        expectEqual(core.transcript.last.map { $0.hasPrefix("graphics-instance-trigger:hide") }, true, "toggle follows Program: it hides")
    }
}
