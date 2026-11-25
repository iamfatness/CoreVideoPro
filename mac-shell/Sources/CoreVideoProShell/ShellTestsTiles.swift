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
            // Task 8: the wall in scene sync, the starter and the layer guards.
            ("tiles/sync-both-buses-empty-routes", testATilesSceneSendsItsWallOnBothBusesWithEmptyRoutes),
            ("tiles/take-same-wall-one-batch", testATakeOfTheSameWallKeepsItsLayerIdAndSendsOneBatch),
            ("tiles/starter-preview-only", testTheTilesStarterIsANamedPreviewOnlyScene),
            ("tiles/layer-edits-refused", testLayerEditsOnATilesSceneAreRefusedAndSendNoRoutes),
            ("tiles/respawn-resends-wall", testACoreRespawnResendsTheWallOnBothBuses),
            ("tiles/non-tiles-scene-no-key", testANonTilesSceneCarriesNoTilesKey),
            ("tiles/roster-change-resends-wall", testARosterChangeResendsAWallOnABus),
            ("tiles/no-layers-or-clips-on-a-wall", testAWallOnAPresetLayoutGetsNoLayersAndNoVoltClip),
            // Task 10: the source set with Tiles tiers, from Windows-generated fixtures.
            ("tiles/spine-fixtures", testEverySpineFixtureMatchesTheSwiftSourceSet),
            ("tiles/spine-windows-facts", testTheSourceSetPortsTheWindowsSpineFacts),
            // Task 11: sticky audio latch, spine additions, re-push triggers.
            ("tiles/latch-windows-facts", testTheTilesAudioLatchPortsTheWindowsFacts),
            ("tiles/cue-repushes-spine", testCueingAWallRepushesTheSpineWithItsMembers),
            ("tiles/camera-off-keeps-audio", testACameraOffWallMemberKeepsAudioWhileTheWallIsOnABus),
            ("tiles/capture-off-forgets-latch", testCaptureOffForgetsTheLatch),
            ("tiles/respawn-resends-walls-spine-latch", testACoreRespawnResendsBothWallsAndTheSpineAndKeepsTheLatch),
            ("tiles/bus-caption", testBusCaptionCarriesTheCoresNotice),
            ("tiles/iso-arming-repushes-spine", testArmingIsoRepushesTheSpine),
            ("tiles/mac-multiview-kind-label", testAnOverBudgetMacMultiviewTileCarriesTheLabel),
            // Task 14: membership actions on the Preview wall.
            ("tiles/member-choices-windows-facts", testTilesMemberChoicesPortTheWindowsFacts),
            ("tiles/workspace-membership-rules", testTilesWorkspaceMembershipRules),
            ("tiles/actions-refuse-without-a-preview-wall", testEveryTilesActionRefusesWhenPreviewIsNotATilesScene),
            ("tiles/assign-slot-windows-facts", testTilesSlotAssignmentPortsTheWindowsFacts),
            ("tiles/assign-moves-not-duplicates", testAssigningAGuestMovesThemNotDuplicatesThem),
            ("tiles/include-manual-first-empty-slot", testIncludeInManualModeTakesTheFirstEmptySlot),
            ("tiles/membership-summary", testTilesMembershipSummaryReadsLikeWindows),
            ("tiles/edits-sync-and-save", testEveryTilesEditIsSentAndSaved),
            ("tiles/identity-roster-is-full", testACameraOffGuestsSavedSlotIsNotStale),
            ("tiles/move-and-include-paths", testTilesMoveAndIncludeCoverEveryPath),
            // Task 15: Edit Tiles and the two-column workspace.
            ("tiles/edit-tiles-entry", testEditTilesCuesPreviewOpensTheInspectorAndNeverTouchesProgram),
            ("tiles/scenes-tab-layout-rules", testTheScenesTabStopsScrollingAndWidensOnlyWhileEditingTiles),
            // Task 13: identity and roster-aware membership.
            ("tiles/identity-windows-facts", testTilesIdentityPortsTheWindowsFacts),
            ("tiles/membership-windows-facts", testTilesMembershipPortsTheWindowsFacts),
            ("tiles/rejoin-and-duplicate-name", testARejoinedGuestKeepsTheirSlotAndADuplicateNameSeatsNobody),
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

    // ── Task 8 ───────────────────────────────────────────────────────────────

    /// takeModel (Program s2, Preview s1) plus two walls: t-pgm on Program,
    /// t-pvw on Preview, a roster of 7 and 5 (camera on), a logo bug, and the
    /// harness timer for the scene-sync debounce.
    @MainActor
    private static func tilesSyncModel(_ core: FakeTakeCore, programWall: Bool = true) -> (AppModel, ManualTimers) {
        let model = takeModel(core)
        model.scenes.append(SceneDef(id: "t-pgm", name: "Wall A", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        model.scenes.append(SceneDef(id: "t-pvw", name: "Wall B", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        if programWall { model.programSceneId = "t-pgm" }
        model.previewSceneId = "t-pvw"
        model.roster = [guest("7"), guest("5")]
        model.logoBug = MediaAsset(id: "bug", name: "Bug", kind: "still", relativePath: "still/bug.png",
                                   filePath: "/b/still/bug.png", naturalWidth: 200, naturalHeight: 100)
        let timers = ManualTimers()
        model.timerSleepForTesting = { await timers.sleep($0) }
        return (model, timers)
    }

    /// Fires the parked debounce until a body carrying a scene graph lands.
    @MainActor
    private static func flushSceneSync(_ core: FakeTakeCore, _ timers: ManualTimers, after count: Int = 0) -> [JSONObject] {
        spin(until: {
            timers.fireAll()
            return core.bodies.dropFirst(count).contains { body in
                (body["commands"] as? [JSONObject] ?? []).contains { $0["type"] as? String == "load-scene-graph" }
            }
        }, limit: 3)
        return core.bodies.dropFirst(count).last { body in
            (body["commands"] as? [JSONObject] ?? []).contains { $0["type"] as? String == "load-scene-graph" }
        }?["commands"] as? [JSONObject] ?? []
    }

    private static func command(_ commands: [JSONObject], _ type: String) -> JSONObject {
        commands.first { $0["type"] as? String == type } ?? [:]
    }

    @MainActor static func testATilesSceneSendsItsWallOnBothBusesWithEmptyRoutes() {
        let w = "Tiles scene sync (spec 3.2; Windows BuildProductionSyncContext)"
        let core = FakeTakeCore()
        let (model, timers) = tilesSyncModel(core)
        model.syncScenes()
        // The debounce runs on the harness timer, never on wall clock: it parks,
        // and nothing is sent until the harness fires it.
        spin(until: { timers.pending > 0 }, limit: 1)
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))   // past the 120 ms a wall-clock sleep would take
        let early = core.bodies.contains { body in
            (body["commands"] as? [JSONObject] ?? []).contains { $0["type"] as? String == "load-scene-graph" } }
        expect(!early, "\(w): no scene sync is sent before the harness fires the debounce (timer seam, not wall clock)")
        let commands = flushSceneSync(core, timers)
        let program = command(commands, "load-scene-graph"), preview = command(commands, "set-preview-scene")
        expectEqual((program["routes"] as? [JSONObject])?.count, 0, "\(w): Program routes are []")
        expectEqual((program["tiles"] as? JSONObject)?["layerId"] as? String, "tiles:t-pgm", "\(w): Program wall")
        expectEqual((preview["routes"] as? [JSONObject])?.count, 0, "\(w): Preview routes are []")
        expectEqual((preview["tiles"] as? JSONObject)?["members"] as? [String], ["zoom:7", "zoom:5"], "\(w): Preview members")
        // A Tiles scene that somehow carries layers (an older file, a future
        // editor) still sends no routes: the wall owns the scene.
        model.scenes[model.scenes.firstIndex { $0.id == "t-pvw" }!].layers =
            [SceneLayer(id: "t-pvw-layer-0", slotId: nil, x: 0, y: 0, width: 1, height: 1)]
        expectEqual(model.buildRoutes(for: "t-pvw").count, 0, "\(w): layers on a wall send no routes, no logo bug")
        // A scene is a Tiles scene by its settings (Windows Scene.DynamicGallery),
        // not its layout word: a wall on a preset layout still sends no routes.
        model.scenes.append(SceneDef(id: "t-preset", name: "Wall C", layout: "single", tiles: TilesSettings()))
        expectEqual(model.buildRoutes(for: "t-preset").count, 0, "\(w): a wall on a preset layout sends no routes, no logo bug")
        let anyBug = core.bodies.flatMap { $0["commands"] as? [JSONObject] ?? [] }
            .flatMap { $0["routes"] as? [JSONObject] ?? [] }
            .contains { ($0["routeId"] as? String ?? "").hasSuffix("-bug") }
        expect(!anyBug, "\(w): the logo bug never rides a Tiles scene (D5)")
    }

    @MainActor static func testATakeOfTheSameWallKeepsItsLayerIdAndSendsOneBatch() {
        let w = "Take of a wall (spec 3.2)"
        let core = FakeTakeCore()
        let (model, _) = tilesSyncModel(core, programWall: false)   // Program s2, Preview t-pvw
        model.logoBug = nil
        let box = runTake(model)
        expectEqual(box.result, .success, "\(w): Take succeeds")
        expectEqual(core.bodies.count, 1, "\(w): one media-core-sync body")
        expectEqual(core.bodies.isEmpty ? [] : core.types(), ["begin-take-transition", "load-scene-graph", "set-preview-scene"],
                    "\(w): transition, Program graph, Preview scene")
        let commands = core.bodies.first?["commands"] as? [JSONObject] ?? []
        expectEqual((command(commands, "load-scene-graph")["tiles"] as? JSONObject)?["layerId"] as? String, "tiles:t-pvw",
                    "\(w): the wall keeps its layer id on Program, so the core cuts to it")
        expect(command(commands, "set-preview-scene")["tiles"] == nil, "\(w): the outgoing non-Tiles scene carries no wall")
    }

    @MainActor static func testTheTilesStarterIsANamedPreviewOnlyScene() {
        let w = "StarterSceneFactory Tiles starter / MakeUniqueSceneName"
        let model = sceneEditModel()
        let program = model.programSceneId
        model.newTilesScene()
        model.newTilesScene()
        let walls = model.scenes.filter { $0.layout == AppModel.tilesLayout }
        expectEqual(walls.map(\.name), ["CoreVideo Tiles", "CoreVideo Tiles 2"], "\(w): unique names")
        expect(walls.allSatisfy { $0.tiles == TilesSettings() && $0.layers.isEmpty }, "\(w): default settings, no layers")
        expectEqual(Set(walls.map(\.id)).count, 2, "\(w): distinct ids even within one second")
        expectEqual(model.programSceneId, program, "\(w): Program unchanged")
        expectEqual(model.previewSceneId, walls.last?.id ?? "", "\(w): Preview is the newest")
        expectEqual(AppModel.uniqueSceneName("corevideo tiles", existing: ["CoreVideo Tiles", "COREVIDEO TILES 2"]),
                    "corevideo tiles 3", "\(w): case-insensitive")
    }

    @MainActor static func testLayerEditsOnATilesSceneAreRefusedAndSendNoRoutes() {
        let w = "Tiles layer guard (D5)"
        let core = FakeTakeCore()
        let (model, timers) = tilesSyncModel(core)
        model.addLayer(to: "t-pvw")
        expectEqual(model.studioCommandStatus, AppModel.tilesLayersRefusal, "\(w): Add layer refused")
        model.studioCommandStatus = ""
        model.toggleStudioLayoutEditing()
        expectEqual(model.studioCommandStatus, AppModel.tilesLayersRefusal, "\(w): Edit layout refused")
        expect(!model.studioLayoutEditing, "\(w): the editor does not open")
        model.studioCommandStatus = ""
        expectEqual(model.addOverlayLayer(placement: "lower-left", source: .activeSpeaker), AppModel.tilesLayersRefusal,
                    "\(w): Add overlay refused")
        model.ensureLayers(for: "t-pvw")
        expect(model.scenes.first { $0.id == "t-pvw" }?.layers.isEmpty ?? false, "\(w): the wall has no layers")
        model.syncScenes()
        let commands = flushSceneSync(core, timers)
        expectEqual((command(commands, "set-preview-scene")["routes"] as? [JSONObject])?.count, 0, "\(w): routes still []")
    }

    @MainActor static func testACoreRespawnResendsTheWallOnBothBuses() {
        let w = "Global constraint: re-apply on every core generation"
        let core = FakeTakeCore()
        let (model, timers) = tilesSyncModel(core)
        let before = core.bodies.count
        model.onConnected()
        let commands = flushSceneSync(core, timers, after: before)
        expect(command(commands, "load-scene-graph")["tiles"] != nil, "\(w): Program wall resent")
        expect(command(commands, "set-preview-scene")["tiles"] != nil, "\(w): Preview wall resent")
    }

    @MainActor static func testANonTilesSceneCarriesNoTilesKey() {
        let w = "non-Tiles scene"
        let core = FakeTakeCore()
        let model = takeModel(core)
        let timers = ManualTimers()
        model.timerSleepForTesting = { await timers.sleep($0) }
        model.syncScenes()
        let commands = flushSceneSync(core, timers)
        expect(!commands.isEmpty, "\(w): a sync landed")
        expect(command(commands, "load-scene-graph")["tiles"] == nil, "\(w): Program graph has no tiles key")
        expect(command(commands, "set-preview-scene")["tiles"] == nil, "\(w): Preview has no tiles key")
    }

    @MainActor static func testARosterChangeResendsAWallOnABus() {
        let w = "Task 8 review: a wall's members follow the roster"
        let core = FakeTakeCore()
        let (model, timers) = tilesSyncModel(core)
        // First the meeting roster arrives (the slot law seats 7 and 5 and
        // syncs scenes itself)...
        model.applyZoomForTesting(["meetingState": "in_meeting", "participants": [
            ["userId": "7", "displayName": "Guest 7", "videoOn": true],
            ["userId": "5", "displayName": "Guest 5", "videoOn": true],
        ]])
        _ = flushSceneSync(core, timers)
        let sent = core.bodies.count
        // ...then only a camera turns off: no slot changes, so only the wall
        // re-send rule can carry the new membership to the core.
        model.applyZoomForTesting(["meetingState": "in_meeting", "participants": [
            ["userId": "7", "displayName": "Guest 7", "videoOn": true],
            ["userId": "5", "displayName": "Guest 5", "videoOn": false],
        ]])
        let commands = flushSceneSync(core, timers, after: sent)
        expectEqual((command(commands, "set-preview-scene")["tiles"] as? JSONObject)?["members"] as? [String],
                    ["zoom:7"], "\(w): a camera turning off re-sends the Preview wall")
        expectEqual((command(commands, "load-scene-graph")["tiles"] as? JSONObject)?["members"] as? [String],
                    ["zoom:7"], "\(w): and the Program wall")
    }

    @MainActor static func testAWallOnAPresetLayoutGetsNoLayersAndNoVoltClip() {
        let w = "Task 8 review: a wall is a wall by its settings"
        let model = sceneEditModel()
        model.scenes.append(SceneDef(id: "t-preset", name: "Wall", layout: "single", tiles: TilesSettings()))
        model.ensureLayers(for: "t-preset")
        expect(model.scenes.first { $0.id == "t-preset" }?.layers.isEmpty ?? false, "\(w): ensureLayers seeds nothing on a wall")
        expect(model.voltClipLayerId(scene: "t-preset", route: "") == nil, "\(w): a wall offers no Volt clip route")
        model.newScene(layout: "single")
        model.newScene(layout: "single")
        expectEqual(Set(model.scenes.map(\.id)).count, model.scenes.count, "\(w): new scenes never share an id")
    }

    // ── Task 10 ──────────────────────────────────────────────────────────────

    static func loadSpineFixtures() -> [[String: Any]]? {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        guard let data = try? Data(contentsOf: root.appendingPathComponent("contracts/tiles-spine.fixtures.json")),
              let doc = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return doc["cases"] as? [[String: Any]]
    }

    static func spineInput(_ json: [String: Any]) -> (ZoomSourceBudget.Input, [String: String]) {
        var input = ZoomSourceBudget.Input()
        let people = json["participants"] as? [[String: Any]] ?? []
        input.participants = people.map { ($0["id"] as? String ?? "", $0["videoOn"] as? Bool ?? false) }
        input.programRoutes = json["programRoutes"] as? [JSONObject] ?? []
        input.previewRoutes = json["previewRoutes"] as? [JSONObject] ?? []
        input.programTiles = json["programTiles"] as? JSONObject
        input.previewTiles = json["previewTiles"] as? JSONObject
        input.stickyAudio = json["sticky"] as? [String] ?? []
        input.iso = json["iso"] as? [String] ?? []
        input.multiview = json["multiview"] as? [JSONObject] ?? []
        let names = Dictionary(people.map { ($0["id"] as? String ?? "", $0["name"] as? String ?? "") },
                               uniquingKeysWith: { first, _ in first })
        return (input, names)
    }

    static func testEverySpineFixtureMatchesTheSwiftSourceSet() {
        let w = "TilesSpineFixtureTests.EveryCaseMatchesTheWindowsBuilder (Swift twin)"
        guard let cases = loadSpineFixtures() else {
            expect(false, "\(w): contracts/tiles-spine.fixtures.json must be readable"); return
        }
        var checked = 0
        for fixture in cases {
            let name = fixture["name"] as? String ?? "?"
            guard let inputJson = fixture["input"] as? [String: Any], let expected = fixture["expected"] as? [String: Any] else {
                expect(false, "\(w): \(name) has input and a generated expected"); continue
            }
            let (input, names) = spineInput(inputJson)
            let parts = ZoomSourceBudget.spineParts(input, names: names)
            var actual: JSONObject = [
                "subscriptions": parts.subscriptions, "sourceParticipantIds": parts.sourceParticipantIds,
                "videoSubscriptionShortfall": parts.shortfall, "warnings": parts.warnings,
            ]
            if inputJson["multiview"] is [Any] {
                actual["multiview"] = ["programNotice": parts.programNotice, "previewNotice": parts.previewNotice,
                                       "labels": parts.multiviewSources.map { $0["label"] as? String ?? "" }]
            }
            if let difference = tilesJsonDifference(expected, tilesWire(actual), name) { expect(false, "\(w): \(difference)") }
            checked += 1
        }
        expectEqual(checked, cases.count, "\(w): every case checked")
        expect(checked >= 7, "\(w): at least 7 cases (a vacuous loop fails)")
    }

    private static func video(_ s: [ZoomSourceBudget.Source], max: Int = 10) -> [String] {
        ZoomSourceBudget.decideVideo(s, max: max).subscribed.map { "\($0.participantId):\($0.purpose)" }
    }

    private static func fixedRoute(_ pid: String?, mode: String = "fixed") -> JSONObject {
        var r: JSONObject = ["mode": mode]
        if let pid { r["participantId"] = pid }
        return r
    }

    private static func wall(_ pids: [String]) -> [JSONObject] {
        pids.enumerated().map { ["kind": "zoom", "participantId": $0.element, "slot": $0.offset] }
    }

    static func testTheSourceSetPortsTheWindowsSpineFacts() {
        let t = "ZoomMediaSpinePayloadBuilderTests"
        typealias I = ZoomSourceBudget.Input
        func people(_ ids: [String], off: Set<String> = []) -> [(id: String, videoOn: Bool)] { ids.map { ($0, !off.contains($0)) } }

        var a = I(); a.participants = people((1...10).map { "p\($0)" })
        a.programRoutes = [fixedRoute("p9")]; a.previewRoutes = [fixedRoute("p10")]; a.multiview = wall(["p1", "p2", "p3"])
        expectEqual(video(ZoomSourceBudget.resolve(a), max: 3), ["p9:program", "p10:preview", "p1:multiview"],
                    "\(t).ProgramAndPreviewRoutesOutrankTheWallAndTheActiveSpeakerDoesNotJumpTheQueue")

        let host = "16778240", comms = "16783360", offWall = "16788480", offB = "16789504", offC = "16790528"
        let pgm = "16791552", pvw = "16792576", alex = "50332672"
        var b = I()
        b.participants = people([host, comms, offWall, offB, offC, pgm, pvw, "16793600", "16794624", "16795648", "16796672", alex],
                                off: [comms, offB, offC])
        b.programRoutes = [fixedRoute(pgm)]; b.previewRoutes = [fixedRoute(pvw)]
        b.multiview = [pgm, pvw, alex, "16793600", "16794624", "16795648", "16796672"].enumerated()
            .map { ["kind": "zoom", "participantId": $0.element, "slot": $0.offset] }
        let live = ZoomSourceBudget.resolve(b)
        expectEqual(ZoomSourceBudget.decideVideo(live, max: 10).subscribed.map(\.participantId),
                    [pgm, pvw, alex, "16793600", "16794624", "16795648", "16796672"],
                    "\(t).LiveCase478_AWallGuestWhoJoinedLateIsSubscribedAndNothingUnroutedIs")
        expect(!ZoomSourceBudget.audioIds(live).contains(host) && !ZoomSourceBudget.audioIds(live).contains(offWall),
               "\(t).LiveCase478_AudioIsSubscribedForTheSameSourcesAndNobodyElse")

        var c = I(); c.participants = people(["w1", "t1", "t2", "bg", "pv", "x"])
        c.programTiles = ["members": ["zoom:t1", "zoom:t2", "capture:cam-1"], "style": ["backgroundSourceId": "zoom:bg"]]
        c.previewTiles = ["members": ["zoom:t2", "zoom:pv"]]; c.multiview = wall(["w1"])
        expectEqual(video(ZoomSourceBudget.resolve(c)),
                    ["t1:program-tiles", "t2:program-tiles", "bg:program-tiles", "pv:preview-tiles", "w1:multiview"],
                    "\(t).TilesMembersOfBothBusesAreSubscribedBecauseATilesSceneSendsNoRoutes (background read from the wire's style node)")

        var d = I(); d.participants = people(["t1", "t2"])
        d.programTiles = ["members": ["zoom:t1", "zoom:t2"]]; d.previewRoutes = [fixedRoute("t1")]
        expectEqual(video(ZoomSourceBudget.resolve(d)), ["t1:preview", "t2:program-tiles"],
                    "\(t).AFixedBusRouteSetsTheFullResolutionPurposeWhateverTierListsTheGuestFirst")

        let members = (0..<10).map { "t\($0)" }
        func cue(_ cued: Bool) -> I {
            var e = I(); e.participants = people(members + ["cued"])
            e.programTiles = ["members": members.map { "zoom:" + $0 }]
            e.previewRoutes = cued ? [fixedRoute("cued")] : []; e.multiview = wall(["t0"])
            return e
        }
        let after = ZoomSourceBudget.spineParts(cue(true), names: ["cued": "Cued guest"])
        expectEqual(video(ZoomSourceBudget.resolve(cue(true))), video(ZoomSourceBudget.resolve(cue(false))),
                    "\(t).AtTheCapCueingPreviewNeverTakesVideoFromAProgramGalleryMember: Program unchanged")
        expectEqual(after.previewNotice, "no video: Cued guest (subscription limit 10)", "\(t).AtTheCap…: the PVW cell names them")
        expectEqual(after.programNotice, "", "\(t).AtTheCap…: nothing on Program")

        var f = I(); f.participants = people(["latched", "walled"], off: ["latched"])
        f.stickyAudio = ["latched", "walled"]; f.multiview = wall(["walled"])
        let sticky = ZoomSourceBudget.resolve(f)
        expectEqual(video(sticky), ["walled:multiview"], "\(t).AStickyLatchedTilesMemberIsAnAudioOnlySourceUnlessAnotherTierListsThem")
        expectEqual(ZoomSourceBudget.audioIds(sticky), ["walled", "latched"], "\(t).AStickyLatched…: audio")

        var g = I(); g.participants = people(["off", "on"], off: ["off"]); g.multiview = wall(["off", "on"])
        let camOff = ZoomSourceBudget.resolve(g)
        expectEqual(video(camOff), ["on:multiview"], "\(t).ACameraOffWallGuestGetsAudioButNoVideo")
        expectEqual(ZoomSourceBudget.audioIds(camOff), ["off", "on"], "\(t).ACameraOffWallGuestGetsAudioButNoVideo: audio")

        var h = I(); h.participants = people(["w1", "w2", "x"])
        h.programRoutes = [fixedRoute(nil, mode: "active-speaker")]; h.multiview = wall(["w1", "w2"])
        let follow = ZoomSourceBudget.resolve(h)
        expectEqual(video(follow), ["w1:multiview", "w2:multiview"], "\(t).AFollowSpeakerRouteGrantsNoFeedAndNoPurpose")
        expect(!ZoomSourceBudget.audioIds(follow).contains("x"), "\(t).AFollowSpeaker…: a talking non-source gets nothing")

        // The Mac multiview names a Zoom tile `participant-video`: it counts like Windows' `zoom`.
        var m = I(); m.participants = people(["a"]); m.multiview = [["kind": "participant-video", "participantId": "a", "slot": 0]]
        expectEqual(video(ZoomSourceBudget.resolve(m)), ["a:multiview"], "Mac multiview kind maps to the Windows zoom kind")
    }

    // ── Task 11 ──────────────────────────────────────────────────────────────

    private static func wallObject(_ members: [String]) -> JSONObject { ["members": members.map { "zoom:" + $0 }] }

    static func testTheTilesAudioLatchPortsTheWindowsFacts() {
        let t = "TilesAudioSourceLatchTests"
        let everyone: Set<String> = ["a", "b", "comms", "c"]
        var latch = TilesAudioSourceLatch()
        expectEqual(latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["a", "b"]),
                                  previewSceneId: nil, previewTiles: nil, present: everyone), ["a", "b"], "\(t).TheLatchClears…: latched")
        expectEqual(latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["b"]),
                                  previewSceneId: nil, previewTiles: nil, present: everyone), ["a", "b"], "\(t).TheLatchClears…: a dropped member stays")
        expectEqual(latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject([]),
                                  previewSceneId: nil, previewTiles: nil, present: ["a"]), ["a"], "\(t).TheLatchClears…: leaving the meeting ends it")
        expectEqual(latch.observe(inMeeting: true, programSceneId: "other", programTiles: wallObject(["c"]),
                                  previewSceneId: nil, previewTiles: nil, present: everyone), ["c"], "\(t).TheLatchClears…: a scene off both buses is forgotten")
        expectEqual(latch.observe(inMeeting: true, programSceneId: "routes", programTiles: nil,
                                  previewSceneId: nil, previewTiles: nil, present: everyone), [],
                    "\(t).TheLatchClearsWhenTheSceneLeavesBothBusesOrTheGuestLeavesTheMeeting")

        latch = TilesAudioSourceLatch()
        let mxy: Set<String> = ["m", "x", "y"]
        _ = latch.observe(inMeeting: true, programSceneId: "A", programTiles: wallObject(["x"]), previewSceneId: "B",
                          previewTiles: wallObject(["m", "y"]), present: mxy)
        expect(latch.observe(inMeeting: true, programSceneId: "A", programTiles: wallObject(["x"]), previewSceneId: "B",
                             previewTiles: wallObject(["y"]), present: mxy).contains("m"), "\(t).ATakeThatSwaps…: m latched on Preview")
        expect(latch.observe(inMeeting: true, programSceneId: "B", programTiles: wallObject(["y"]), previewSceneId: "A",
                             previewTiles: wallObject(["x"]), present: mxy).contains("m"),
               "\(t).ATakeThatSwapsTheBusesKeepsACameraOffPanelistAudibleAsTheirGalleryGoesToAir")

        latch = TilesAudioSourceLatch()
        _ = latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["a"]), previewSceneId: nil, previewTiles: nil, present: everyone)
        expectEqual(latch.observe(inMeeting: false, programSceneId: "gallery", programTiles: wallObject([]), previewSceneId: nil, previewTiles: nil, present: everyone),
                    [], "\(t).LeavingTheMeetingOrEngineOffForgetsEverySoAReusedIdIsNotAudible: known not-in-meeting")
        expectEqual(latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject([]), previewSceneId: nil, previewTiles: nil, present: everyone),
                    [], "\(t).LeavingTheMeeting…: stays forgotten")
        _ = latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["b"]), previewSceneId: nil, previewTiles: nil, present: everyone)
        latch.clear()
        expectEqual(latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject([]), previewSceneId: nil, previewTiles: nil, present: everyone),
                    [], "\(t).LeavingTheMeeting…: Engine off (clear) forgets")

        latch = TilesAudioSourceLatch()
        let sticky = latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["a"]), previewSceneId: nil, previewTiles: nil, present: everyone)
        expect(!sticky.contains("comms"), "\(t).ANeverOnCameraParticipantIsNeverLatchedSoNeverAudible")
        var input = ZoomSourceBudget.Input(); input.participants = [("a", true), ("comms", false)]
        input.programTiles = wallObject(["a"]); input.stickyAudio = sticky
        expect(!ZoomSourceBudget.audioIds(ZoomSourceBudget.resolve(input)).contains("comms"), "\(t).ANeverOnCamera…: not audible")

        latch = TilesAudioSourceLatch()
        _ = latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["a", "b"]), previewSceneId: nil, previewTiles: nil, present: everyone)
        expect(!latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["b"]), previewSceneId: nil, previewTiles: nil,
                              present: ["b", "comms", "c"]).contains("a"),
               "\(t).AVideoOnlyPresentSetForgetsACameraOffPanelist_ProductionMustNotObserveThatWay (the trap)")

        latch = TilesAudioSourceLatch()
        _ = latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["a", "b"]), previewSceneId: nil, previewTiles: nil, present: everyone)
        expectEqual(latch.observe(inMeeting: nil, programSceneId: "gallery", programTiles: wallObject(["b"]), previewSceneId: nil, previewTiles: nil, present: everyone),
                    ["a", "b"], "\(t).AnUnknownMeetingStateTickNeverWipesTheLatch: unknown keeps")
        expectEqual(latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: wallObject(["b"]), previewSceneId: nil, previewTiles: nil, present: everyone),
                    ["a", "b"], "\(t).AnUnknownMeetingState…: in meeting keeps")
        expectEqual(latch.observe(inMeeting: false, programSceneId: "gallery", programTiles: wallObject(["b"]), previewSceneId: nil, previewTiles: nil, present: everyone),
                    [], "\(t).AnUnknownMeetingStateTickNeverWipesTheLatch: known false clears")

        latch = TilesAudioSourceLatch()
        expectEqual(latch.observe(inMeeting: true, programSceneId: "pgm", programTiles: wallObject(["a"]), previewSceneId: "pvw",
                                  previewTiles: wallObject(["b"]), present: everyone), ["a", "b"], "\(t).EachSceneOnABusKeepsItsOwnLatch")
        expectEqual(latch.observe(inMeeting: true, programSceneId: "pgm", programTiles: wallObject([]), previewSceneId: "pvw2",
                                  previewTiles: wallObject(["c"]), present: everyone), ["a", "c"], "\(t).EachSceneOnABusKeepsItsOwnLatch: second tick")

        // APanelistOnTheProgramWallWhoTurnsTheirCameraOffLosesVideoButKeepsAudio
        latch = TilesAudioSourceLatch()
        func spine(aOn: Bool, wall: [String]) -> ZoomSourceBudget.SpineParts {
            var i = ZoomSourceBudget.Input(); i.participants = [("a", aOn), ("b", true)]; i.programTiles = wallObject(wall)
            i.stickyAudio = latch.observe(inMeeting: true, programSceneId: "gallery", programTiles: i.programTiles,
                                          previewSceneId: nil, previewTiles: nil, present: everyone)
            return ZoomSourceBudget.spineParts(i, names: [:])
        }
        func ids(_ p: ZoomSourceBudget.SpineParts, _ kind: String) -> [String] {
            p.subscriptions.filter { $0["kind"] as? String == kind }.compactMap { $0["participantId"] as? String }
        }
        let before = spine(aOn: true, wall: ["a", "b"]), after = spine(aOn: false, wall: ["b"])
        expect(ids(before, "participant-video").contains("a") && ids(before, "participant-audio").contains("a"), "\(t).APanelist…: before")
        expect(!ids(after, "participant-video").contains("a") && ids(after, "participant-audio").contains("a"),
               "\(t).APanelistOnTheProgramWallWhoTurnsTheirCameraOffLosesVideoButKeepsAudio")
    }

    /// A model with a spine seam, the harness timer, and a meeting known to be on.
    @MainActor
    private static func spineModel(_ core: FakeTakeCore) -> (AppModel, ManualTimers, () -> [JSONObject]) {
        let model = takeModel(core)
        let timers = ManualTimers()
        model.timerSleepForTesting = { await timers.sleep($0) }
        let spines = SpineBox()
        model.spineRequestForTesting = { body in
            if body["type"] as? String == "zoom-media-spine-sync" { spines.append(body) }
            return ["ok": true]
        }
        model.applyZoomForTesting(["meetingState": "in_meeting", "participants": [JSONObject]()])
        return (model, timers, { spines.values })
    }

    final class SpineBox: @unchecked Sendable {
        private let lock = NSLock(); private var _values: [JSONObject] = []
        var values: [JSONObject] { lock.lock(); defer { lock.unlock() }; return _values }
        func append(_ v: JSONObject) { lock.lock(); _values.append(v); lock.unlock() }
    }

    @MainActor
    private static func lastSpine(_ timers: ManualTimers, _ spines: () -> [JSONObject], after count: Int) -> JSONObject {
        spin(until: { timers.fireAll(); return spines().count > count }, limit: 3)
        spin(until: { timers.fireAll(); return false }, limit: 0.3)   // let the last re-push land
        return spines().last?["spinePayload"] as? JSONObject ?? [:]
    }

    private static func subs(_ payload: JSONObject, _ kind: String) -> [JSONObject] {
        (payload["subscriptions"] as? [JSONObject] ?? []).filter { $0["kind"] as? String == kind }
    }

    @MainActor static func testCueingAWallRepushesTheSpineWithItsMembers() {
        let w = "Task 11: a cue re-pushes the spine"
        let core = FakeTakeCore()
        let (model, timers, spines) = spineModel(core)
        model.roster = [guest("1"), guest("2"), guest("3")]
        for i in model.slots.indices { model.slots[i].inShow = false }
        model.scenes.append(SceneDef(id: "wall", name: "Wall", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        let before = spines().count
        model.selectPreviewScene("wall")
        let payload = lastSpine(timers, spines, after: before)
        let video = subs(payload, "participant-video")
        expectEqual(video.compactMap { $0["participantId"] as? String }, ["1", "2", "3"], "\(w): wall members get video")
        expect(video.allSatisfy { $0["purpose"] as? String == "preview-tiles" }, "\(w): purpose preview-tiles")
        expectEqual(payload["sourceParticipantIds"] as? [String], ["1", "2", "3"], "\(w): the director's source set")
        let multiview = payload["multiview"] as? JSONObject
        expectEqual(multiview?["previewNotice"] as? String, "", "\(w): notices always present, empty when clear")
        expectEqual(multiview?["programNotice"] as? String, "", "\(w): programNotice present")
    }

    @MainActor static func testACameraOffWallMemberKeepsAudioWhileTheWallIsOnABus() {
        let w = "Task 11: sticky Tiles audio"
        let core = FakeTakeCore()
        let (model, timers, spines) = spineModel(core)
        model.roster = [guest("1"), guest("2"), guest("3")]
        for i in model.slots.indices { model.slots[i].inShow = false }
        model.scenes.append(SceneDef(id: "wall", name: "Wall", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        model.programSceneId = "wall"
        _ = model.spinePushRequests()   // the latch observes 2 as a member
        model.roster = [guest("1"), guest("2", video: false), guest("3")]
        let payload = model.spinePushRequests().last?["spinePayload"] as? JSONObject ?? [:]
        expect(!subs(payload, "participant-video").contains { $0["participantId"] as? String == "2" }, "\(w): 2 has no video")
        expect(subs(payload, "participant-audio").contains { $0["participantId"] as? String == "2" }, "\(w): 2 keeps audio")
        expect(model.zoomAudioSourceIds.contains("2"), "\(w): the console's audio set includes the sticky member")
        // Program moves off the wall onto a non-wall scene; Preview is not the wall either.
        model.programSceneId = "s2"; model.previewSceneId = "s1"
        let off = model.spinePushRequests().last?["spinePayload"] as? JSONObject ?? [:]
        expect(!subs(off, "participant-audio").contains { $0["participantId"] as? String == "2" }, "\(w): off both buses, 2 has no audio")
        _ = timers; _ = spines
    }

    @MainActor static func testCaptureOffForgetsTheLatch() {
        let w = "Task 11: Engine off forgets the latch (StudioViewModel.cs:10707)"
        let core = FakeTakeCore()
        let (model, _, _) = spineModel(core)
        model.roster = [guest("1"), guest("2")]
        model.scenes.append(SceneDef(id: "wall", name: "Wall", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        model.programSceneId = "wall"
        _ = model.spinePushRequests()
        model.setCapture(enabled: false)
        model.roster = [guest("1"), guest("2", video: false)]
        let payload = model.spinePushRequests().last?["spinePayload"] as? JSONObject ?? [:]
        expect(!subs(payload, "participant-audio").contains { $0["participantId"] as? String == "2" }, "\(w): the camera-off member is no longer latched")
    }

    @MainActor static func testACoreRespawnResendsBothWallsAndTheSpineAndKeepsTheLatch() {
        let w = "Review Focus 4: core respawn with the wall on Program"
        let core = FakeTakeCore()
        let (model, timers, spines) = spineModel(core)
        model.roster = [guest("1"), guest("2")]
        model.scenes.append(SceneDef(id: "wall", name: "Wall", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        model.scenes.append(SceneDef(id: "wall2", name: "Wall 2", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        model.programSceneId = "wall"; model.previewSceneId = "wall2"
        _ = model.spinePushRequests()            // latch holds 1 and 2
        model.roster = [guest("1"), guest("2", video: false)]
        let bodies = core.bodies.count, spineCount = spines().count
        model.onConnected()
        expect(model.tilesLatchMeetingState == nil, "\(w): a new generation's meeting state is unknown")
        let payload = lastSpine(timers, spines, after: spineCount)
        expect(subs(payload, "participant-audio").contains { $0["participantId"] as? String == "2" },
               "\(w): an unknown-state tick keeps the latch: 2 is still audible")
        let commands = flushSceneSync(core, timers, after: bodies)
        expect(command(commands, "load-scene-graph")["tiles"] != nil && command(commands, "set-preview-scene")["tiles"] != nil,
               "\(w): both walls re-sent")
    }

    static func testBusCaptionCarriesTheCoresNotice() {
        let w = "MultiviewOverlayFormatting.WithBusNotice"
        expectEqual(AppModel.busCaption(role: "pgm", label: "Program · no video: Ann (subscription limit 10)"),
                    "PROGRAM · no video: Ann (subscription limit 10)", "\(w): Program notice")
        expectEqual(AppModel.busCaption(role: "pvw", label: "Preview"), "PREVIEW", "\(w): no notice")
        expectEqual(AppModel.busCaption(role: "pvw", label: ""), "PREVIEW", "\(w): empty label")
    }

    // ── Task 13 ──────────────────────────────────────────────────────────────

    private static func person(_ session: String, _ name: String, _ pid: String? = nil) -> TilesIdentityPolicy.Person {
        TilesIdentityPolicy.Person(sessionSourceId: "zoom:" + session, displayName: name, persistentId: pid)
    }

    static func testTilesIdentityPortsTheWindowsFacts() {
        let t = "TilesIdentityPolicyTests"
        typealias P = TilesIdentityPolicy
        for id in ["capture:cam-1", "capture:screen:1", "media:clip-7"] {
            let roster = [person("16778240", "Someone", "pid-1")]
            expectEqual(P.persist(id, roster: roster), id, "\(t).Local1071_ALocalId…: persist \(id)")
            expectEqual(P.resolveLiveSourceId(id, roster: roster, currentMeetingId: "8916561023", boundMeetingId: "97682593786"), id,
                        "\(t).Local1071_ALocalId…: resolve \(id)")
            expectEqual(P.resolveLiveSourceId(id, roster: [], currentMeetingId: nil, boundMeetingId: nil), id, "\(t).Local1071_ALocalId…: no roster")
            expectEqual(P.label(id, roster: roster), id, "\(t).Local1071_ALocalId…: label")
            var s = TilesSettings(); s.manualSlots = [id]; s.excludedSourceIds = [id]; s.boundMeetingId = "97682593786"
            expect(P.describeStale(s, roster: roster, meetingId: "8916561023", liveLocal: nil).isEmpty, "\(t).Local1071_ALocalId…: not stale unjudged")
            P.clearStale(&s, roster: roster, meetingId: "8916561023", liveLocal: nil)
            expectEqual(s.manualSlots, [id], "\(t).Local1071_ALocalId…: slot kept")
            expectEqual(s.excludedSourceIds, [id], "\(t).Local1071_ALocalId…: never-show kept")
            expect(P.describeStale(s, roster: roster, meetingId: "8916561023", liveLocal: [id]).isEmpty,
                   "\(t).Local1071_ALocalIdPersistsAndResolvesUnchangedInAnyMeeting (\(id))")
        }

        do {
            let roster = [person("16778240", "Someone", "pid-1")]
            var s = TilesSettings()
            s.manualSlots = ["zoom-pid:pid-1", "capture:gone", "capture:cam-1"]
            s.excludedSourceIds = ["media:deleted", "capture:cam-1"]; s.boundMeetingId = "97682593786"
            let stale = P.describeStale(s, roster: roster, meetingId: "8916561023", liveLocal: ["capture:cam-1"])
            expectEqual(stale.map(\.label), ["2: capture:gone: not in the show", "media:deleted: not in the show"],
                        "\(t).Local1071_ALocalEntryWhoseSourceLeftTheShowIsStaleAndClearable: labels")
            expectEqual(stale.map(\.kind), ["slot", "never-show"], "\(t).Local1071_ALocalEntry…: kinds")
            P.clearStale(&s, roster: roster, meetingId: "8916561023", liveLocal: ["capture:cam-1"])
            expectEqual(s.manualSlots, ["zoom-pid:pid-1", nil, "capture:cam-1"], "\(t).Local1071_ALocalEntry…: cleared slot")
            expectEqual(s.excludedSourceIds, ["capture:cam-1"], "\(t).Local1071_ALocalEntry…: cleared never-show")
            P.clearStale(&s, roster: roster, meetingId: "8916561023", liveLocal: [])
            expectEqual(s.manualSlots, ["zoom-pid:pid-1", nil, nil], "\(t).Local1071_ALocalEntry…: empty live set")
            expect(s.excludedSourceIds.isEmpty, "\(t).Local1071_ALocalEntryWhoseSourceLeftTheShowIsStaleAndClearable")
        }

        do {
            var s = TilesSettings(); s.manualSlots = ["zoom:33556480"]; s.excludedSourceIds = ["zoom:67109888"]; s.boundMeetingId = "97682593786"
            let next = [person("33556480", "Someone else"), person("16778240", "Jason Bache", "jason-pid")]
            expect(P.resolveLiveSourceId("zoom:33556480", roster: next, currentMeetingId: "8916561023", boundMeetingId: s.boundMeetingId) == nil
                   && P.resolveLiveSourceId("zoom:67109888", roster: next, currentMeetingId: "8916561023", boundMeetingId: s.boundMeetingId) == nil,
                   "\(t).LiveCase479_LegacySessionIdIsNotAppliedToWhoeverInheritedItNextMeeting")
            let stale = P.describeStale(s, roster: next, meetingId: "8916561023", liveLocal: nil)
            expect(stale.contains { $0.kind == "slot" && $0.label.contains("not in this meeting") } && stale.contains { $0.kind == "never-show" },
                   "\(t).LiveCase479_…: both stale")
        }

        expectEqual(P.persist("zoom:16778240", roster: [person("16778240", "Jamal", "jamal-pid")]), "zoom-pid:jamal-pid", "\(t).PersistPrefersPersistentIdThenDisplayName: pid")
        expectEqual(P.persist("zoom:16778240", roster: [person("16778240", "Jamal")]), "zoom-name:Jamal", "\(t).PersistPrefersPersistentIdThenDisplayName: name")
        expectEqual(P.persist("capture:cam-1", roster: [person("16778240", "Jamal", "jamal-pid")]), "capture:cam-1", "\(t).PersistPrefersPersistentIdThenDisplayName: local")

        let saved = P.persist("zoom:111", roster: [person("111", "Jason Bache")])
        expectEqual(saved, "zoom-name:Jason Bache", "\(t).NameKeyFollowsThePersonAcrossSessionIds: saved")
        expectEqual(P.resolveLiveSourceId(saved, roster: [person("222", "Jason Bache")], currentMeetingId: "meeting-2", boundMeetingId: "meeting-1"),
                    "zoom:222", "\(t).NameKeyFollowsThePersonAcrossSessionIds")
        expect(P.resolveLiveSourceId("zoom-name:Alex", roster: [person("1", "Alex"), person("2", "Alex")], currentMeetingId: "m", boundMeetingId: "m") == nil,
               "\(t).DuplicateDisplayNamesDoNotStealASlot")
        expect(P.resolveLiveSourceId("zoom:16778240", roster: [person("16778240", "David Paskin")], currentMeetingId: "8916561023", boundMeetingId: nil) == nil,
               "\(t).UnboundLegacySessionIdIsNeverApplied")

        do {
            var s = TilesSettings(); s.manualSlots = ["zoom:33556480"]; s.boundMeetingId = "97682593786"
            let next = [person("33556480", "Someone else"), person("222", "Jason Bache")]
            s.boundMeetingId = "8916561023"
            s.manualSlots.append(P.persist("zoom:222", roster: next))
            expect(P.resolveLiveSourceId("zoom:33556480", roster: next, currentMeetingId: "8916561023", boundMeetingId: s.boundMeetingId) == nil,
                   "\(t).AssigningInANewMeetingDoesNotValidateLeftoverSessionIds: leftover")
            expectEqual(P.resolveLiveSourceId("zoom-name:Jason Bache", roster: next, currentMeetingId: "8916561023", boundMeetingId: s.boundMeetingId),
                        "zoom:222", "\(t).AssigningInANewMeetingDoesNotValidateLeftoverSessionIds")
        }

        do {
            var s = TilesSettings()
            s.manualSlots = ["zoom-name:Jamal", "zoom:33556480"]; s.excludedSourceIds = ["zoom-name:Jason Bache", "zoom-name:Susan"]; s.boundMeetingId = "old"
            P.clearStale(&s, roster: [person("9", "Susan")], meetingId: "new", liveLocal: nil)
            expectEqual(s.manualSlots, [nil, nil], "\(t).ClearStaleRemovesMissingSlotsAndNeverShow: slots")
            expectEqual(s.excludedSourceIds, ["zoom-name:Susan"], "\(t).ClearStaleRemovesMissingSlotsAndNeverShow")
        }

        expectEqual(P.label("zoom-pid:x", roster: []), "saved Zoom participant", "Label: unknown pid")
        expectEqual(P.label("zoom:5", roster: nil), "Unavailable source", "Label: session id without roster")
        expectEqual(P.label("", roster: nil), "Unavailable source", "Label: empty")
    }

    static func testTilesMembershipPortsTheWindowsFacts() {
        let t = "TilesMembershipPolicyTests"
        typealias M = TilesMembershipPolicy
        var manual = TilesSettings(); manual.autoFill = false; manual.membershipMode = "manual"
        manual.manualSlots = ["zoom:a", nil, "zoom:absent", "zoom:b", "zoom:a"]; manual.excludedSourceIds = ["zoom:b"]
        expectEqual(M.resolve(manual, eligible: ["zoom:a", "zoom:c"], routed: nil, roster: nil, meetingId: nil),
                    ["zoom:a", "", "zoom:absent", "", ""], "\(t).ManualSlotsKeepHolesAndMissingIdentityWithoutPromotingRoster")
        expectEqual(M.resolve(manual, eligible: ["zoom:a", "zoom:c", "zoom:absent"], routed: nil, roster: nil, meetingId: nil),
                    ["zoom:a", "", "zoom:absent", "", ""], "\(t).ManualSlotsKeepHoles…: unchanged when present")

        var routed = TilesSettings(); routed.membershipMode = "routed"; routed.manualSlots = ["zoom:c"]
        expectEqual(M.resolve(routed, eligible: ["zoom:a", "zoom:b", "zoom:c"], routed: ["zoom:a", "zoom:c", "capture:cam"], roster: nil, meetingId: nil),
                    ["zoom:c", "zoom:a"], "\(t).RoutedModeUsesOnlyLiveSourcesAssignedToTheShow")

        let mixed = [person("a", "Guest A", "pid-a"), person("b", "Guest B", "pid-b")]
        var bound = TilesSettings(); bound.boundMeetingId = "111"
        expectEqual(M.resolve(bound, eligible: ["zoom:a", "capture:cam-1", "zoom:b", "media:clip-7"], routed: nil, roster: mixed, meetingId: "111"),
                    ["zoom:a", "capture:cam-1", "zoom:b", "media:clip-7"], "\(t).Local1071_EligibleModeSeatsZoomAndLocalMembersInEligibleOrder")
        var manualLocal = TilesSettings(); manualLocal.membershipMode = "manual"
        manualLocal.manualSlots = ["capture:screen:1", "zoom-pid:pid-a", "media:clip-7"]; manualLocal.boundMeetingId = "111"
        expectEqual(M.resolve(manualLocal, eligible: ["zoom:a", "capture:screen:1"], routed: nil, roster: mixed, meetingId: "222"),
                    ["capture:screen:1", "zoom:a", "media:clip-7"], "\(t).Local1071_ManualSlotsHoldCanonicalLocalIdsWithoutRosterResolution")
        var never = TilesSettings(); never.excludedSourceIds = ["capture:cam-1"]; never.manualSlots = ["capture:cam-1"]; never.boundMeetingId = "111"
        let eligible = ["zoom:a", "capture:cam-1", "media:clip-7"]
        expectEqual(M.resolve(never, eligible: eligible, routed: nil, roster: mixed, meetingId: "111"), ["zoom:a", "media:clip-7"],
                    "\(t).Local1071_NeverShowAcceptsALocalIdAndKeepsItOffEveryMode: eligible")
        never.membershipMode = "manual"
        expectEqual(M.resolve(never, eligible: eligible, routed: nil, roster: mixed, meetingId: "111"), [""],
                    "\(t).Local1071_NeverShow…: manual")
        never.membershipMode = "routed"
        expectEqual(M.resolve(never, eligible: eligible, routed: ["capture:cam-1", "media:clip-7"], roster: mixed, meetingId: "111"), ["media:clip-7"],
                    "\(t).Local1071_NeverShowAcceptsALocalIdAndKeepsItOffEveryMode")
        var routedMixed = TilesSettings(); routedMixed.membershipMode = "routed"
        expectEqual(M.resolve(routedMixed, eligible: ["zoom:a", "capture:cam-1", "media:clip-7", "capture:screen:1"],
                              routed: ["capture:screen:1", "zoom:a"], roster: mixed, meetingId: "111"),
                    ["zoom:a", "capture:screen:1"], "\(t).Local1071_RoutedModeKeepsFilteringMixedIdsToTheRoutedSet")

        var auto = TilesSettings(); auto.maxTiles = 3; auto.manualSlots = ["zoom:c"]; auto.excludedSourceIds = ["zoom:b"]
        expectEqual(M.resolve(auto, eligible: ["zoom:a", "zoom:b", "zoom:c", "zoom:d", "zoom:e"], routed: nil, roster: nil, meetingId: nil),
                    ["zoom:c", "zoom:a", "zoom:d"], "\(t).AutomaticFillRespectsPriorityAssignmentsExclusionsAndLimit")

        var live = TilesSettings(); live.maxTiles = 4; live.membershipMode = "manual"; live.manualSlots = ["zoom:33556480"]
        live.excludedSourceIds = ["zoom-name:Jason Bache"]; live.boundMeetingId = "97682593786"
        let roster = [person("16778240", "Someone else"), person("222", "Jason Bache")]
        expectEqual(M.resolve(live, eligible: ["zoom:16778240", "zoom:222", "zoom:333"], routed: nil, roster: roster, meetingId: "8916561023"),
                    [""], "\(t).LiveCase479_…: a leftover session id seats nobody")
        live.membershipMode = "eligible"
        let members = M.resolve(live, eligible: ["zoom:16778240", "zoom:222", "zoom:333"], routed: nil, roster: roster, meetingId: "8916561023")
        expect(!members.contains("zoom:222") && members.contains("zoom:16778240") && members.contains("zoom:333"),
               "\(t).LiveCase479_NameKeyedNeverShowFollowsThePersonAndLegacyIdsDoNotSeatAStranger")
    }

    static func testARejoinedGuestKeepsTheirSlotAndADuplicateNameSeatsNobody() {
        let w = "Review Focus 5"
        var s = TilesSettings(); s.membershipMode = "manual"; s.manualSlots = ["zoom-pid:P1", "zoom-name:Sam"]; s.boundMeetingId = "m1"
        let rejoined = [person("900", "Pat", "P1"), person("31", "Sam"), person("32", "Sam")]
        expectEqual(TilesMembershipPolicy.resolve(s, eligible: ["zoom:900", "zoom:31", "zoom:32"], routed: nil, roster: rejoined, meetingId: "m1"),
                    ["zoom:900", ""], "\(w): the pid slot follows the rejoined session id; two Sams seat nobody")
        s.membershipMode = "eligible"; s.excludedSourceIds = ["zoom-name:Sam"]
        expectEqual(TilesMembershipPolicy.resolve(s, eligible: ["zoom:900", "zoom:31", "zoom:32"], routed: nil, roster: rejoined, meetingId: "m1"),
                    ["zoom:900", "zoom:31", "zoom:32"], "\(w): an ambiguous never-show name excludes nobody (it resolves to no one)")
    }

    @MainActor static func testArmingIsoRepushesTheSpine() {
        let w = "S2 review: ISO arming re-pushes the spine"
        let core = FakeTakeCore()
        let (model, timers, spines) = spineModel(core)
        model.roster = [guest("1"), guest("9")]
        for i in model.slots.indices { model.slots[i].inShow = false; model.slots[i].iso = false }
        if let i = model.slots.firstIndex(where: { $0.id == 3 }) {
            model.slots[i].kind = "zoom"; model.slots[i].sourceId = "9"; model.slots[i].iso = true
        }
        let before = spines().count
        model.isoRecordingEnabled = true
        let payload = lastSpine(timers, spines, after: before)
        expect(subs(payload, "participant-video").contains { $0["participantId"] as? String == "9" && $0["purpose"] as? String == "iso" },
               "\(w): the ISO-armed guest is subscribed at purpose iso")
    }

    static func testAnOverBudgetMacMultiviewTileCarriesTheLabel() {
        let w = "S2 review: the Mac multiview kind gets the unsubscribed label"
        var input = ZoomSourceBudget.Input()
        input.participants = (0..<11).map { ("g\($0)", true) }
        input.multiview = (0..<11).map { ["kind": "participant-video", "participantId": "g\($0)", "slot": $0, "label": "Guest \($0)"] }
        let parts = ZoomSourceBudget.spineParts(input, names: [:])
        expectEqual(parts.multiviewSources.last?["label"] as? String, "Guest 10 · no video: subscription limit (10)", "\(w): label")
        expectEqual(parts.shortfall.first?["participantId"] as? String, "g10", "\(w): shortfall")
    }

    // ── Task 14 ──────────────────────────────────────────────────────────────

    static func testTilesMemberChoicesPortTheWindowsFacts() {
        let t = "TilesMemberChoicePolicyTests"
        let locals = [TilesLocalSource(id: "capture:screen:1", name: "Input 02", group: "Screen"),
                      TilesLocalSource(id: "capture:ndi-1", name: "Studio NDI", group: "NDI"),
                      TilesLocalSource(id: "capture:cam-b", name: "Cam B", group: "Camera"),
                      TilesLocalSource(id: "capture:cam-a", name: "Cam A", group: "Camera")]
        let roster = [RosterParticipant(id: "2", name: "Zed", hasVideo: true, muted: false, talking: false, assigned: false),
                      RosterParticipant(id: "1", name: "Amy", hasVideo: true, muted: false, talking: false, assigned: false)]
        let choices = TilesMemberChoicePolicy.build(roster: roster, local: locals)
        expectEqual(choices.map(\.id), ["zoom:2", "zoom:1", "capture:cam-b", "capture:cam-a", "capture:screen:1", "capture:ndi-1"],
                    "\(t).Build_ListsZoomInRosterOrderThenLocalsInPickerGroupOrderThenSlotOrder")
        expectEqual(choices.map(\.group), ["Zoom", "Zoom", "Camera", "Camera", "Screen", "NDI"], "\(t).Build_Lists…: groups")
        let labelled = TilesMemberChoicePolicy.build(roster: [roster[1]], local: [TilesLocalSource(id: "capture:cam-1", name: "Logitech Brio", group: "Camera")])
        expectEqual(labelled.map(\.label), ["Amy", "Camera · Logitech Brio"], "\(t).Build_LabelsALocalSourceAsGroupDotNameAndAZoomGuestByNameAlone")
        expectEqual(labelled[1].name, "Logitech Brio", "\(t).Build_Labels…: name")
        let dup = TilesMemberChoicePolicy.build(
            roster: [roster[1], RosterParticipant(id: "zoom:1", name: "Amy again", hasVideo: true, muted: false, talking: false, assigned: false)],
            local: [TilesLocalSource(id: "capture:cam-1", name: "Cam", group: "Camera"), TilesLocalSource(id: "capture:cam-1", name: "Cam dup", group: "Camera")])
        expectEqual(dup.map(\.id), ["zoom:1", "capture:cam-1"], "\(t).Build_QualifiesBareZoomIdsAndDeduplicatesById")
        let c = TilesMemberChoicePolicy.build(roster: [roster[1]], local: [TilesLocalSource(id: "capture:cam-1", name: "Cam", group: "Camera")])
        expect(TilesMemberChoicePolicy.contains(c, "zoom:1") && TilesMemberChoicePolicy.contains(c, "capture:cam-1")
               && !TilesMemberChoicePolicy.contains(c, "capture:cam-2") && !TilesMemberChoicePolicy.contains(c, "cam-1")
               && !TilesMemberChoicePolicy.contains(c, "media:clip-7") && !TilesMemberChoicePolicy.contains(c, "")
               && !TilesMemberChoicePolicy.contains(c, nil), "\(t).Contains_AcceptsOnlyCurrentChoicesSoSettersTakeInShowLocalIds")
        let off = TilesMemberChoicePolicy.build(roster: [RosterParticipant(id: "5", name: "Off", hasVideo: false, muted: false, talking: false, assigned: false)], local: [])
        expect(off.isEmpty, "Windows builds choices from RoomVideoParticipants: a camera-off guest is not offered")
    }

    static func testTilesWorkspaceMembershipRules() {
        let t = "SceneWorkspaceParityTests.CapacityDoesNotSilentlyUncheckConfiguredMembers"
        var wall = TilesSettings(); wall.membershipMode = "eligible"; wall.maxTiles = 1
        var states = TilesWorkspaceMembership.build(wall, members: ["zoom:1", "zoom:2"], routed: [], roster: [], meetingId: nil)
        expect(states.values.allSatisfy(\.selected) && states.values.allSatisfy { !$0.canSelect }, "\(t): eligible selects all, edits none")
        wall.membershipMode = "routed"
        states = TilesWorkspaceMembership.build(wall, members: ["zoom:1", "zoom:2"], routed: ["zoom:1"], roster: [], meetingId: nil)
        expect(states["zoom:1"]?.selected == true && states["zoom:2"]?.canSelect == false, t)
        var manual = TilesSettings(); manual.membershipMode = "manual"; manual.manualSlots = ["zoom-name:Ann"]
        manual.excludedSourceIds = ["zoom:5"]; manual.overrides = ["zoom:7": TilesMemberOverride(rect: TilesRect())]
        let people = [person("7", "Ann")]
        states = TilesWorkspaceMembership.build(manual, members: ["zoom:7", "zoom:8"], routed: [], roster: people, meetingId: nil)
        expectEqual(states["zoom:7"], TilesMemberState(selected: true, canSelect: true, excluded: false, pinned: true), "SceneWorkspaceMembershipPolicy: a manual slot by name, pinned")
        expectEqual(states["zoom:8"], TilesMemberState(selected: false, canSelect: true, excluded: false, pinned: false), "SceneWorkspaceMembershipPolicy: unslotted")
        expect(TilesWorkspaceMembership.build(nil, members: ["zoom:1"], routed: [], roster: [], meetingId: nil).isEmpty, "no wall, no states")
    }

    /// Preview t-pvw (a wall), Program s2; roster 7 (pid P7), 2 (pid P2), 9; capture cam-a in show.
    @MainActor
    private static func tilesActionModel() -> AppModel {
        let model = sceneEditModel()
        model.scenes.append(SceneDef(id: "t-pvw", name: "Wall", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        model.previewSceneId = "t-pvw"
        model.roster = [RosterParticipant(id: "7", name: "Ann", hasVideo: true, muted: false, talking: false, assigned: false, persistentId: "P7"),
                        RosterParticipant(id: "2", name: "Bo", hasVideo: true, muted: false, talking: false, assigned: false, persistentId: "P2"),
                        RosterParticipant(id: "9", name: "Cy", hasVideo: true, muted: false, talking: false, assigned: false)]
        model.joinMeetingId = "8675309"
        return model
    }

    @MainActor static func testEveryTilesActionRefusesWhenPreviewIsNotATilesScene() {
        let w = "Task 14: every action is Preview-wall only"
        let model = tilesActionModel()
        model.previewSceneId = "s1"   // not a wall
        let program = model.programSceneId, syncs = model.sceneSyncRequestCount
        let before = model.currentPrefs()
        let actions: [(String, () throws -> Void)] = [
            ("mode", { try model.setTilesMembershipMode("manual") }),
            ("assign", { try model.assignTilesSlot(1, sourceId: "zoom:7") }),
            ("exclude", { try model.setTilesExclusion("zoom:7", excluded: true) }),
            ("include", { try model.includeTilesMember("zoom:7", included: true) }),
            ("clear stale", { try model.clearStaleTiles() }),
            ("update", { try model.updateTiles { $0.maxTiles = 3 } }),
        ]
        for (name, action) in actions {
            model.studioCommandStatus = ""
            var thrown: TilesEditError?
            do { try action() } catch let e as TilesEditError { thrown = e } catch {}
            expectEqual(thrown, .notTilesPreview, "\(w): \(name) throws notTilesPreview")
            expectEqual(model.studioCommandStatus, "Queue a CoreVideo Tiles scene in Preview first.", "\(w): \(name) says why")
        }
        // Program is never the target, even when it is a wall.
        model.programSceneId = "t-pvw"; model.previewSceneId = "t-pvw"
        expect((try? model.assignTilesSlot(1, sourceId: "zoom:7")) == nil, "\(w): a wall on Program (Preview == Program) is not edited")
        model.programSceneId = program; model.previewSceneId = "s1"
        expectEqual(model.sceneSyncRequestCount, syncs, "\(w): nothing synced")
        expectEqual(model.currentPrefs().scenes, before.scenes, "\(w): persisted scenes unchanged")
        expectEqual(model.programSceneId, program, "\(w): Program unchanged")
    }

    @MainActor static func testTilesSlotAssignmentPortsTheWindowsFacts() {
        let t = "TilesMembershipPolicyTests"
        let model = tilesActionModel()
        for slot in [0, 65] {
            let before = model.scenes.first { $0.id == "t-pvw" }?.tiles, syncs = model.sceneSyncRequestCount
            var thrown: TilesEditError?
            do { try model.assignTilesSlot(slot, sourceId: "zoom:7") } catch let e as TilesEditError { thrown = e } catch {}
            expectEqual(thrown, .slotOutOfRange, "\(t).InvalidApiSlotNeverMutates(\(slot))")
            expectEqual(model.studioCommandStatus, "Tiles slot must be 1–64.", "\(t).InvalidApiSlotNeverMutates: message")
            expect(model.scenes.first { $0.id == "t-pvw" }?.tiles == before && model.sceneSyncRequestCount == syncs,
                   "\(t).InvalidApiSlotNeverMutates(\(slot)): nothing changed")
        }
        do { try model.assignTilesSlot(64, sourceId: "zoom:7") } catch { expect(false, "\(t).ApiSlotPasses…: slot 64 accepted") }
        let tiles = model.scenes.first { $0.id == "t-pvw" }?.tiles
        expectEqual(tiles?.manualSlots.count, 64, "\(t).ApiSlotPassesCanonicalIdentityAndReportsMutationFailure: slot 64 exists")
        expectEqual(tiles?.manualSlots.last ?? nil, "zoom-pid:P7", "\(t).ApiSlotPasses…: saved as a stable identity")
        expectEqual(tiles?.boundMeetingId, "8675309", "\(t).ApiSlotPasses…: bound to the current meeting")
        var thrown: TilesEditError?
        do { try model.assignTilesSlot(1, sourceId: "zoom:404") } catch let e as TilesEditError { thrown = e } catch {}
        expectEqual(thrown, .unavailableSource, "an unavailable source is refused")
        expectEqual(model.studioCommandStatus, "Choose an available source for this Tiles slot.", "an unavailable source: message")
        // Confirmatory (value types): a copy never aliases membership intent.
        var copy = tiles!; copy.manualSlots[63] = "zoom:c"; copy.excludedSourceIds = []
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots.last ?? nil, "zoom-pid:P7",
                    "\(t).CloneDoesNotAliasMembershipIntent (confirmatory: TilesSettings is a value type)")
    }

    @MainActor static func testAssigningAGuestMovesThemNotDuplicatesThem() {
        let w = "Task 14: AssignTilesSlot moves"
        let model = tilesActionModel()
        try? model.assignTilesSlot(1, sourceId: "zoom:2")
        try? model.assignTilesSlot(3, sourceId: "zoom:2")
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, [nil, nil, "zoom-pid:P2"], "\(w): one seat, the last one")
        try? model.assignTilesSlot(3, sourceId: "")
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, [nil, nil, nil], "\(w): \"\" clears the slot")
        try? model.setTilesExclusion("zoom:9", excluded: true)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.excludedSourceIds, ["zoom-name:Cy"], "\(w): never-show by name when there is no pid")
        try? model.setTilesExclusion("zoom:9", excluded: false)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.excludedSourceIds, [], "\(w): allowed again")
    }

    @MainActor static func testIncludeInManualModeTakesTheFirstEmptySlot() {
        let w = "Task 14: WorkspaceIncludeMember"
        let model = tilesActionModel()
        try? model.setTilesMembershipMode("manual")
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.autoFill, false, "\(w): manual mode turns autoFill off")
        try? model.assignTilesSlot(2, sourceId: "zoom:7")
        try? model.includeTilesMember("zoom:2", included: true)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, ["zoom-pid:P2", "zoom-pid:P7"], "\(w): the first empty slot")
        try? model.includeTilesMember("zoom:9", included: true)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, ["zoom-pid:P2", "zoom-pid:P7", "zoom-name:Cy"], "\(w): else a new slot")
        try? model.includeTilesMember("zoom:9", included: false)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.omittedSourceIds, ["zoom-name:Cy"], "\(w): excluding records an omission")
        // 64 full slots refuse with the Windows text.
        try? model.updateTiles { $0.manualSlots = (0..<64).map { "capture:x\($0)" } }
        var thrown: TilesEditError?
        do { try model.includeTilesMember("zoom:2", included: true) } catch let e as TilesEditError { thrown = e } catch {}
        expectEqual(thrown, .slotsFull, "\(w): a full wall refuses")
        expectEqual(model.studioCommandStatus, "Clear a manual slot before adding another member; a wall supports 64 slots.", "\(w): message")
    }

    @MainActor static func testTilesMembershipSummaryReadsLikeWindows() {
        let w = "GalleryMembershipSummary"
        let model = tilesActionModel()
        try? model.assignTilesSlot(1, sourceId: "zoom:7")
        try? model.setTilesExclusion("zoom:9", excluded: true)
        expectEqual(model.tilesMembershipSummary, "Manual slots: 1: Ann · Never show: Cy", "\(w): names from the choices")
        try? model.updateTiles { $0.manualSlots.append("zoom-name:Gone") }
        expectEqual(model.tilesMembershipSummary, "Manual slots: 1: Ann, 2: Gone · Never show: Cy · Stale: 2: Gone: not in this meeting",
                    "\(w): stale entries listed")
        try? model.clearStaleTiles()
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, ["zoom-pid:P7", nil], "\(w): clear stale")
    }

    @MainActor static func testEveryTilesEditIsSentAndSaved() {
        let w = "S3 review: a successful edit is re-sent and saved"
        let model = tilesActionModel()
        let edits: [(String, () throws -> Void)] = [
            ("assign", { try model.assignTilesSlot(1, sourceId: "zoom:7") }),
            ("exclude", { try model.setTilesExclusion("zoom:9", excluded: true) }),
            ("mode", { try model.setTilesMembershipMode("manual") }),
            ("include", { try model.includeTilesMember("zoom:2", included: true) }),
            ("clear stale", { try model.clearStaleTiles() }),
        ]
        for (name, edit) in edits {
            let syncs = model.sceneSyncRequestCount
            do { try edit() } catch { expect(false, "\(w): \(name) succeeds") }
            expect(model.sceneSyncRequestCount > syncs, "\(w): \(name) syncs scenes")
            let saved = model.lastSavedPrefs.scenes.first { $0.id == "t-pvw" }?.tiles
            expectEqual(saved, model.scenes.first { $0.id == "t-pvw" }?.tiles?.persisted, "\(w): \(name) is saved at once")
        }
    }

    @MainActor static func testACameraOffGuestsSavedSlotIsNotStale() {
        let w = "S3 review: identities resolve against the FULL roster"
        let model = tilesActionModel()
        try? model.assignTilesSlot(1, sourceId: "zoom:7")
        model.roster[0].hasVideo = false   // Ann turns her camera off
        expect(!model.tilesMembershipSummary.contains("Stale"), "\(w): a camera-off guest's pid slot is not stale")
        try? model.clearStaleTiles()
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, ["zoom-pid:P7"], "\(w): clear stale keeps it")
    }

    @MainActor static func testTilesMoveAndIncludeCoverEveryPath() {
        let w = "S3 review: move and include paths"
        let model = tilesActionModel()
        // A legacy raw id and a name-keyed entry for the same guest are both cleared on a move.
        try? model.updateTiles { $0.manualSlots = ["zoom:2", "zoom-name:Cy"] }
        try? model.assignTilesSlot(3, sourceId: "zoom:2")
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, [nil, "zoom-name:Cy", "zoom-pid:P2"],
                    "\(w): the raw-id copy is cleared")
        try? model.assignTilesSlot(1, sourceId: "zoom:9")
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, ["zoom-name:Cy", nil, "zoom-pid:P2"],
                    "\(w): the name-keyed copy is cleared (Cy has no pid, so the new key is the name)")
        // Exclusion binds the wall to the meeting too.
        try? model.updateTiles { $0.boundMeetingId = nil }
        try? model.setTilesExclusion("zoom:7", excluded: true)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.boundMeetingId, "8675309", "\(w): exclusion binds the meeting")
        var thrown: TilesEditError?
        do { try model.setTilesExclusion(" ", excluded: true) } catch let e as TilesEditError { thrown = e } catch {}
        expectEqual(thrown, .unavailableSource, "\(w): a blank exclude is refused")
        expectEqual(model.studioCommandStatus, "Choose a source to exclude or allow.", "\(w): with the Windows blank-id text")
        // Eligible mode: include is a no-op.
        let before = model.scenes.first { $0.id == "t-pvw" }?.tiles
        try? model.setTilesMembershipMode("eligible")
        let eligible = model.scenes.first { $0.id == "t-pvw" }?.tiles
        try? model.includeTilesMember("zoom:2", included: false)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles, eligible, "\(w): eligible mode ignores include/omit")
        _ = before
        // Routed mode: only the omitted list changes.
        try? model.setTilesMembershipMode("routed")
        let routedBefore = model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots
        try? model.includeTilesMember("zoom:2", included: false)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.omittedSourceIds, ["zoom-pid:P2"], "\(w): routed omit recorded")
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, routedBefore, "\(w): routed include never takes a slot")
        // Manual mode: including someone already on the wall takes no new slot.
        try? model.setTilesMembershipMode("manual")
        try? model.updateTiles { $0.manualSlots = ["zoom-pid:P7"]; $0.excludedSourceIds = []; $0.omittedSourceIds = [] }
        try? model.includeTilesMember("zoom:7", included: true)
        expectEqual(model.scenes.first { $0.id == "t-pvw" }?.tiles?.manualSlots, ["zoom-pid:P7"], "\(w): an existing member keeps one slot")
    }

    // ── Task 15 ──────────────────────────────────────────────────────────────

    @MainActor static func testEditTilesCuesPreviewOpensTheInspectorAndNeverTouchesProgram() {
        let w = "StudioViewModel.EditTiles (StudioWorkspaceLayoutProbe: opens the wall, preserves Program)"
        let model = sceneEditModel()   // Program s2, Preview s1
        model.scenes.append(SceneDef(id: "wall", name: "Panel wall", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        model.selectedTab = .studio
        model.editTiles("s3")          // not a Tiles scene: nothing happens
        expectEqual(model.previewSceneId, "s1", "\(w): a non-Tiles id does nothing")
        expectEqual(model.selectedTab, .studio, "\(w): the tab is unchanged for a non-Tiles id")
        model.editTiles("wall")
        expectEqual(model.programSceneId, "s2", "\(w): Program unchanged")
        expectEqual(model.previewSceneId, "wall", "\(w): the wall is on Preview")
        expectEqual(model.selectedTab, .scenes, "\(w): the Scenes tab opens")
        expectEqual(model.studioCommandStatus, "Editing Tiles in Panel wall", "\(w): status text")
        expectEqual(model.tilesEditTargetId, "wall", "\(w): the workspace edits the wall")
        model.editTiles("missing")
        expectEqual(model.previewSceneId, "wall", "\(w): an unknown id does nothing")
    }

    static func testTheScenesTabStopsScrollingAndWidensOnlyWhileEditingTiles() {
        let w = "TabPageLayout (Tiles workspace)"
        for tab in StudioTab.allCases {
            for editing in [false, true] {
                let expectWidth: CGFloat = (tab == .scenes && editing) ? .infinity : TabPageLayout.maxWidth(for: tab)
                expectEqual(TabPageLayout.maxWidth(for: tab, tilesEditing: editing), expectWidth,
                            "\(w): width of \(tab.rawValue) editing=\(editing)")
                let expectScroll = !(tab == .scenes && editing) && !TabPageLayout.fillsHeight(tab)
                expectEqual(TabPageLayout.scrolls(tab, tilesEditing: editing), expectScroll,
                            "\(w): scrolling of \(tab.rawValue) editing=\(editing)")
            }
        }
        expectEqual(TabPageLayout.maxWidth(for: .scenes, tilesEditing: false), 720, "\(w): Scenes keeps its measure otherwise")
        expect(!TabPageLayout.scrolls(.scenes, tilesEditing: true) && TabPageLayout.scrolls(.scenes, tilesEditing: false),
               "\(w): only the Tiles workspace stops the page scroll")
        expectEqual(TilesWorkspaceMetrics.dockWidth, 292, "\(w): the Windows 292 pt dock")
    }
}
