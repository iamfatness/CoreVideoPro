// Spec 5.3 "templates the playlist pins are reinstalled per core generation", as Windows
// does it: GraphicsDesignResync (fires once per armed new epoch with graphics state) +
// GraphicsDesignService.EnsureInstalledForPlaylistAsync (DESIGN template ids referenced by
// playlist rows, row images, saved cues and the studio lower-third cue; from the pin, else
// the show's `designs` entry; failures on the status line). Plan: 2026-11-22-mac-graphics-warmup.md W4/W5.

import Foundation

extension ShellTests {
    static var graphicsDesignResyncCases: [(String, () -> Void)] {
        [
            ("graphics-design-resync/installs-referenced-designs-once", testDesignResyncInstallsReferencedDesigns),
            ("graphics-design-resync/once-per-epoch", testDesignResyncOncePerEpoch),
            ("graphics-design-resync/refusal-on-status-line", testDesignResyncRefusalOnStatusLine),
            ("graphics-design-resync/not-armed-without-a-new-core", testDesignResyncNeedsANewCoreGeneration),
            ("graphics-design-resync/waits-for-the-capability", testDesignResyncWaitsForTheCapability),
        ]
    }

    static let resyncHash = String(repeating: "e", count: 64)

    static func resyncManifest(_ id: String, _ version: Int) -> JSONObject {
        ["templateId": id, "templateVersion": version, "runtime": "declarative", "fields": [["id": "heading", "type": "string"]]]
    }

    static func resyncPin(_ id: String, _ version: Int) -> JSONObject {
        ["templateId": id, "templateVersion": version, "manifestHash": resyncHash, "definition": resyncManifest(id, version), "packageRoot": ""]
    }

    /// A show that references, by every Windows path: a pinned design on a row, a design only in
    /// the `designs` section on a row's image, a pinned design on a saved cue and on the studio
    /// lower-third cue, a NON-design pin on a row (must stay lazy), and a design the core already has.
    @MainActor
    static func resyncModel() -> (GraphicsModel, ScriptedGraphicsCore) {
        let core = ScriptedGraphicsCore(epoch: "epoch-1")
        core.node["templates"] = realCatalogTemplates + [resyncManifest("design-held-aaa111", 1)]
        core.node["sources"] = [] as [Any]
        core.node["installHash"] = resyncHash
        core.react = ScriptedGraphicsCore.instancesReaction()
        let model = graphicsModel(core)
        model.playout.sleep = { [weak core, weak model] _ in if let core { model?.observe(snapshot: ["graphics": core.node]) } }
        var rows = interviewRows()
        rows[0].templateId = "design-lower-abc123"; rows[0].templateVersion = 2
        rows[1].templateId = "pinned-title"; rows[1].templateVersion = 1
        var imageRow = rows[2]
        imageRow.json["image"] = ["templateId": "design-card-def456", "templateVersion": 1]
        rows[2] = imageRow
        if rows.count > 3 { rows[3].templateId = "design-held-aaa111"; rows[3].templateVersion = 1 }
        model.document.playlist = rows
        model.document.json["templates"] = [resyncPin("design-lower-abc123", 2), resyncPin("pinned-title", 1),
                                            resyncPin("design-cue-bbb222", 3), resyncPin("design-slt-ccc333", 1)]
        model.document.json["designs"] = [["designId": "card", "name": "Card", "family": "card", "templateId": "design-card-def456",
                                           "templateVersion": 1, "definition": resyncManifest("design-card-def456", 1), "assets": [] as [Any]]]
        model.document.json["cues"] = [["id": "welcome", "name": "Welcome", "draft": ["templateId": "design-cue-bbb222", "templateVersion": 3]]]
        model.document.json["studioLowerThird"] = ["cue": ["draft": ["templateId": "design-slt-ccc333", "templateVersion": 1]]]
        return (model, core)
    }

    static let availableProfile: JSONObject = ["capabilityStates": ["graphics-instances": ["state": "available"]]]

    /// A new core process: AppModel calls coreGenerationChanged(), then the new core's first snapshot
    /// arrives (with `profile` unless `capabilityLater`, in which case a second snapshot brings it).
    @MainActor
    static func restartCore(_ model: GraphicsModel, _ core: ScriptedGraphicsCore, epoch: String, capabilityLater: Bool = false) {
        model.coreGenerationChanged()
        core.node["coreEpoch"] = epoch
        core.node["templates"] = realCatalogTemplates + [resyncManifest("design-held-aaa111", 1)]
        if capabilityLater {
            model.observe(snapshot: ["graphics": core.node])
            _ = awaitMain(5) { await model.designResync?.value; return true }
            expectEqual(installedIds(core).count, 0, "nothing is sent before the new core confirms graphics")
            model.observe(snapshot: ["graphics": core.node, "profile": availableProfile])
        } else {
            model.observe(snapshot: ["graphics": core.node, "profile": availableProfile])
        }
        _ = awaitMain(5) { await model.designResync?.value; return true }
    }

    @MainActor
    static func installedIds(_ core: ScriptedGraphicsCore) -> [String] {
        core.commands.filter { $0["type"] as? String == "graphics-template-install" }.compactMap {
            ((($0["graphics"] as? JSONObject)?["manifest"] as? String).flatMap(GraphicsJSON.parseObject))?["templateId"] as? String
        }
    }

    @MainActor
    static func testDesignResyncInstallsReferencedDesigns() {
        let (model, core) = resyncModel()
        restartCore(model, core, epoch: "epoch-2")
        expectEqual(installedIds(core).sorted(), ["design-card-def456", "design-cue-bbb222", "design-lower-abc123", "design-slt-ccc333"],
                    "every referenced design the new core lacks is installed: row, row image (from designs), saved cue, studio lower third")
        expect(!installedIds(core).contains("pinned-title"), "a non-design pin stays lazy (installed on first Ensure, as on Windows)")
        expect(!installedIds(core).contains("design-held-aaa111"), "a design the core already has is not reinstalled")
        expectEqual(model.status, GraphicsModel.restartedText, "success leaves the restart notice on the status line")
        expect(!model.busy, "the reinstall does not show the playout as busy")
    }

    /// One design is refused by the core, so a second run on the same core WOULD send something:
    /// that is what makes "once per epoch" observable (an all-installed rerun sends nothing).
    @MainActor
    static func refuseCueDesign(_ core: ScriptedGraphicsCore) {
        let base = core.react
        core.react = { command, node in
            let g = command["graphics"] as? JSONObject ?? [:]
            if command["type"] as? String == "graphics-template-install",
               ((g["manifest"] as? String).flatMap(GraphicsJSON.parseObject))?["templateId"] as? String == "design-cue-bbb222" {
                return (false, "template-invalid")
            }
            return base(command, &node)
        }
    }

    @MainActor
    static func testDesignResyncOncePerEpoch() {
        let (model, core) = resyncModel()
        refuseCueDesign(core)
        restartCore(model, core, epoch: "epoch-2")
        let first = installedIds(core).count
        expectEqual(first, 4, "one attempt per referenced design, the refused one included")
        model.observe(snapshot: ["graphics": core.node.merging(["revisionTick": 1]) { $1 }, "profile": availableProfile])
        _ = awaitMain(5) { await model.designResync?.value; return true }
        expectEqual(installedIds(core).count, first, "a later snapshot of the same core retries nothing, not even the refused design")
        restartCore(model, core, epoch: "epoch-3")
        expectEqual(installedIds(core).count, first * 2, "the next core generation installs them again")
    }

    @MainActor
    static func testDesignResyncRefusalOnStatusLine() {
        let (model, core) = resyncModel()
        refuseCueDesign(core)
        restartCore(model, core, epoch: "epoch-2")
        expect(model.status.hasPrefix("Graphics designs: "), "a refused reinstall is named on the status line (\(model.status))")
        expect(installedIds(core).contains("design-slt-ccc333"), "one refusal does not stop the others")
    }

    @MainActor
    static func testDesignResyncNeedsANewCoreGeneration() {
        let (model, core) = resyncModel()
        model.observe(snapshot: ["graphics": core.node.merging(["revisionTick": 1]) { $1 }])
        _ = awaitMain(5) { await model.designResync?.value; return true }
        expectEqual(installedIds(core), [], "no reinstall without a new core generation")
    }

    @MainActor
    static func testDesignResyncWaitsForTheCapability() {
        let (model, core) = resyncModel()
        restartCore(model, core, epoch: "epoch-2", capabilityLater: true)
        expectEqual(installedIds(core).count, 4, "the reinstall runs once the capability arrives, even with an unchanged graphics node")
        expectEqual(model.status, GraphicsModel.restartedText, "and is not reported as refused")
    }
}
