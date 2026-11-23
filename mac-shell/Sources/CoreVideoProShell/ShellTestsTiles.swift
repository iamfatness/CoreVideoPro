// CoreVideo Tiles on the Mac shell (docs/superpowers/plans/2026-11-22-mac-tiles.md).
// Kept in its own file so parallel Mac branches extending ShellTests.swift do not
// collide line-for-line. Each assertion message names the Windows test it ports.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var tilesCases: [(String, () -> Void)] {
        [
            // Task 6: TilesSettings model and persistence.
            ("tiles/defaults-are-windows", testTilesDefaultsAreTheWindowsDefaults),
            ("tiles/persisted-round-trip-and-clamp", testPersistedTilesRoundTripAndClampLikeWindows),
            ("tiles/partial-or-newer-object", testAPartialOrNewerTilesObjectDecodesWithDefaultsAndKeepsEveryOtherPref),
            ("tiles/legacy-layout-default", testALegacyDynamicGalleryLayoutWithoutTilesGetsDefaultSettings),
            ("tiles/duplicate-copies-value", testDuplicatingATilesSceneCopiesItsSettingsNotAReference),
            ("tiles/hex-normalizer", testTilesHexMatchesTheWindowsNormalizer),
            ("tiles/clamp-other-ends-and-caps", testPersistedTilesClampTheOtherEndsAndCapTheLists),
            ("tiles/bad-values-cost-only-themselves", testABadTilesValueCostsOnlyItself),
            ("tiles/save-and-relaunch-through-the-model", testTilesSurviveASaveAndRelaunchThroughTheModel),
            // Task 7: the `tiles` payload, confirmed against the Windows builder.
            ("tiles/payload-fixtures", testEveryTilesFixtureMatchesTheSwiftBuilder),
            ("tiles/payload-windows-facts", testTilesPayloadPortsTheWindowsBuilderFacts),
            ("tiles/local-source-policy", testTilesLocalSourcePolicyPortsTheWindowsFacts),
        ]
    }

    // ── Task 6 ───────────────────────────────────────────────────────────────

    static func testTilesDefaultsAreTheWindowsDefaults() {
        let w = "StarterSceneFactoryTests.TilesStarterPersistsCoreOwnedMembershipAndIsDistinctFromPanelGrid"
        let s = TilesSettings()
        expectEqual(s.maxTiles, 16, "\(w): maxTiles")
        expectEqual(s.gutterPercent, 0.741, "\(w): gutterPercent")
        expectEqual(s.marginPercent, 0.741, "\(w): marginPercent")
        expectEqual(s.membershipMode, "eligible", "\(w): membershipMode")
        expect(s.autoFill, "\(w): autoFill")
        expectEqual(s.cornerRadius, 16.0, "\(w): cornerRadius")
        expectEqual(s.glowColor, "#FFFFFF", "\(w): glowColor")
        expectEqual(s.glowIntensity, 100.0, "\(w): glowIntensity")
        expectEqual(s.animationDurationMs, 350, "\(w): animationDurationMs")
        expectEqual(s.backgroundColor, "#000000", "\(w): backgroundColor")
        expectEqual(s.borderColor, "#000000", "\(w): borderColor")
        expectEqual(s.tileAspect, "16:9", "\(w): tileAspect")
        expectEqual(s.wallRect, TilesRect(x: 0, y: 0, width: 1, height: 1), "\(w): wallRect")
        expect(s.wallVisible && !s.wallLocked && !s.transparentBackground, "\(w): wall flags")
        expect(!s.nameEachTile, "\(w): nameEachTile off")
    }

    private static func decodePersistedTiles(_ json: String) -> PersistedTilesSettings? {
        try? JSONDecoder().decode(PersistedTilesSettings.self, from: Data(json.utf8))
    }

    static func testPersistedTilesRoundTripAndClampLikeWindows() {
        let w = "ScenePersistenceService.FromPersisted"
        var edited = TilesSettings()
        edited.maxTiles = 9
        edited.membershipMode = "manual"
        edited.autoFill = false
        edited.manualSlots = ["zoom-pid:abc", nil, "capture:cam-a"]
        edited.excludedSourceIds = ["zoom-name:Ann"]
        edited.boundMeetingId = "8675309"
        edited.borderShape = "rounded"
        edited.borderColor = "#657D87"
        edited.glowSize = 12
        edited.overrides = ["zoom:7": TilesMemberOverride(rect: TilesRect(x: 0.5, y: 0, width: 0.5, height: 1),
                                                          cropLeftPercent: 10, cropRightPercent: 20, z: 3)]
        // Through JSON too: what the prefs file stores must come back equal.
        let data = try? JSONEncoder().encode(edited.persisted)
        let decoded = data.flatMap { try? JSONDecoder().decode(PersistedTilesSettings.self, from: $0) }
        expect(decoded != nil, "\(w): persisted settings encode and decode")
        if let decoded { expectEqual(TilesSettings(persisted: decoded), edited, "\(w): round trip") }

        guard let wild = decodePersistedTiles("""
            {"maxTiles": 99, "glowSize": 500, "borderShape": "hex", "borderColor": "abc123",
             "tileAspect": "21:9", "autoFill": false, "glowColor": "", "customAspectRatio": 9,
             "borderThickness": 99, "cornerRadius": -5, "animationDurationMs": 50,
             "excludedSourceIds": ["a", "", "a", "  ", "b"], "boundMeetingId": "  "}
            """) else {
            expect(false, "\(w): clamp fixture decodes"); return
        }
        let s = TilesSettings(persisted: wild)
        expectEqual(s.maxTiles, 64, "\(w): maxTiles clamps to 64")
        expectEqual(s.glowSize, 64.0, "\(w): glowSize clamps to 64")
        expectEqual(s.borderShape, "square", "\(w): unknown shape is square")
        expectEqual(s.borderColor, "#ABC123", "\(w): colour normalized")
        expectEqual(s.glowColor, "#44C1A1", "\(w): blank colour is the route default")
        expectEqual(s.tileAspect, "16:9", "\(w): unknown aspect is 16:9")
        expectEqual(s.membershipMode, "manual", "\(w): absent mode defaults from autoFill=false")
        expectEqual(s.customAspectRatio, 4.0, "\(w): ratio clamps to 4")
        expectEqual(s.borderThickness, 32.0, "\(w): thickness clamps to 32")
        expectEqual(s.cornerRadius, 0.0, "\(w): radius clamps to 0")
        expectEqual(s.animationDurationMs, 100, "\(w): duration clamps to 100")
        expectEqual(s.excludedSourceIds, ["a", "b"], "\(w): excluded non-blank, deduped")
        expect(s.boundMeetingId == nil, "\(w): blank meeting id is nil")
    }

    @MainActor static func testAPartialOrNewerTilesObjectDecodesWithDefaultsAndKeepsEveryOtherPref() {
        let w = "Review Focus 1 (partial/newer tiles object)"
        let json = """
            {"joinMeetingId": "8675309",
             "scenes": [
               {"id": "wall", "name": "Wall", "layout": "dynamic-gallery", "backgroundAssetId": "",
                "layers": [], "tiles": {"maxTiles": 4, "futureKey": true}},
               {"id": "duo", "name": "Duo", "layout": "two-up", "backgroundAssetId": "", "layers": []}
             ]}
            """
        guard let prefs = try? JSONDecoder().decode(ShellPrefs.self, from: Data(json.utf8)) else {
            expect(false, "\(w): the whole prefs file decodes"); return
        }
        expectEqual(prefs.joinMeetingId, "8675309", "\(w): meeting id survives")
        expectEqual(prefs.scenes.map(\.id), ["wall", "duo"], "\(w): every scene survives")
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        var expected = TilesSettings()
        expected.maxTiles = 4
        expectEqual(model.scenes.first { $0.id == "wall" }?.tiles, expected,
                    "\(w): maxTiles 4, every other field at its default")
        expect(model.scenes.contains { $0.id == "duo" && $0.tiles == nil }, "\(w): the other scene restores, not a Tiles scene")
    }

    @MainActor static func testALegacyDynamicGalleryLayoutWithoutTilesGetsDefaultSettings() {
        let w = "ScenePersistenceService.SceneFromPersisted (docs/archive/qa/tiles-incident-2026-09-07.md)"
        var prefs = ShellPrefs()
        prefs.scenes = [PersistedScene(id: "legacy", name: "Legacy", layout: "dynamic-gallery"),
                        PersistedScene(id: "plain", name: "Plain", layout: "single")]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        expectEqual(model.scenes.first { $0.id == "legacy" }?.tiles, TilesSettings(), "\(w): default settings")
        expect(model.scenes.first { $0.id == "plain" }?.tiles == nil, "\(w): a plain scene stays plain")
    }

    /// Confirmatory (plan-authoring rule 8): TilesSettings is a value type, so a
    /// shallow copy cannot alias. Kept as documentation of the contract.
    @MainActor static func testDuplicatingATilesSceneCopiesItsSettingsNotAReference() {
        let w = "duplicateScene copies tiles by value"
        let model = AppModel()
        var edited = TilesSettings()
        edited.maxTiles = 9   // non-default, so a copy built from TilesSettings() is caught
        edited.glowColor = "#8AE0CA"
        model.scenes.append(SceneDef(id: "wall", name: "Wall", layout: "dynamic-gallery", tiles: edited))
        model.duplicateScene("wall")
        guard let copyIndex = model.scenes.firstIndex(where: { $0.id.hasPrefix("wall-copy-") }) else {
            expect(false, "\(w): a copy exists"); return
        }
        expectEqual(model.scenes[copyIndex].tiles, edited, "\(w): the copy carries the edited settings")
        model.scenes[copyIndex].tiles?.maxTiles = 3
        expectEqual(model.scenes.first { $0.id == "wall" }?.tiles?.maxTiles, 9, "\(w): the original keeps 9")
    }

    static func testTilesHexMatchesTheWindowsNormalizer() {
        let w = "SceneRoutingService.NormalizeBorderColor / HexColor.TryParse"
        expectEqual(TilesHex.normalize(""), "#44C1A1", "\(w): blank")
        expectEqual(TilesHex.normalize(nil), "#44C1A1", "\(w): nil")
        expectEqual(TilesHex.normalize("zz"), "#44C1A1", "\(w): wrong length")
        expectEqual(TilesHex.normalize(" abc123 "), "#ABC123", "\(w): trim, prefix, upper")
        expectEqual(TilesHex.normalize("#12345678"), "#12345678", "\(w): 9 chars pass")
        expect(TilesHex.parse("#GGGGGG") == nil, "\(w): non-hex refused")
        let p = TilesHex.parse("8ae0ca")
        expect(p.map { $0.r == 138 && $0.g == 224 && $0.b == 202 } ?? false, "\(w): 8ae0ca parses")
        expectEqual(TilesHex.format(r: 138, g: 224, b: 202), "#8AE0CA", "\(w): format upper")
    }

    static func testPersistedTilesClampTheOtherEndsAndCapTheLists() {
        let w = "ScenePersistenceService.FromPersisted (other bounds)"
        let manyOverrides = (0..<70).map { "\"zoom:\($0)\": {\"cropLeftPercent\": 1}" }.joined(separator: ",")
        let manySlots = (0..<70).map { "\"zoom-name:G\($0)\"" }.joined(separator: ",")
        let manyIds = (0..<1100).map { "\"zoom-name:X\($0)\"" }.joined(separator: ",")
        guard let low = decodePersistedTiles("""
            {"maxTiles": 0, "gutterPercent": -1, "marginPercent": -1, "wallOpacity": 2,
             "glowIntensity": -1, "glowSoftness": 101, "customAspectRatio": 0.1, "cornerRadius": 101,
             "glowSize": -1, "animationDurationMs": 3000, "overrides": {\(manyOverrides)},
             "manualSlots": [\(manySlots)], "excludedSourceIds": [\(manyIds)], "omittedSourceIds": [\(manyIds)],
             "membershipMode": " Routed "}
            """), let high = decodePersistedTiles("""
            {"gutterPercent": 11, "marginPercent": 21, "wallOpacity": -1, "glowIntensity": 101, "glowSoftness": -1}
            """) else {
            expect(false, "\(w): fixtures decode"); return
        }
        let s = TilesSettings(persisted: low)
        expectEqual(s.maxTiles, 1, "\(w): maxTiles low bound 1")
        expectEqual(s.gutterPercent, 0.0, "\(w): gutter low 0")
        expectEqual(s.marginPercent, 0.0, "\(w): margin low 0")
        expectEqual(s.wallOpacity, 1.0, "\(w): opacity high 1")
        expectEqual(s.glowIntensity, 0.0, "\(w): intensity low 0")
        expectEqual(s.glowSoftness, 100.0, "\(w): softness high 100")
        expectEqual(s.customAspectRatio, 0.25, "\(w): ratio low 0.25")
        expectEqual(s.cornerRadius, 100.0, "\(w): radius high 100")
        expectEqual(s.glowSize, 0.0, "\(w): glow low 0")
        expectEqual(s.animationDurationMs, 2000, "\(w): duration high 2000")
        expectEqual(s.overrides.count, 64, "\(w): overrides capped at 64")
        expectEqual(s.manualSlots.count, 64, "\(w): manual slots capped at 64")
        expectEqual(s.excludedSourceIds.count, 1024, "\(w): excluded capped at 1024")
        expectEqual(s.omittedSourceIds.count, 1024, "\(w): omitted capped at 1024")
        expectEqual(s.membershipMode, "routed", "\(w): mode trimmed and lowercased")
        let h = TilesSettings(persisted: high)
        expectEqual(h.gutterPercent, 10.0, "\(w): gutter high 10")
        expectEqual(h.marginPercent, 20.0, "\(w): margin high 20")
        expectEqual(h.wallOpacity, 0.0, "\(w): opacity low 0")
        expectEqual(h.glowIntensity, 100.0, "\(w): intensity high 100")
        expectEqual(h.glowSoftness, 0.0, "\(w): softness low 0")
    }

    @MainActor static func testABadTilesValueCostsOnlyItself() {
        let w = "Review Focus 1 (bad values inside and around the tiles object)"
        // A `tiles` that is not an object must not fail the scene: ShellPrefs drops
        // every scene when one fails.
        let json = """
            {"scenes": [
               {"id": "wall", "name": "Wall", "layout": "dynamic-gallery", "backgroundAssetId": "", "layers": [], "tiles": "oops"},
               {"id": "duo", "name": "Duo", "layout": "two-up", "backgroundAssetId": "", "layers": []}]}
            """
        let prefs = try? JSONDecoder().decode(ShellPrefs.self, from: Data(json.utf8))
        expectEqual(prefs?.scenes.map(\.id) ?? [], ["wall", "duo"], "\(w): a non-object tiles keeps every scene")
        guard let p = decodePersistedTiles("""
            {"wallRect": {"x": 0.5}, "maxTiles": "nine", "glowSize": 12,
             "overrides": {"zoom:7": {"rect": {"x": 0.5, "y": 0, "width": 0.5, "height": 1}},
                           "zoom:5": {"z": 3}, "zoom:9": "bad", "zoom:4": {"rect": "bad", "cropLeftPercent": 5}},
             "manualSlots": ["zoom-name:Ann", 7, null, "capture:cam-a"],
             "excludedSourceIds": ["a", 1, "b"]}
            """) else {
            expect(false, "\(w): the object decodes"); return
        }
        expectEqual(p.wallRect, TilesRect(x: 0.5, y: 0, width: 1, height: 1), "\(w): a partial wallRect keeps its key")
        expectEqual(p.maxTiles, 16, "\(w): a wrong-typed key takes its default")
        expectEqual(p.glowSize, 12.0, "\(w): its neighbours survive")
        expectEqual(p.overrides["zoom:7"], TilesMemberOverride(rect: TilesRect(x: 0.5, y: 0, width: 0.5, height: 1)),
                    "\(w): an override without crop keys survives")
        expectEqual(p.overrides["zoom:5"], TilesMemberOverride(z: 3), "\(w): a z-only override survives")
        expect(p.overrides["zoom:9"] == nil, "\(w): a non-object override is dropped alone")
        expectEqual(p.overrides["zoom:4"], TilesMemberOverride(cropLeftPercent: 5), "\(w): a bad override rect is no rect")
        expectEqual(p.manualSlots, ["zoom-name:Ann", nil, nil, "capture:cam-a"], "\(w): a bad slot is a hole, positions kept")
        expectEqual(p.excludedSourceIds, ["a", "b"], "\(w): a bad id is dropped alone")
    }

    @MainActor static func testTilesSurviveASaveAndRelaunchThroughTheModel() {
        let w = "AppModel.currentPrefs -> file -> applyRestoredPrefs"
        let model = AppModel()
        var edited = TilesSettings()
        edited.maxTiles = 7
        edited.borderShape = "rounded"
        edited.manualSlots = ["zoom-pid:abc", nil]
        model.scenes.append(SceneDef(id: "wall", name: "Wall", layout: "dynamic-gallery", tiles: edited))
        guard let data = try? JSONEncoder().encode(model.currentPrefs()),
              let restored = try? JSONDecoder().decode(ShellPrefs.self, from: data) else {
            expect(false, "\(w): prefs encode and decode"); return
        }
        let relaunched = AppModel()
        relaunched.applyRestoredPrefs(restored)
        expectEqual(relaunched.scenes.first { $0.id == "wall" }?.tiles, edited, "\(w): the edited settings come back")
    }

    // ── Task 7 ───────────────────────────────────────────────────────────────

    static func loadTilesFixtures() -> [[String: Any]]? {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        guard let data = try? Data(contentsOf: root.appendingPathComponent("contracts/tiles.fixtures.json")),
              let doc = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return doc["cases"] as? [[String: Any]]
    }

    /// JSON equality the way TilesFixtureTests judges it: same key sets, numbers
    /// within 1e-9 (Int vs Double is no difference), NSNull for null. Returns the
    /// first difference's path, or nil when equal.
    static func tilesJsonDifference(_ expected: Any?, _ actual: Any?, _ path: String) -> String? {
        let eNull = expected == nil || expected is NSNull, aNull = actual == nil || actual is NSNull
        if eNull || aNull { return eNull == aNull ? nil : "\(path): expected \(expected ?? "nil"), got \(actual ?? "nil")" }
        if let e = expected as? [String: Any] {
            guard let a = actual as? [String: Any] else { return "\(path): expected an object" }
            if Set(e.keys) != Set(a.keys) { return "\(path): keys \(e.keys.sorted()) vs \(a.keys.sorted())" }
            for key in e.keys.sorted() {
                if let d = tilesJsonDifference(e[key], a[key], path + "." + key) { return d }
            }
            return nil
        }
        if let e = expected as? [Any] {
            guard let a = actual as? [Any], a.count == e.count else { return "\(path): expected \(e), got \(actual ?? "nil")" }
            for i in e.indices { if let d = tilesJsonDifference(e[i], a[i], "\(path)[\(i)]") { return d } }
            return nil
        }
        if let e = expected as? NSNumber, let a = actual as? NSNumber {
            // JSONSerialization booleans are NSNumbers too: compare kinds first.
            let eBool = CFGetTypeID(e) == CFBooleanGetTypeID(), aBool = CFGetTypeID(a) == CFBooleanGetTypeID()
            if eBool || aBool { return eBool == aBool && e.boolValue == a.boolValue ? nil : "\(path): expected \(e), got \(a)" }
            return abs(e.doubleValue - a.doubleValue) <= 1e-9 ? nil : "\(path): expected \(e), got \(a)"
        }
        if let e = expected as? Bool, let a = actual as? Bool { return e == a ? nil : "\(path): expected \(e), got \(a)" }
        if let e = expected as? String, let a = actual as? String { return e == a ? nil : "\(path): expected \(e), got \(a)" }
        return "\(path): type mismatch \(String(describing: expected)) vs \(String(describing: actual))"
    }

    /// The payload as it reaches the core: through JSON, so Int/Double/NSNull
    /// shapes are the wire's, not Swift's.
    static func tilesWire(_ object: JSONObject?) -> Any? {
        guard let object, let data = try? JSONSerialization.data(withJSONObject: object) else { return nil }
        return try? JSONSerialization.jsonObject(with: data)
    }

    static func testEveryTilesFixtureMatchesTheSwiftBuilder() {
        let w = "TilesFixtureTests.EveryCaseMatchesTheWindowsBuilder (Swift twin)"
        guard let cases = loadTilesFixtures() else {
            expect(false, "\(w): contracts/tiles.fixtures.json must be readable from the harness"); return
        }
        var checked = 0
        for fixture in cases {
            let name = fixture["name"] as? String ?? "?"
            guard let sceneId = fixture["sceneId"] as? String,
                  let settingsJson = fixture["settings"].flatMap({ try? JSONSerialization.data(withJSONObject: $0) }),
                  let persisted = try? JSONDecoder().decode(PersistedTilesSettings.self, from: settingsJson),
                  let expected = fixture["expected"] as? [String: Any] else {
                expect(false, "\(w): \(name) is well formed"); continue
            }
            let settings = TilesSettings(persisted: persisted)
            // Fixture invariants (plan-authoring rules): checked before the builder.
            let members = expected["members"] as? [String] ?? []
            expect(members.count <= settings.maxTiles, "\(w): \(name) members within maxTiles")
            expect(((expected["style"] as? [String: Any])?["nameEachTile"] as? Bool) == false, "\(w): \(name) nameEachTile false")
            expect((expected["overrides"] as? [String: Any])?.keys.allSatisfy { $0.contains(":") } ?? false,
                   "\(w): \(name) override keys are scheme-qualified")
            let participants = (fixture["participants"] as? [[String: Any]] ?? []).map {
                RosterParticipant(id: $0["id"] as? String ?? "", name: $0["name"] as? String ?? "",
                                  hasVideo: $0["videoOn"] as? Bool ?? false, muted: false, talking: false, assigned: false,
                                  persistentId: $0["persistentId"] as? String)
            }
            expect(participants.allSatisfy { !$0.id.contains(":") }, "\(w): \(name) participant ids are bare")
            let slots = (fixture["slots"] as? [[String: Any]] ?? []).map { row -> ShowInputSlot in
                var slot = ShowInputSlot(id: row["slot"] as? Int ?? 0)
                slot.kind = row["kind"] as? String ?? "unassigned"
                slot.sourceId = row["sourceId"] as? String ?? ""
                slot.inShow = row["inShow"] as? Bool ?? false
                slot.name = row["name"] as? String ?? ""
                return slot
            }
            let local = TilesLocalSourcePolicy.localVideoSources(slots, devices: []).map(\.id)
            let payload = TilesLayerPayload.build(sceneId: sceneId, settings: settings, roster: participants,
                                                  localVideoSourceIds: local,
                                                  routedSourceIds: TilesLocalSourcePolicy.routedSourceIds(slots),
                                                  identityRoster: nil, meetingId: fixture["meetingId"] as? String)
            if let difference = tilesJsonDifference(expected, tilesWire(payload), "\(name).tiles") {
                expect(false, "\(w): \(difference)")
            }
            checked += 1
        }
        expectEqual(checked, cases.count, "\(w): every case checked")
        expect(checked >= 10, "\(w): at least 10 cases (a vacuous loop fails)")
    }

    private static func guest(_ id: String, video: Bool = true, talking: Bool = false, muted: Bool = false,
                              sharing: Bool = false) -> RosterParticipant {
        RosterParticipant(id: id, name: "Guest \(id)", hasVideo: video, muted: muted, talking: talking, assigned: false,
                          sharingScreen: sharing)
    }

    private static func members(_ roster: [RosterParticipant], settings: TilesSettings? = TilesSettings(),
                                local: [String] = [], routed: Set<String> = []) -> [String]? {
        TilesLayerPayload.build(sceneId: "scene-1", settings: settings, roster: roster, localVideoSourceIds: local,
                                routedSourceIds: routed, identityRoster: nil, meetingId: nil)?["members"] as? [String]
    }

    static func testTilesPayloadPortsTheWindowsBuilderFacts() {
        let t = "TilesLayerPayloadBuilderTests"
        expect(TilesLayerPayload.build(sceneId: "s", settings: nil, roster: [guest("1")], localVideoSourceIds: [],
                                       routedSourceIds: [], identityRoster: nil, meetingId: nil) == nil,
               "\(t).Build_ReturnsNullForANonGalleryScene")
        expectEqual(members(guestsRoster(["1", "p-2", "3"])) ?? [], ["zoom:1", "zoom:p-2", "zoom:3"],
                    "\(t).Build_CarriesEligibleMembersInRosterOrder")
        expectEqual(members([guest("1"), guest("p-2", video: false)]) ?? [], ["zoom:1"], "\(t).Build_SkipsParticipantsWithVideoOff")
        var two = TilesSettings(); two.maxTiles = 2
        expectEqual(members(guestsRoster(["1", "p-2", "3"]), settings: two) ?? [], ["zoom:1", "zoom:p-2"], "\(t).Build_TruncatesToMaxTiles")
        let busy = RosterParticipant(id: "1", name: "Busy", hasVideo: true, muted: true, talking: true, assigned: true,
                                     sharingScreen: true, sourceGeneration: 5)
        expectEqual(members([busy]) ?? [], ["zoom:1"], "\(t).Build_DoesNotDropAParticipantMerelyBecauseItLooksUnhealthy")
        expectEqual(members([guest("42")]) ?? [], ["zoom:42"], "\(t).Build_QualifiesBareIdsWithTheZoomScheme")
        expectEqual(members([guest("capture:cam-1")]) ?? [], ["capture:cam-1"], "\(t).Build_PassesThroughAnAlreadyQualifiedMemberUnchanged")
        let slots = [slot(1, "zoom", "1", inShow: true), slot(2, "capture", "cam-1", inShow: true),
                     slot(3, "capture", "screen:1", inShow: true), slot(4, "capture", "cam-offshow", inShow: false)]
        let local = TilesLocalSourcePolicy.localVideoSources(slots, devices: []).map(\.id)
        expectEqual(members([guest("1"), guest("p-2", video: false)], local: local,
                            routed: TilesLocalSourcePolicy.routedSourceIds(slots)) ?? [],
                    ["zoom:1", "capture:cam-1", "capture:screen:1"], "\(t).Build_SeatsInShowCaptureSlotsAfterTheZoomParticipants")
        var routedMode = TilesSettings(); routedMode.membershipMode = "routed"
        expectEqual(members(guestsRoster(["1", "2"]), settings: routedMode, local: ["capture:cam-1", "media:clip-7"],
                            routed: ["zoom:2", "capture:cam-1"]) ?? [],
                    ["zoom:2", "capture:cam-1"], "\(t).Build_RoutedModeFiltersLocalMembersLikeZoomOnes")
        expectEqual(members([guest("1")], local: ["cam-1", "zoom:1", "capture:cam-1", "capture:cam-1"]) ?? [],
                    ["zoom:1", "capture:cam-1"], "\(t).Build_IgnoresANonCanonicalLocalIdAndDeduplicatesACanonicalOne")

        let s4 = "\(t).BuildCarriesStylingAndRejectsNonFiniteOrOutOfRangeValues"
        var styled = TilesSettings()
        styled.borderShape = "rounded"; styled.borderColor = "#123456"; styled.borderThickness = 4
        styled.cornerRadius = 22; styled.glowColor = "#abcdef"; styled.glowSize = 12; styled.glowIntensity = 75; styled.glowSoftness = 40
        var style = tilesStyle(styled)
        expectEqual(style["borderShape"] as? String, "rounded", "\(s4): shape")
        expectEqual(style["borderColor"] as? String, "#123456", "\(s4): border colour")
        expectEqual(style["glowColor"] as? String, "#ABCDEF", "\(s4): glow colour normalized")
        expectEqual(style["borderThickness"] as? Double, 4, "\(s4): thickness")
        expectEqual(style["cornerRadius"] as? Double, 22, "\(s4): radius")
        expectEqual(style["glowSize"] as? Double, 12, "\(s4): glow")
        expectEqual(style["glowIntensity"] as? Double, 75, "\(s4): intensity")
        expectEqual(style["glowSoftness"] as? Double, 40, "\(s4): softness")
        styled.borderThickness = .nan; styled.cornerRadius = -10; styled.glowSize = 1000
        styled.glowIntensity = .infinity; styled.glowSoftness = -1
        style = tilesStyle(styled)
        expectEqual(style["borderThickness"] as? Double, 0, "\(s4): NaN thickness -> 0")
        expectEqual(style["cornerRadius"] as? Double, 0, "\(s4): radius -10 -> 0")
        expectEqual(style["glowSize"] as? Double, 64, "\(s4): glow 1000 -> 64")
        expectEqual(style["glowIntensity"] as? Double, 100, "\(s4): intensity +inf -> 100")
        expectEqual(style["glowSoftness"] as? Double, 0, "\(s4): softness -1 -> 0")
        styled.cornerRadius = .nan
        expectEqual(tilesStyle(styled)["cornerRadius"] as? Double, 16, "\(s4): NaN radius falls back to 16")
        // D3: the Mac never sends nameEachTile, whatever is saved.
        var named = TilesSettings(); named.nameEachTile = true
        expectEqual(tilesStyle(named)["nameEachTile"] as? Bool, false, "D3: nameEachTile is always false on Mac")
        expectEqual(named.nameEachTile, true, "D3: the saved value is untouched")
        let empty = TilesLayerPayload.build(sceneId: "s", settings: TilesSettings(), roster: [], localVideoSourceIds: [],
                                            routedSourceIds: [], identityRoster: nil, meetingId: nil)
        expect((empty?["overrides"] as? JSONObject)?.isEmpty == true, "SerializeTilesLayer: overrides is {} when empty, never absent")
    }

    private static func guestsRoster(_ ids: [String]) -> [RosterParticipant] { ids.map { guest($0) } }

    private static func tilesStyle(_ settings: TilesSettings) -> JSONObject {
        TilesLayerPayload.build(sceneId: "s", settings: settings, roster: [guest("1")], localVideoSourceIds: [],
                                routedSourceIds: [], identityRoster: nil, meetingId: nil)?["style"] as? JSONObject ?? [:]
    }

    private static func slot(_ id: Int, _ kind: String, _ sourceId: String, inShow: Bool = true, name: String = "") -> ShowInputSlot {
        var s = ShowInputSlot(id: id)
        s.kind = kind; s.sourceId = sourceId; s.inShow = inShow
        s.name = name.isEmpty ? String(format: "Input %02d", id) : name
        return s
    }

    static func testTilesLocalSourcePolicyPortsTheWindowsFacts() {
        let t = "TilesLocalSourcePolicyTests"
        let offShow = TilesLocalSourcePolicy.localVideoSources(
            [slot(1, "capture", "cam-1", inShow: false), slot(2, "capture", "cam-2")], devices: [])
        expectEqual(offShow.map(\.id), ["capture:cam-2"], "\(t).LocalVideoSources_SkipsASlotThatIsNotInTheShow")
        let media = TilesLocalSourcePolicy.localVideoSources([slot(1, "media", "clip-7"), slot(2, "capture", "cam-1")], devices: [])
        expectEqual(media.map(\.id), ["capture:cam-1"], "\(t).LocalVideoSources_DoesNotOfferAMediaSlotYet")
        let cam = CaptureDeviceRow(id: "cam-1", name: "Cam", kind: "video", vendor: "", connectionState: "connected",
                                   signalPresent: true, warning: "")
        let twice = TilesLocalSourcePolicy.localVideoSources([slot(2, "capture", "cam-1"), slot(1, "capture", "cam-1")], devices: [cam])
        expectEqual(twice, [TilesLocalSource(id: "capture:cam-1", name: "Cam", group: "Camera")],
                    "\(t).LocalVideoSources_DeduplicatesTheSameDeviceBoundTwiceAndKeepsTheFirstSlot")
        let ordered = TilesLocalSourcePolicy.localVideoSources(
            [slot(4, "capture", "screen:1"), slot(1, "zoom", "16778240"), slot(2, "capture", "cam-1"),
             slot(5, "unassigned", ""), slot(3, "capture", "browser:1")], devices: [cam])
        expectEqual(ordered, [TilesLocalSource(id: "capture:cam-1", name: "Cam", group: "Camera"),
                              TilesLocalSource(id: "capture:browser:1", name: "Input 03", group: "Browser"),
                              TilesLocalSource(id: "capture:screen:1", name: "Input 04", group: "Screen")],
                    "\(t).LocalVideoSources_ListsInShowLocalSlotsInSlotOrderWithTheirPickerGroups")
        let routed = TilesLocalSourcePolicy.routedSourceIds(
            [slot(1, "zoom", "16778240"), slot(2, "capture", "cam-1"), slot(3, "capture", "screen:1", inShow: false),
             slot(4, "media", "clip-7"), slot(5, "unassigned", "")])
        let vendors = ["srt": "SRT", "rtmp": "RTMP", "ndi": "NDI", "blackmagic": "Camera", "aja": "Camera", "": "Camera"]
        for (vendor, group) in vendors {
            let device = CaptureDeviceRow(id: "dev", name: "Dev", kind: "video", vendor: vendor, connectionState: "connected",
                                          signalPresent: true, warning: "")
            expectEqual(TilesLocalSourcePolicy.localVideoSources([slot(1, "capture", "dev")], devices: [device]).first?.group,
                        group, "\(t).GroupFor_MatchesTheShowInputPickerGroups (\(vendor.isEmpty ? "webcam" : vendor))")
        }
        var renamed = slot(6, "capture", "cam-unplugged")
        renamed.name = "Operator's name"
        expectEqual(TilesLocalSourcePolicy.localVideoSources([renamed], devices: []).first?.name, "Input 06",
                    "\(t): an unenumerated device falls back to the slot LABEL (Windows SlotLabel), not its renamed name")
        expectEqual(routed, ["zoom:16778240", "capture:cam-1", "media:clip-7"], "\(t).RoutedSourceIds_AreTheInShowSlotIdsZoomAndLocalAlike")
        expect(TilesLocalSourcePolicy.isLocalSourceId("capture:x") && TilesLocalSourcePolicy.isLocalSourceId("media:x")
               && !TilesLocalSourcePolicy.isLocalSourceId("zoom:1") && !TilesLocalSourcePolicy.isLocalSourceId("")
               && !TilesLocalSourcePolicy.isLocalSourceId(nil) && !TilesLocalSourcePolicy.isLocalSourceId("Capture:x"),
               "\(t).IsLocalSourceId_RecognisesOnlyCanonicalCaptureAndMediaIds")
    }
}
