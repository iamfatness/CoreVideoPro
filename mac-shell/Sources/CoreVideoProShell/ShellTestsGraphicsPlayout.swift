// Graphics G2 Task 7 — the playout service.
// Vectors: contracts/graphics-shell/playout.vectors.json (GraphicsShellVectorTests.PlayoutVectors).
// Transcripts: a scripted core that models create / update / cue / trigger.

import Foundation

extension ScriptedGraphicsCore {
    /// A core that behaves like GraphicsInstances for the playout path: create adds
    /// the instance at a fresh revision, update(target cue) bumps the revision and
    /// copies the fields, cue sets cueRevision (cuePending while `pendingPolls` > 0),
    /// trigger show-from-cue puts the cue copy on Program, hide takes it off,
    /// install adds the manifest to the catalog.
    static func instancesReaction(pendingPolls: Int = 0) -> (JSONObject, inout JSONObject) -> (accepted: Bool, reason: String) {
        { command, node in
            let g = command["graphics"] as? JSONObject ?? [:]
            var instances = node["instances"] as? [JSONObject] ?? []
            let id = g["instanceId"] as? String ?? ""
            let index = instances.firstIndex { $0["instanceId"] as? String == id }
            let nextRevision = Int64((node["nextRevision"] as? NSNumber)?.int64Value ?? 1)
            switch command["type"] as? String {
            case "graphics-instance-create":
                var created = g
                for key in ["schemaVersion", "requestId", "clientId", "coreEpoch"] { created.removeValue(forKey: key) }
                created["revision"] = nextRevision; created["programRevision"] = 0; created["cueRevision"] = 0
                created["visibility"] = "hidden"
                instances.append(created)
                node["nextRevision"] = nextRevision + 1
            case "graphics-instance-update":
                guard let index else { return (false, "instance-not-found") }
                if let fields = g["fields"] { instances[index]["fields"] = fields }
                if let animations = g["animations"] { instances[index]["animations"] = animations }
                if let version = g["templateVersion"] { instances[index]["templateVersion"] = version }
                instances[index]["revision"] = nextRevision
                node["nextRevision"] = nextRevision + 1
            case "graphics-instance-cue":
                guard let index else { return (false, "instance-not-found") }
                instances[index]["cueRevision"] = instances[index]["revision"]
                instances[index]["cuePending"] = pendingPolls > 0
                node["pendingPolls"] = pendingPolls
            case "graphics-instance-trigger":
                guard let index else { return (false, "instance-not-found") }
                switch g["action"] as? String {
                case "show":
                    if (instances[index]["cuePending"] as? Bool) == true { return (false, "cue-not-prepared") }
                    instances[index]["programRevision"] = instances[index]["cueRevision"]; instances[index]["visibility"] = "on-air"
                    instances[index]["cueRevision"] = 0
                case "hide": instances[index]["programRevision"] = 0; instances[index]["visibility"] = "hidden"
                case "uncue": instances[index]["cueRevision"] = 0
                default: break
                }
            case "graphics-clock-control":
                guard let index else { return (false, "instance-not-found") }
                if g["action"] as? String == "configure" {
                    instances[index]["clockRequestedConfiguration"] = g["configuration"]
                    instances[index]["clockConfigurationRevision"] = ((instances[index]["clockConfigurationRevision"] as? NSNumber)?.intValue ?? 1) + 1
                }
            case "graphics-template-install":
                if let manifest = (g["manifest"] as? String).flatMap(GraphicsJSON.parseObject) {
                    var templates = node["templates"] as? [JSONObject] ?? []
                    var listed = manifest
                    listed["manifestHash"] = node["installHash"] as? String ?? ""
                    templates.append(listed)
                    node["pendingTemplates"] = templates
                }
            case "graphics-template-list":
                if let pending = node["pendingTemplates"] { node["templates"] = pending; node.removeValue(forKey: "pendingTemplates") }
            default: break
            }
            node["instances"] = instances
            return (true, "")
        }
    }

    /// One poll of a cue still preparing: counts down and clears cuePending at zero.
    func advancePreparation() {
        var polls = (node["pendingPolls"] as? Int) ?? 0
        guard polls > 0 else { return }
        polls -= 1
        node["pendingPolls"] = polls
        if polls == 0, var instances = node["instances"] as? [JSONObject] {
            for i in instances.indices { instances[i]["cuePending"] = false }
            node["instances"] = instances
        }
    }
}

extension ShellTests {
    static var graphicsPlayoutCases: [(String, () -> Void)] {
        [
            ("graphics-playout/vectors", testGraphicsPlayoutVectors),
            ("graphics-playout/show-new-row", testGraphicsShowNewRowTranscript),
            ("graphics-playout/show-existing-unchanged", testGraphicsShowExistingUnchanged),
            ("graphics-playout/show-existing-changed-updates-cue-copy", testGraphicsShowExistingChanged),
            ("graphics-playout/cue-waits-for-revision", testGraphicsCueWaitsForTheCueRevision),
            ("graphics-playout/cue-budget-refuses", testGraphicsCueBudgetRefuses),
            ("graphics-playout/auto-out", testGraphicsAutoOut),
            ("graphics-playout/epoch-change-mid-show", testGraphicsEpochChangeMidShow),
            ("graphics-playout/pinned-template-reinstalled", testGraphicsPinnedTemplateReinstalled),
            ("graphics-playout/no-template-refused", testGraphicsRowWithoutTemplateRefused),
        ]
    }

    static func playoutVectors() -> [JSONObject] { loadGraphicsVectors("playout") }

    static func interviewRows() -> [GraphicsRow] {
        let row = playoutVectors().first { $0["id"] as? String == "default-rows-interview" }
        return ((row?["expected"] as? JSONObject)?["rows"] as? [JSONObject] ?? []).map(GraphicsRow.init(json:))
    }

    static var realCatalogTemplates: [JSONObject] { loadGraphicsNode("nodes/program-and-cue.json")["templates"] as? [JSONObject] ?? [] }

    /// A model over a scripted core carrying the REAL Metal catalog, with `rows` as the playlist.
    @MainActor
    static func playoutModel(_ rows: [GraphicsRow], pendingPolls: Int = 0, instances: [JSONObject] = [])
        -> (GraphicsModel, ScriptedGraphicsCore) {
        let core = ScriptedGraphicsCore(instances: instances)
        core.node["templates"] = realCatalogTemplates
        core.node["sources"] = [] as [Any]
        core.react = ScriptedGraphicsCore.instancesReaction(pendingPolls: pendingPolls)
        let model = graphicsModel(core)
        model.document.playlist = rows   // playout reads the document, as in the app
        model.playout.sleep = { [weak core, weak model] _ in
            core?.advancePreparation()
            if let core { model?.observe(snapshot: ["graphics": core.node]) }
        }
        return (model, core)
    }

    // ── vectors ────────────────────────────────────────────────────────────

    static func testGraphicsPlayoutVectors() {
        var checked = 0
        let catalog = GraphicsState(node: ["schemaVersion": 1, "coreEpoch": "e", "templates": realCatalogTemplates])
        for row in playoutVectors() {
            let id = row["id"] as? String ?? "?"
            let input = row["input"] as? JSONObject ?? [:], expected = row["expected"] as? JSONObject ?? [:]
            switch row["operation"] as? String {
            case "instanceIdFor":
                expectEqual(GraphicsPlayoutIds.instanceId(rowId: input["rowId"] as? String ?? "", sourceId: input["sourceId"] as? String ?? ""),
                            expected["instanceId"] as? String ?? "", "playout \(id)")
            case "guestEntries":
                let slots = (input["slots"] as? [JSONObject] ?? []).map {
                    GraphicsRosterSlot(slotNumber: $0["slotNumber"] as? Int ?? 0, sourceId: $0["sourceId"] as? String, derivedName: $0["derivedName"] as? String ?? "")
                }
                let row = GraphicsRow(json: ["id": input["rowId"] as? String ?? "", "kind": "guest", "name": "Guest"])
                let got = GraphicsPlayout.guestEntries(row, slots) { _, name in (name, "") }
                let want = expected["entries"] as? [JSONObject] ?? []
                expectEqual(got.map(\.sourceId), want.map { $0["sourceId"] as? String ?? "" }, "playout \(id): sources in slot order, de-duplicated")
                expectEqual(got.map(\.label), want.map { $0["name"] as? String ?? "" }, "playout \(id): names")
                expectEqual(got.map(\.slotNumber), want.map { $0["slotNumber"] as? Int }, "playout \(id): slots")
                expectEqual(got.map(\.instanceId), want.map { $0["instanceId"] as? String ?? "" }, "playout \(id): instance ids")
            case "cueFieldsAndStyle":
                let rowJSON = GraphicsRow(json: input["row"] as? JSONObject ?? [:])
                let e = input["entry"] as? JSONObject ?? [:]
                let entry = GraphicsPlaylistEntry(rowId: rowJSON.id, sourceId: e["sourceId"] as? String ?? "",
                                                  name: e["name"] as? String ?? "", title: e["title"] as? String ?? "", slotNumber: 1)
                guard let template = catalog.template(input["templateId"] as? String ?? "", input["templateVersion"] as? Int ?? 0)
                else { expect(false, "playout \(id): template missing from the real catalog"); continue }
                let fields = GraphicsPlayout.cueFields(rowJSON, entry, declarative: template.runtime == "declarative", descriptors: template.fields)
                expect(jsonEqual(fields, expected["fields"]), "playout \(id): cue fields \(fields)")
                let style = GraphicsStyleWire.wire(rowJSON.style, declarative: !GraphicsPlayout.curated(template.templateId))
                expect(jsonEqual(style, expected["style"]), "playout \(id): style wire \(style)")
            case "defaultRows":
                expect(interviewRows().count == 5 && interviewRows().allSatisfy(\.isValid), "playout \(id): five valid default rows")
            default: expect(false, "playout \(id): unknown operation")
            }
            checked += 1
        }
        expect(checked >= 12, "every playout vector replayed (\(checked))")
    }

    // ── transcripts ────────────────────────────────────────────────────────

    @MainActor
    static func showAndSettle(_ model: GraphicsModel, _ entry: GraphicsPlaylistEntry) -> GraphicsOperationResult? {
        awaitMain(5) { await model.playout.show(entry) }
    }

    @MainActor
    static func testGraphicsShowNewRowTranscript() {
        let rows = interviewRows()
        let title = rows.first { $0.id == "title" }!
        let (model, core) = playoutModel(rows)
        let result = showAndSettle(model, GraphicsPlayout.entry(for: title))
        expectEqual(result?.accepted, true, "Show of a new row is accepted")
        expectEqual(result?.reason, "Shown from Preview.", "Windows' reason")
        expectEqual(core.transcript, ["graphics-instance-create:playlist-title", "graphics-instance-cue:playlist-title",
                                      "graphics-instance-trigger:show:playlist-title"], "create -> cue -> trigger show")
        guard core.commands.count >= 3 else { expect(false, "Show refused: \(result?.reason ?? "nil")"); return }
        let create = core.commands[0]["graphics"] as? JSONObject ?? [:]
        expectEqual(create["scope"] as? String, "global", "global scope")
        expectEqual(create["templateId"] as? String, "centered-title", "the row's pin")
        expect(jsonEqual(create["animations"], GraphicsPlayout.defaultMotion), "default fade motion")
        expect((create["style"] as? JSONObject)?["preset"] == nil, "a declarative template gets only the consumed style keys")
        let cue = core.commands[1]["graphics"] as? JSONObject ?? [:]
        expectEqual(cue["rideTake"] as? Bool, true, "a playlist cue rides the next Take")
        let trigger = core.commands[2]["graphics"] as? JSONObject ?? [:]
        expectEqual(trigger["target"] as? String, "cue", "show FROM the cue")
        expectEqual((trigger["expectedRevision"] as? NSNumber)?.int64Value, (cue["expectedRevision"] as? NSNumber)?.int64Value,
                    "at the cue's revision")
        expectEqual(model.strip.onProgram.map(\.instanceId), ["playlist-title"], "on Program per the core")
    }

    @MainActor
    static func testGraphicsShowExistingUnchanged() {
        let rows = interviewRows()
        let title = rows.first { $0.id == "title" }!
        let (model, core) = playoutModel(rows)
        _ = showAndSettle(model, GraphicsPlayout.entry(for: title))
        _ = awaitMain { await model.playout.hide("playlist-title") }
        let before = core.commands.count
        _ = showAndSettle(model, GraphicsPlayout.entry(for: title))
        expectEqual(Array(core.transcript.dropFirst(before)), ["graphics-instance-cue:playlist-title", "graphics-instance-trigger:show:playlist-title"],
                    "an existing instance with the same fields is cued as is: no update")
    }

    @MainActor
    static func testGraphicsShowExistingChanged() {
        let rows = interviewRows()
        let guestRow = rows.first { $0.id == "guest" }!
        let (model, core) = playoutModel(rows)
        let ada = GraphicsPlaylistEntry(rowId: "guest", sourceId: "zoom:100", name: "Ada", title: "", slotNumber: 1)
        _ = showAndSettle(model, ada)
        let before = core.commands.count
        var renamed = ada
        renamed.bindings["name"] = "Ada Lovelace"
        _ = showAndSettle(model, renamed)
        let id = GraphicsPlayoutIds.instanceId(rowId: "guest", sourceId: "zoom:100")
        // (The Guest row's 8 s auto-out fires on this harness's instant sleep; its own test uses manual timers.)
        expectEqual(Array(core.transcript.dropFirst(before).prefix(3)),
                    ["graphics-instance-update:\(id)", "graphics-instance-cue:\(id)", "graphics-instance-trigger:show:\(id)"],
                    "changed text updates the CUE copy first")
        guard core.commands.count > before else { expect(false, "second Show sent nothing"); return }
        let update = core.commands[before]["graphics"] as? JSONObject ?? [:]
        expectEqual(update["target"] as? String, "cue", "never the Program copy")
        expectEqual((update["fields"] as? JSONObject)?["name"] as? String, "Ada Lovelace", "with the new name")
        expect(update["animations"] == nil, "motion unchanged: not resent")
        expect(guestRow.followSources, "the default Guest row follows Sources")
    }

    @MainActor
    static func testGraphicsCueWaitsForTheCueRevision() {
        let rows = interviewRows()
        let (model, core) = playoutModel(rows, pendingPolls: 3)
        var sleeps = 0
        let inner = model.playout.sleep
        model.playout.sleep = { sleeps += 1; await inner($0) }
        let result = showAndSettle(model, GraphicsPlayout.entry(for: rows.first { $0.id == "title" }!))
        expectEqual(result?.accepted, true, "the show lands once the cue is prepared (the core refuses show while cuePending)")
        expect(sleeps >= 3, "the cue wait polled the core on the injected sleep (\(sleeps))")
        expectEqual(core.transcript.last, "graphics-instance-trigger:show:playlist-title", "trigger sent only after the cue landed")
    }

    @MainActor
    static func testGraphicsCueBudgetRefuses() {
        let rows = interviewRows()
        let (model, core) = playoutModel(rows, pendingPolls: 1_000_000)
        var clock = Date(timeIntervalSince1970: 1000)
        model.playout.now = { clock }
        let inner = model.playout.sleep
        model.playout.sleep = { clock.addTimeInterval(Double($0) / 1e9); await inner($0) }
        let result = showAndSettle(model, GraphicsPlayout.entry(for: rows.first { $0.id == "title" }!))
        expectEqual(result?.reason, "The graphic is still preparing. Retry when ready.", "5 s budget then a refusal")
        expect(!core.transcript.contains { $0.hasPrefix("graphics-instance-trigger") }, "and no trigger")
    }

    @MainActor
    static func testGraphicsAutoOut() {
        let rows = interviewRows()
        let guest = rows.first { $0.id == "guest" }!
        expectEqual(guest.playOutAfterMs, 8000, "the default Guest row hides after 8 s")
        let (model, core) = playoutModel(rows)
        let ada = GraphicsPlaylistEntry(rowId: "guest", sourceId: "zoom:100", name: "Ada", title: "", slotNumber: 1)
        let id = ada.instanceId
        _ = showAndSettle(model, ada)
        // From here on, sleeps park on a manual timer (the auto-out's 8 s).
        let timers = ManualTimers()
        model.playout.sleep = { await timers.sleep($0) }
        _ = showAndSettle(model, ada)   // re-arms with the manual timer
        spin(until: { timers.pending > 0 }, limit: 2)
        expect(model.playout.autoOut[id] != nil, "auto-out armed after Show")
        timers.fireAll()
        spin(until: { core.transcript.last == "graphics-instance-trigger:hide:\(id)" }, limit: 2)
        expectEqual(core.transcript.last, "graphics-instance-trigger:hide:\(id)", "fires a Hide")

        // Hide before it fires cancels it.
        _ = showAndSettle(model, ada)
        spin(until: { timers.pending > 0 }, limit: 2)
        _ = awaitMain { await model.playout.hide(id) }
        let count = core.commands.count
        timers.fireAll()
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        expectEqual(core.commands.count, count, "a manual Hide cancels the auto-out")

        // An epoch change cancels it.
        _ = showAndSettle(model, ada)
        spin(until: { timers.pending > 0 }, limit: 2)
        var next = core.node
        next["coreEpoch"] = "epoch-2"
        model.observe(snapshot: ["graphics": next])
        expect(model.playout.autoOut.isEmpty, "an epoch change drops every auto-out")
        let after = core.commands.count
        timers.fireAll()
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        expectEqual(core.commands.count, after, "nothing fires into the new core")
    }

    @MainActor
    static func testGraphicsEpochChangeMidShow() {
        let rows = interviewRows()
        let (model, core) = playoutModel(rows, pendingPolls: 5)
        let inner = model.playout.sleep
        var polls = 0
        model.playout.sleep = { ns in
            polls += 1
            if polls == 2 { core.node["coreEpoch"] = "epoch-9"; core.node["instances"] = [] as [Any] }
            await inner(ns)
        }
        let result = showAndSettle(model, GraphicsPlayout.entry(for: rows.first { $0.id == "title" }!))
        expectEqual(result?.reason, GraphicsPlayout.coreChangedMessage, "a core restart mid-Show refuses")
        expect(!core.transcript.contains { $0.hasPrefix("graphics-instance-trigger") }, "and never replays into the new core")
    }

    @MainActor
    static func testGraphicsPinnedTemplateReinstalled() {
        var rows = interviewRows()
        rows[1].templateId = "pinned-title"
        rows[1].templateVersion = 1
        let (model, core) = playoutModel(rows)
        let hash = String(repeating: "c", count: 64)
        core.node["installHash"] = hash
        let pin = GraphicsTemplatePin(templateId: "pinned-title", templateVersion: 1, manifestHash: hash,
                                      definition: ["templateId": "pinned-title", "templateVersion": 1, "runtime": "declarative",
                                                   "fields": [["id": "heading", "type": "string"]]], packageRoot: "")
        model.playout.pins = { [pin] }
        let result = showAndSettle(model, GraphicsPlayout.entry(for: rows[1]))
        expectEqual(result?.accepted, true, "a pinned template missing from the core is installed, then the row plays")
        expectEqual(Array(core.transcript.prefix(3)), ["graphics-template-install", "graphics-template-list", "graphics-instance-create:playlist-title"],
                    "install -> list -> create")
        core.node["installHash"] = String(repeating: "d", count: 64)
        core.node["templates"] = realCatalogTemplates
        var other = rows
        other[1].templateId = "pinned-title"
        let fresh = playoutModel(other)
        fresh.1.node["installHash"] = String(repeating: "d", count: 64)
        fresh.0.playout.pins = { [pin] }
        let mismatch = showAndSettle(fresh.0, GraphicsPlayout.entry(for: other[1]))
        expectEqual(mismatch?.reason, "Show title: the installed template differs from its saved pin. Program is unchanged.",
                    "an installed template whose hash differs from the pin is refused")
    }

    @MainActor
    static func testGraphicsRowWithoutTemplateRefused() {
        let rows = interviewRows()
        let sponsor = rows.first { $0.id == "sponsor" }!
        let (model, core) = playoutModel(rows)
        let result = showAndSettle(model, GraphicsPlayout.entry(for: sponsor))
        expectEqual(result?.reason, "Sponsor card has no graphic yet. Choose an image or template for it first.", "placeholder row refused")
        expect(core.commands.isEmpty, "nothing sent")
    }
}
