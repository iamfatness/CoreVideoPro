// macOS control parity, Phase 4 RC7 (docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md,
// Batch C). Every refusal sentence and effect asserted here comes from
// contracts/control.action-vectors.json, which the WINDOWS tests write by running the Windows helpers
// (CT/WT/MCT ControlActionVectorExportTests). Nothing in a vector is typed on this side.
// ContractParity skips *Tests.swift files.

import Foundation

/// One case of contracts/control.action-vectors.json.
struct ActionVector {
    let name: String
    let action: String
    /// JSON args as the router would bind them: Bool, String, Int, Double or nil.
    let args: [Any?]
    let given: [String: Any]
    let ok: Bool
    let error: String?
    /// When set, only this prefix of `error` is portable (runtime exception text follows it).
    let errorPrefix: String?
    let effects: [String: Any]
}

extension ShellTests {

    @MainActor
    static var controlRC7Cases: [(String, () -> Void)] {
        [
            ("control/action-vectors-shape", testActionVectorsShape),
            ("control/rc7-srt-vectors", testRC7SrtVectors),
            ("control/rc7-srt-host-injection", testRC7SrtHostInjection),
            ("control/rc7-multiview-actions", testRC7MultiviewActions),
            ("control/rc7-scene-route-parse", testRC7SceneRouteParse),
            ("control/rc7-scene-routes", testRC7SceneRoutes),
            ("control/rc7-restore-starters", testRC7RestoreStarters),
        ]
    }

    /// A JSON scalar as the control binder sees it (CFBoolean is a Bool, never 0/1).
    static func vectorScalar(_ value: Any) -> Any? {
        if value is NSNull { return nil }
        if let number = value as? NSNumber {
            if CFGetTypeID(number) == CFBooleanGetTypeID() { return number.boolValue }
            if CFNumberIsFloatType(number) { return number.doubleValue }
            return number.intValue
        }
        return value
    }

    static func actionVectorSections() -> [String: Any]? {
        repoJSON("contracts/control.action-vectors.json")?["sections"] as? [String: Any]
    }

    /// The cases of one section, in file order. Empty (and a failed check) when the section is missing.
    static func loadActionVectors(section: String) -> [ActionVector] {
        guard let body = actionVectorSections()?[section] as? [String: Any],
              let cases = body["cases"] as? [[String: Any]] else {
            expect(false, "contracts/control.action-vectors.json has section '\(section)'")
            return []
        }
        return cases.map { row in
            let expectation = row["expect"] as? [String: Any] ?? [:]
            return ActionVector(
                name: row["name"] as? String ?? "?",
                action: row["action"] as? String ?? "",
                args: (row["args"] as? [Any] ?? []).map(vectorScalar),
                given: row["given"] as? [String: Any] ?? [:],
                ok: expectation["ok"] as? Bool ?? false,
                error: expectation["error"] as? String,
                errorPrefix: expectation["errorPrefix"] as? String,
                effects: expectation["effects"] as? [String: Any] ?? [:])
        }
    }

    /// The reply matches the vector's ok/error (exactly, or by its declared portable prefix).
    static func expectVectorReply(_ result: ControlInvokeResult?, _ vector: ActionVector, _ label: String) {
        guard let result else { expect(false, "\(label): answered"); return }
        expectEqual(result.ok, vector.ok, "\(label): ok")
        guard let error = vector.error else {
            expect(result.error == nil, "\(label): no error (got \(result.error ?? ""))")
            return
        }
        if let prefix = vector.errorPrefix {
            expect(error.hasPrefix(prefix) && (result.error ?? "").hasPrefix(prefix)
                   && (result.error ?? "").count > prefix.count,
                   "\(label): error starts with '\(prefix)' and says why (got \(result.error ?? "nil"))")
        } else {
            expectEqual(result.error, error, "\(label): Windows sentence")
        }
    }

    /// Drift guard for the shared file: parses, every section names its source and writer and has
    /// cases, every action is a Windows registry id, and no case name repeats within a section.
    /// Mutations: a case naming `scene.bogus`, or a deleted `writer`, turn this red.
    static func testActionVectorsShape() {
        guard let file = repoJSON("contracts/control.action-vectors.json") else {
            expect(false, "contracts/control.action-vectors.json parses"); return
        }
        expectEqual(file["version"] as? Int, 1, "version 1")
        guard let sections = file["sections"] as? [String: Any], !sections.isEmpty else {
            expect(false, "the file has sections"); return
        }
        let registry = Set((repoJSON("contracts/control.manifest.json")?["Actions"] as? [[String: Any]] ?? [])
            .compactMap { $0["Id"] as? String })
        expect(registry.count == 110, "the Windows registry has 110 actions (fixture-derived operand)")
        for (name, value) in sections.sorted(by: { $0.key < $1.key }) {
            guard let section = value as? [String: Any] else { expect(false, "\(name) is an object"); continue }
            expect(!((section["source"] as? String) ?? "").isEmpty, "\(name) names its Windows source")
            expect(["CT", "WT", "MCT"].contains(section["writer"] as? String ?? ""), "\(name) names its writer (CT|WT|MCT)")
            let cases = section["cases"] as? [[String: Any]] ?? []
            expect(!cases.isEmpty, "\(name) has at least one case")
            var seen = Set<String>()
            for row in cases {
                let caseName = row["name"] as? String ?? ""
                expect(!caseName.isEmpty && seen.insert(caseName).inserted, "\(name)/\(caseName): a unique case name")
                let action = row["action"] as? String ?? ""
                expect(registry.contains(action), "\(name)/\(caseName): '\(action)' is a Windows registry id")
                expect((row["expect"] as? [String: Any])?["ok"] is Bool, "\(name)/\(caseName): expect.ok is a bool")
            }
        }
        for required in ["scene-routes"] {
            expect(sections[required] != nil, "section \(required) is present")
        }
    }

    /// Task 2: every srt-configure vector against a real AppModel. `saved` is the
    /// save seam being called; `applied` is the model holding the new config after
    /// the call; `steps` is their order (the model must still hold the baseline
    /// when the save runs). Mutations: validate before the busy check (busy case:
    /// mode refusal instead of busy); treat an omitted passphrase as empty (kept
    /// case); apply before save (steps); interpolate the mode into the refusal
    /// (secret-leak assertion).
    @MainActor
    static func testRC7SrtVectors() {
        let vectors = loadActionVectors(section: "srt-configure")
        expect(vectors.count >= 19, "the srt-configure section carries the plan's cases (\(vectors.count))")
        // Fixture invariant: the baseline differs from every saved config in host and
        // port, so "applied" and "steps" can tell old from new.
        var baseline = StreamDestinationsConfig()
        baseline.srtHost = "baseline.example"
        baseline.srtPort = "7000"
        baseline.hlsPlaylistUrl = "https://hls.example/keep.m3u8"   // a non-SRT field is kept
        for vector in vectors {
            let label = "srt/\(vector.name)"
            let model = takeModel(FakeTakeCore())
            let stored = vector.given["storedPassphrase"] as? String ?? ""
            model.streamDestinations = baseline
            model.srtPassphrase = stored
            if vector.given["busy"] as? Bool == true { model.streamingDesired = true }
            var steps: [String] = []
            var savedPrefs: ShellPrefs?
            var savedPassphrase: String?
            model.srtDestinationSaver = { prefs, passphrase in
                steps.append(model.streamDestinations == baseline && model.srtPassphrase == stored ? "save" : "save-after-apply")
                savedPrefs = prefs
                savedPassphrase = passphrase
                return true
            }
            let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
            let result = invoke(surface, vector.action, vector.args)
            expectVectorReply(result, vector, label)
            for secret in ["invalid-mode-test-secret", "test-passphrase-sentinel"] {
                expect(!(result?.error ?? "").contains(secret), "\(label): the reply never echoes '\(secret)'")
            }
            let applied = model.streamDestinations != baseline || model.srtPassphrase != stored
            if applied { steps.append("apply") }
            let effects = vector.effects
            expectEqual(!steps.isEmpty, effects["saved"] as? Bool ?? false, "\(label): saved")
            expectEqual(applied, effects["applied"] as? Bool ?? false, "\(label): applied")
            expectEqual(steps, effects["steps"] as? [String] ?? [], "\(label): save runs before apply")
            expect(!model.streamingDesired || vector.given["busy"] as? Bool == true, "\(label): never starts streaming")
            guard effects["saved"] as? Bool == true else {
                expectEqual(model.streamDestinations, baseline, "\(label): a refusal changes nothing")
                expectEqual(model.srtPassphrase, stored, "\(label): passphrase untouched")
                continue
            }
            let config = model.streamDestinations
            expectEqual(config.srtEnabled, effects["enabled"] as? Bool ?? !config.srtEnabled, "\(label): enabled")
            for (key, value) in [("mode", config.srtMode), ("host", config.srtHost), ("port", config.srtPort),
                                 ("latencyMs", config.srtLatencyMs), ("streamId", config.srtStreamId),
                                 ("keyLength", config.srtKeyLength)] {
                expectEqual(value, effects[key] as? String ?? "<absent>", "\(label): \(key)")
            }
            let passphrase = model.srtPassphrase
            let kind = passphrase.isEmpty ? "cleared" : (!stored.isEmpty && passphrase == stored ? "kept" : "set")
            expectEqual(kind, effects["passphrase"] as? String ?? "<absent>", "\(label): passphrase")
            expectEqual(savedPassphrase, passphrase, "\(label): the saved passphrase is the applied one")
            if let savedPrefs { expectEqual(savedPrefs.streamDestinations, config, "\(label): the saved prefs carry the applied config") }
            expectEqual(config.hlsPlaylistUrl, baseline.hlsPlaylistUrl, "\(label): non-SRT fields are kept")
        }
        // Storage failure: Windows' sentence (StreamDestinationControl.cs:56), nothing applied.
        let model = takeModel(FakeTakeCore())
        model.streamDestinations = baseline
        model.srtDestinationSaver = { _, _ in false }
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let valid = vectors.first { $0.name == "valid-caller-with-passphrase" }?.args ?? []
        expectEqual(invoke(surface, "transport.stream.srt.configure", valid),
                    .fail("Destination settings were not saved. Previous settings remain active; retry after resolving preference storage."),
                    "srt: storage failure")
        expectEqual(model.streamDestinations, baseline, "srt: a failed save applies nothing")
        expectEqual(model.srtPassphrase, "", "srt: passphrase unchanged after a failed save")
    }

    /// Task 3: the four multiview.show*.set actions replayed from section
    /// multiview-overlays: each flips exactly its flag, the resulting command equals
    /// Windows', a change sends ONE configure-multiviewer and a repeat sends none, and
    /// the flag reaches prefs (a relaunch restores it). Mutations: write the flag
    /// without persisting (relaunch assertion); send twice for false-then-false.
    @MainActor
    static func testRC7MultiviewActions() {
        func overlays(_ o: [String: Any]) -> MultiviewOverlays {
            MultiviewOverlays(showLabels: o["showLabels"] as? Bool ?? false, showTally: o["showTally"] as? Bool ?? false,
                              showMeters: o["showMeters"] as? Bool ?? false, showClock: o["showClock"] as? Bool ?? false)
        }
        let vectors = loadActionVectors(section: "multiview-overlays")
        expectEqual(vectors.count, 4, "four overlay fixtures")
        for vector in vectors {
            let label = "multiview/\(vector.name)"
            let model = takeModel(FakeTakeCore())
            model.multiviewLayoutMode = vector.given["layoutMode"] as? String ?? ""
            model.multiviewTileCount = vector.given["tileCount"] as? Int ?? 0
            let before = overlays(vector.given)
            model.multiviewOverlays = before
            var sent: [JSONObject] = []
            model.coreCommandCaptureForTesting = { body in
                sent += ((body["commands"] as? [JSONObject]) ?? [body]).filter { $0["type"] as? String == "configure-multiviewer" }
            }
            let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
            expectVectorReply(invoke(surface, vector.action, vector.args), vector, label)
            let expected = vector.effects["command"] as? [String: Any] ?? [:]
            let after = overlays(expected)
            expectEqual(model.multiviewOverlays, after, "\(label): exactly the named flag moved")
            expectEqual(sent.count, before == after ? 0 : 1, "\(label): one command per change, none for a repeat")
            if let command = sent.last {
                expect(NSDictionary(dictionary: command).isEqual(to: expected), "\(label): the sent command is Windows'")
            }
            let relaunched = AppModel()
            relaunched.applyRestoredPrefs(model.currentPrefs())
            expectEqual(relaunched.multiviewOverlays, after, "\(label): the flag persists across a relaunch")
            // A repeat of the same value sends nothing.
            _ = invoke(surface, vector.action, vector.args)
            expectEqual(sent.count, before == after ? 0 : 1, "\(label): repeating the value sends nothing more")
        }
    }

    /// Task 4: section scene-routes through the Mac twins. Parse cases through
    /// SceneRoutesArgs.parse (exact Windows sentences; the runtime JSON-exception case by
    /// its declared prefix), refuse cases through SceneRoutesArgs.refuse with the
    /// vector's own known lists. Mutation: accept 1.5 as slot 1 (parse-fractional-slot).
    static func testRC7SceneRouteParse() {
        let vectors = loadActionVectors(section: "scene-routes")
        expect(vectors.count >= 14, "the scene-routes section carries the plan's inputs (\(vectors.count))")
        for vector in vectors {
            let label = "routes/\(vector.name)"
            let raw = vector.args.count > 1 ? vector.args[1] as? String ?? "" : ""
            switch vector.given["step"] as? String {
            case "parse":
                switch SceneRoutesArgs.parse(raw) {
                case .routes(let routes):
                    expectVectorReply(.success, vector, label)
                    let expected = (vector.effects["routes"] as? [String: Any] ?? [:]).mapValues { vectorScalar($0) as? Int }
                    expectEqual(routes.count, expected.count, "\(label): route count")
                    for entry in routes.entries {
                        expect(expected.keys.contains(entry.route), "\(label): route \(entry.route) expected")
                        expectEqual(entry.slot, expected[entry.route] ?? nil, "\(label): slot for \(entry.route)")
                    }
                    expectEqual(routes.entries.map { $0.route }, vector.effects["order"] as? [String] ?? [],
                                "\(label): Windows' enumeration order")
                case .refused(let error):
                    expectVectorReply(.fail(error), vector, label)
                }
            case "refuse":
                guard case .routes(let routes) = SceneRoutesArgs.parse(raw) else {
                    expect(false, "\(label): the refuse input parses"); continue
                }
                let refusal = SceneRoutesArgs.refuse(
                    sceneId: vector.args.first as? String ?? "",
                    knownScenes: vector.given["scenes"] as? [String] ?? [],
                    knownRouteIds: vector.given["routeIds"] as? [String] ?? [],
                    knownSlots: vector.given["slots"] as? [Int] ?? [],
                    routes: routes)
                expectVectorReply(refusal.map { .fail($0) } ?? .success, vector, label)
            default:
                expect(false, "\(label): a known step")
            }
        }
        expect(vectors.contains { $0.name == "parse-duplicate-key" },
               "the repeated-key order comes from Windows (parse-duplicate-key), not from this file")
    }

    /// The Task 4 fixture: takeModel (Program s2, Preview s1) plus scene "two-up" whose
    /// preset layers are two-up-layer-0 and two-up-layer-1; slots 1-10.
    @MainActor
    static func sceneRoutesModel() -> AppModel {
        let model = takeModel(FakeTakeCore())
        model.scenes.append(SceneDef(id: "two-up", name: "Side by side", layout: "two-up"))
        return model
    }

    /// Task 4: the refuse vectors (whose known lists ARE this fixture) through the real
    /// dispatcher, all-or-nothing. Mutations: apply routes one by one, refusing midway
    /// (refuse-unknown-route: layer 0 moved); select Preview before validating (Preview
    /// moved on a refusal).
    @MainActor
    static func testRC7SceneRoutes() {
        let model = sceneRoutesModel()
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        // Fixture invariants (rule 1): the vectors' lists equal the fixture's.
        let refuse = loadActionVectors(section: "scene-routes").filter { $0.given["step"] as? String == "refuse" }
        expect(!refuse.isEmpty, "refuse vectors present")
        expectEqual(model.sceneLayerCandidates("two-up")?.map { $0.id }, ["two-up-layer-0", "two-up-layer-1"],
                    "fixture: two-up's route ids are its two layer ids")
        expectEqual(model.slots.map(\.id), Array(1...10), "fixture: slots 1-10, slot 11 does not exist")
        for vector in refuse {
            expectEqual(vector.given["routeIds"] as? [String], ["two-up-layer-0", "two-up-layer-1"], "\(vector.name): routeIds match the fixture")
            expectEqual(vector.given["slots"] as? [Int], Array(1...10), "\(vector.name): slots match the fixture")
        }
        func layers() -> [SceneLayer] { model.scenes.first { $0.id == "two-up" }?.layers ?? [] }

        for vector in refuse where !vector.ok {
            model.previewSceneId = "s1"   // every refusal starts off two-up, so a moved Preview shows
            let preview = model.previewSceneId, before = layers(), syncs = model.sceneSyncRequestCount
            let result = invoke(surface, vector.action, vector.args)
            expectVectorReply(result, vector, "routes-model/\(vector.name)")
            expectEqual(model.previewSceneId, preview, "\(vector.name): Preview did not move")
            expectEqual(layers(), before, "\(vector.name): no layer changed (all-or-nothing)")
            expectEqual(model.sceneSyncRequestCount, syncs, "\(vector.name): nothing was sent")
        }

        guard let success = refuse.first(where: { $0.ok }) else { expect(false, "a success vector"); return }
        let syncs = model.sceneSyncRequestCount
        expectEqual(invoke(surface, success.action, success.args), .appliedPreview("two-up"), "success: applied to Preview")
        expectEqual(model.previewSceneId, "two-up", "success: Preview is the scene")
        expectEqual(layers().map(\.slotId), [1, nil], "success: layer 0 -> slot 1, layer 1 -> active speaker (null)")
        expectEqual(model.sceneSyncRequestCount, syncs + 1, "success: ONE scene sync")
        expectEqual(model.programSceneId, "s2", "Program is untouched")

        // scene.route.set: a one-entry map; then null clears a slot back to the active speaker.
        expectEqual(invoke(surface, "scene.route.set", ["two-up", "two-up-layer-1", 4]), .appliedPreview("two-up"), "route.set")
        expectEqual(layers().map(\.slotId), [1, 4], "route.set writes only its layer")
        expectEqual(invoke(surface, "scene.cueWithRoutes", ["two-up", #"{"two-up-layer-1":null}"#]), .appliedPreview("two-up"), "null route")
        expectEqual(layers().map(\.slotId), [1, nil], "null clears the layer to the active speaker")
        expectEqual(invoke(surface, "scene.route.set", ["two-up", "two-up-layer-0", 11]),
                    .fail("Unknown Show Input slot 11 for route 'two-up-layer-0'."), "route.set: unknown slot (same Refuse)")
        expectEqual(layers().map(\.slotId), [1, nil], "a refused route.set changes nothing")

        // A parse refusal changes nothing either.
        let preview = model.previewSceneId
        model.previewSceneId = "s1"
        let result = invoke(surface, "scene.cueWithRoutes", ["two-up", "not json"])
        expect(result?.error?.hasPrefix(SceneRoutesArgs.notJsonPrefix) == true, "parse refusal: Windows prefix (\(result?.error ?? "nil"))")
        expectEqual(model.previewSceneId, "s1", "parse refusal: Preview did not move")
        model.previewSceneId = preview
    }

    /// Task 5: scene.restoreStarters. The status sentences are the restore-starters
    /// vectors (Windows SceneCatalogPolicy.RestoreStatus); the model cases delete
    /// starters through the real deleteScene and restore through the real dispatcher.
    /// Mutations: pluralise "1 starter scenes restored" (one-missing vector); restore a
    /// starter whose id is already present (duplicate assertion); forget the removed id
    /// on delete (leftovers/starter-removed-set-persists).
    @MainActor
    static func testRC7RestoreStarters() {
        let vectors = loadActionVectors(section: "restore-starters")
        expectEqual(vectors.count, 3, "0, 1 and 3 restored")
        for vector in vectors {
            let names = vector.given["restoredNames"] as? [String] ?? []
            expectEqual(AppModel.restoreStatus(names), vector.effects["status"] as? String ?? "<absent>",
                        "restore/\(vector.name): Windows sentence")
        }
        // Fixture invariant: three Mac starters in catalog order.
        expectEqual(AppModel.starterSceneIds, ["fullscreen", "two-up", "pip"], "the Mac starter catalog")

        func fresh() -> (AppModel, AppModelControlSurface) {
            let model = AppModel()
            model.programSceneId = "fullscreen"
            model.previewSceneId = "pip"
            return (model, AppModelControlSurface(model: model, box: model.controlSnapshotBox))
        }

        // Nothing missing.
        do {
            let (model, surface) = fresh()
            let syncs = model.sceneSyncRequestCount
            expectEqual(invoke(surface, "scene.restoreStarters"), .success, "restore always succeeds")
            expectEqual(model.studioCommandStatus, AppModel.restoreStatus([]), "nothing missing: the zero sentence")
            expectEqual(model.scenes.map(\.id), AppModel.starterSceneIds, "nothing added")
            expectEqual(model.sceneSyncRequestCount, syncs, "nothing missing sends no scene sync")
        }
        // One missing: back at its catalog position with fresh preset layers.
        do {
            let (model, surface) = fresh()
            if let index = model.scenes.firstIndex(where: { $0.id == "two-up" }) {
                model.scenes[index].layers = [SceneLayer(id: "two-up-layer-0", slotId: 7)]
            }
            expect(model.deleteScene("two-up") == nil, "two-up is removable")
            let restoreSyncsBefore = model.sceneSyncRequestCount
            expectEqual(invoke(surface, "scene.restoreStarters"), .success, "restore one")
            expectEqual(model.scenes.map(\.id), AppModel.starterSceneIds, "two-up is back between fullscreen and pip")
            expectEqual(model.scenes.first { $0.id == "two-up" }?.layers, [], "with fresh preset layers, not the deleted edit")
            expectEqual(model.studioCommandStatus, AppModel.restoreStatus(["Side by side"]), "one restored: names it")
            expectEqual(model.studioCommandStatus, "Side by side restored", "fixture-derived operand: the Mac starter's name")
            expectEqual(model.programSceneId, "fullscreen", "Program untouched")
            expectEqual(model.previewSceneId, "pip", "Preview untouched")
            expectEqual(model.currentPrefs().removedStarterSceneIds, nil, "the removed set is empty after restore")
            expectEqual(model.sceneSyncRequestCount, restoreSyncsBefore + 1, "restore sends ONE scene sync")
            let saved = ShellPrefs.load()
            expect(saved.scenes.contains { $0.id == "two-up" }, "restore wrote the restored scene to the prefs file")
            expectEqual(saved.removedStarterSceneIds, nil, "and the cleared removed set")
        }
        // All three missing (a custom scene keeps the rail non-empty).
        do {
            let (model, surface) = fresh()
            model.newScene(layout: "single")
            let custom = model.scenes.last?.id ?? ""
            model.programSceneId = custom
            model.previewSceneId = custom
            for id in AppModel.starterSceneIds { expect(model.deleteScene(id) == nil, "\(id) removable") }
            expectEqual(model.removedStarterSceneIds, AppModel.starterSceneIds, "three recorded, in delete order")
            expectEqual(invoke(surface, "scene.restoreStarters"), .success, "restore three")
            expectEqual(model.studioCommandStatus, AppModel.restoreStatus(["a", "b", "c"]), "three restored: counted")
            expectEqual(Set(model.scenes.map(\.id)), Set(AppModel.starterSceneIds + [custom]), "all starters back, custom kept")
            expectEqual(model.scenes.filter { AppModel.starterSceneIds.contains($0.id) }.map(\.id), AppModel.starterSceneIds,
                        "starters back in catalog order")
        }
        // A starter id already present is never duplicated, even when the removed set names it.
        do {
            let (model, surface) = fresh()
            model.removedStarterSceneIds = ["two-up"]
            expectEqual(invoke(surface, "scene.restoreStarters"), .success, "restore with a stale removed set")
            expectEqual(model.scenes.filter { $0.id == "two-up" }.count, 1, "a present starter is not duplicated")
            expectEqual(model.studioCommandStatus, AppModel.restoreStatus([]), "nothing was missing")
            expectEqual(model.removedStarterSceneIds, ["two-up"], "nothing missing: Windows returns early, the set is untouched")
        }
    }

    /// SECURITY (2026-11-23 review): the host lands in the srt:// URL whose query carries
    /// mode, passphrase and streamid. An IPv6 zone id is accepted only as 1-15
    /// [A-Za-z0-9_.-]; '?', '&', '#', '/', '%', '@' and whitespace never pass. (Windows'
    /// Uri.CheckHostName accepts the zone-id strings below; the native URL builder refuses
    /// them too, SrtFfmpegArgsTest RefusesHostsThatCouldInjectUrlOptions.)
    /// Mutation: accept everything after '%' (the first five strings go green).
    @MainActor
    static func testRC7SrtHostInjection() {
        let refused = [
            "fe80::1%en0?passphrase=x&mode=listener", "fe80::1%en0&mode=listener", "fe80::1%en0#x",
            "fe80::1%en 0", "fe80::1%", "fe80::1%abcdefghijklmnop", "fe80::1%en0/x",
            "host?mode=listener", "host&mode=listener", "user@host", "host name",
        ]
        for host in refused {
            expect(!SrtDestinationControlPolicy.isHostName(host), "refused host: \(host)")
            let model = takeModel(FakeTakeCore())
            var saved = false
            model.srtDestinationSaver = { _, _ in saved = true; return true }
            let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
            let result = invoke(surface, "transport.stream.srt.configure",
                                [true, "caller", host, "9000", "120", "", "0"])
            expectEqual(result, .fail(SrtDestinationControlPolicy.hostRefusal), "configure refuses \(host)")
            expect(!saved, "\(host): nothing saved")
            expect(!(result?.error ?? "").contains("passphrase=x"), "\(host): the reply never echoes it")
        }
        for host in ["fe80::1%en0", "fe80::1%utun_3.a-b", "::1", "example.test", "192.168.1.20"] {
            expect(SrtDestinationControlPolicy.isHostName(host), "accepted host: \(host)")
        }
    }
}
