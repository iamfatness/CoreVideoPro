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
            ("graphics-design-resync/waits-for-the-show-document", testDesignResyncWaitsForTheShowDocument),
            ("graphics-design-resync/operator-show-waits-at-most-one-install", testDesignResyncAnOperatorShowWaitsForAtMostOneInstall),
            ("graphics-design-resync/dead-bridge-stops-the-reinstall", testDesignResyncStopsAtATransportFailure),
            ("graphics-design-resync/lower-third-adopt-during-reinstall", testDesignResyncLowerThirdAdoptDuringReinstall),
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
        model.documentLoaded = true   // as after loadDocument()/adopt
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
        if capabilityLater {
            beginRestart(model, core, epoch: epoch, profile: false)
            _ = awaitMain(5) { await model.designResync?.value; return true }
            expectEqual(installedIds(core).count, 0, "nothing is sent before the new core confirms graphics")
            model.observe(snapshot: ["graphics": core.node, "profile": availableProfile])
        } else {
            beginRestart(model, core, epoch: epoch)
        }
        _ = awaitMain(5) { await model.designResync?.value; return true }
    }

    /// The restart without waiting for the reinstall it starts.
    @MainActor
    static func beginRestart(_ model: GraphicsModel, _ core: ScriptedGraphicsCore, epoch: String, profile: Bool = true) {
        model.coreGenerationChanged()
        core.node["coreEpoch"] = epoch
        core.node["templates"] = realCatalogTemplates + [resyncManifest("design-held-aaa111", 1)]
        model.observe(snapshot: profile ? ["graphics": core.node, "profile": availableProfile] : ["graphics": core.node])
    }

    /// Parks the playout's NEXT sleep (inside an install, between its install and list commands)
    /// until the test resumes it: the reinstall is then genuinely mid-install, holding what it holds.
    @MainActor
    static func parkNextSleep(_ model: GraphicsModel, _ core: ScriptedGraphicsCore) -> ResyncPark {
        let park = ResyncPark()
        model.playout.sleep = { [weak core, weak model] _ in
            if park.armed { park.armed = false; await withCheckedContinuation { park.parked = $0 } }
            if let core { model?.observe(snapshot: ["graphics": core.node]) }
        }
        return park
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

    /// At launch the core can come up before the show document finishes loading (loadDocument is async).
    @MainActor
    static func testDesignResyncWaitsForTheShowDocument() {
        let (model, core) = resyncModel()
        let show = model.document
        model.document = GraphicsDocument()
        model.documentLoaded = false
        restartCore(model, core, epoch: "epoch-2")
        expectEqual(installedIds(core).count, 0, "nothing is installed before the show is loaded")
        model.adopt(GraphicsDocumentLoad(document: show, readOnly: false, recoveredBackup: false, status: ""))
        _ = awaitMain(5) { await model.designResync?.value; return true }
        expectEqual(installedIds(core).count, 4, "the reinstall runs when the show finishes loading")
    }

    /// Review round 1, IMPORTANT 1: the reinstall holds the playout gate for one design at a time,
    /// so an operator action that arrives mid-reinstall waits for at most the install in flight.
    @MainActor
    static func testDesignResyncAnOperatorShowWaitsForAtMostOneInstall() {
        let (model, core) = resyncModel()
        var rows = model.document.playlist
        rows[1].templateId = "centered-title"   // the operator's row needs no install of its own
        model.document.playlist = rows
        let park = parkNextSleep(model, core)
        beginRestart(model, core, epoch: "epoch-2")
        spin(until: { park.parked != nil }, limit: 3)
        expect(park.parked != nil, "the reinstall is mid-install (its first design's list is pending)")
        expect(!model.busy, "a reinstall alone does not show the playout busy")
        var shown: GraphicsOperationResult?
        let title = GraphicsPlayout.entry(for: rows[1])
        Task { @MainActor in shown = await model.playout.show(title) }
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        expect(model.busy, "the operator's Show is waiting (busy)")
        park.parked?.resume()
        park.parked = nil
        spin(until: { shown != nil }, limit: 5)
        _ = awaitMain(5) { await model.designResync?.value; return true }
        expectEqual(shown?.accepted, true, "the operator's Show is accepted")
        let t = core.transcript
        guard let first = t.firstIndex(where: { $0.hasPrefix("graphics-instance-") }), first > 0 else {
            expect(false, "the Show sent nothing: \(t)"); return
        }
        expectEqual(t[..<first].filter { $0 == "graphics-template-install" }.count, 1,
                    "the Show waits for the one install in flight, not the whole reinstall (\(t))")
        expectEqual(t[first - 1], "graphics-template-list", "and never lands inside an install, between its install and list")
        expectEqual(installedIds(core).count, 4, "the reinstall finishes after the Show")
    }

    /// Review round 1, IMPORTANT 1: a dead bridge (or a core change) stops the loop; the rest would
    /// only fail the same way, each after a bridge timeout.
    @MainActor
    static func testDesignResyncStopsAtATransportFailure() {
        let (model, core) = resyncModel()
        core.failTransport = { $0["type"] as? String == "graphics-template-install" }
        restartCore(model, core, epoch: "epoch-2")
        expectEqual(installedIds(core).count, 1, "one attempt, then the reinstall stops")
        expect(model.status.hasPrefix("Graphics designs: \(GraphicsGateway.transportFailedPrefix)"), "named on the status line (\(model.status))")
    }

    /// Review round 1, IMPORTANT 2: the first lower-third Show after a restart adopts "instances"
    /// authority. That adopt must go through the playout gate, or it can take the single-slot gateway
    /// while the reinstall's next install is sent, which then refuses "graphics-command-overloaded"
    /// and is never retried this core generation.
    @MainActor
    static func testDesignResyncLowerThirdAdoptDuringReinstall() {
        let (model, core) = resyncModel()
        var rows = model.document.playlist
        rows[0].playOutAfterMs = 0   // no auto-out Hide joining the race
        model.document.playlist = rows
        let park = parkNextSleep(model, core)
        beginRestart(model, core, epoch: "epoch-2")
        spin(until: { park.parked != nil }, limit: 3)
        // From the Show's on-air trigger on, park every send: whichever command reaches the gateway
        // next holds its single slot until the test lets go.
        let base = core.react
        core.react = { [weak core] command, node in
            let outcome = base(command, &node)
            let g = command["graphics"] as? JSONObject ?? [:]
            if command["type"] as? String == "graphics-instance-trigger", g["action"] as? String == "show" { core?.hold = true }
            return outcome
        }
        var result: GraphicsOperationResult?
        let guest = GraphicsPlayout.entry(for: model.document.playlist[0])
        Task { @MainActor in
            result = await model.playout.applyLowerThirdIntent(visible: true, subject: guest, authorityIsInstances: false) {
                await model.adoptInstancesAuthority()
            }
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        park.parked?.resume()
        park.parked = nil
        spin(until: { core.heldCount > 0 }, limit: 3)
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))   // the other contender reaches the gateway
        core.hold = false
        core.releaseAll()
        spin(until: { result != nil }, limit: 5)
        _ = awaitMain(5) { await model.designResync?.value; return true }
        expectEqual(result?.accepted, true, "the lower third is shown")
        expectEqual(result?.reason, "Shown; lower thirds now follow the Graphics playlist.", "and the adopt was accepted")
        expectEqual(installedIds(core).count, 4, "every design the show uses still reaches the new core (\(installedIds(core)); \(core.transcript); status: \(model.status))")
        expect(!model.status.hasPrefix("Graphics designs:"), "no reinstall failure (\(model.status))")
    }
}

@MainActor
final class ResyncPark {
    var armed = true
    var parked: CheckedContinuation<Void, Never>?
}
