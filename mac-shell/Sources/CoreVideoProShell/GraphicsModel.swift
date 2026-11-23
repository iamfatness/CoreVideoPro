// GraphicsModel — the ONE graphics sub-model on AppModel (spec 5.1: no graphics
// behaviour as AppModel methods, the Windows strangler rule on the Mac god
// object). Owns the decoded graphics node, the capability gate, the gateway,
// the playout and the On Air strip, and the operator status line (spec 5.4
// degraded states).

import Foundation
import SwiftUI

@MainActor
final class GraphicsModel: ObservableObject {
    static let busyText = "Graphics busy, try again"
    static let restartedText = "Media core restarted: graphics cleared"
    static let needsNewerCoreText = "Graphics need a newer media core"

    /// The core advertises `graphics-instances` (Windows HasAvailableCapability).
    @Published private(set) var capable = false
    @Published private(set) var state: GraphicsState?
    @Published private(set) var strip = GraphicsStrip.empty
    /// The status line: the last result's reason (a refusal verbatim), the busy
    /// text, or the restart notice.
    @Published var status = ""
    @Published private(set) var busy = false

    /// True while a core process is up (set by AppModel from BridgeStatus).
    var coreRunning: () -> Bool
    /// The show document (playlist rows, template pins; everything else verbatim).
    @Published var document = GraphicsDocument()
    @Published var documentReadOnly = false
    @Published var documentStatus = ""
    @Published var documentLoaded = false
    /// Operate's selection, as Windows `_selectedKey`: "", a row id, or "rowId|sourceId" for a
    /// Guest or names-list line (GraphicsPlayoutControl.Select).
    @Published var selectedKey = ""
    /// Set by every accepted lower-third Hide, cleared by every accepted Show
    /// (spec 5.5). Nothing reads it while Auto is retired on Mac.
    @Published var lowerThirdAutomationSuppressed = false
    /// Show Input slots as playout sees them (AppModel supplies them).
    var rosterSlots: () -> [GraphicsRosterSlot] = { [] }
    let documentStore: GraphicsDocumentStore
    let documentQueue = DispatchQueue(label: "us.iamfatness.corevideopro.graphics-document")
    var saveTask: Task<Void, Never>?
    /// Debounce for playlist saves (tests set 0).
    var saveDelayNs: UInt64 = 300_000_000

    private(set) var gateway: GraphicsGateway!
    private(set) var playout: GraphicsPlayout!
    /// The private social review queue and its sole path to air (gateway.invokeSocial).
    private(set) var social: SocialPlayout!
    private var lastNode: NSDictionary?

    init(coreRunning: @escaping () -> Bool, send: @escaping (JSONObject) async throws -> JSONObject,
         documentPath: String = GraphicsDocumentStore.defaultPath) {
        self.coreRunning = coreRunning
        documentStore = GraphicsDocumentStore(path: documentPath)
        gateway = GraphicsGateway(capable: { [weak self] in self?.coreRunning() == true && self?.capable == true },
                                  observe: { [weak self] in self?.state }, send: send)
        playout = GraphicsPlayout(invoke: { [weak self] action, body in
            guard let self else { return .refused(GraphicsCommandRequest.unavailable) }
            return await self.gateway.invoke(action, body)
        }, observe: { [weak self] in self?.state })
        social = SocialPlayout(gateway: gateway)
        playout.onChange = { [weak self] in self?.playoutChanged() }
        // A command's own reply carries the freshest snapshot: observe it at once.
        gateway.onSnapshot = { [weak self] in self?.observe(snapshot: $0) }
        playout.rows = { [weak self] in self?.document.playlist ?? [] }
        playout.pins = { [weak self] in self?.document.templates ?? [] }
    }

    var available: Bool { gateway.available }

    /// Which pane the Graphics tab shows: Operate|Design, or today's pane with the
    /// "newer media core" line when graphics is unavailable.
    var unavailableReason: String? { available ? nil : Self.needsNewerCoreText }

    /// `profile` capability, per NativeMediaCoreProfile.HasAvailableCapability:
    /// capabilityStates wins when present, else the flat capabilities list.
    nonisolated static func capability(_ name: String, in snapshot: JSONObject) -> Bool? {
        guard let profile = snapshot["profile"] as? JSONObject else { return nil }
        if let states = profile["capabilityStates"] as? JSONObject, !states.isEmpty {
            return ((states[name] as? JSONObject)?["state"] as? String)?.lowercased() == "available"
        }
        if let list = profile["capabilities"] as? [String] { return list.contains { $0.lowercased() == name } }
        return nil
    }

    /// Every snapshot (sync replies and the gateway's own replies).
    func observe(snapshot: JSONObject) {
        if let capable = Self.capability(GraphicsGateway.capability, in: snapshot), capable != self.capable {
            self.capable = capable
            resyncDesignsIfDue()   // the graphics node may be unchanged (and skipped below)
        }
        guard let node = snapshot["graphics"] as? JSONObject else { return }
        let dictionary = node as NSDictionary
        if let lastNode, lastNode.isEqual(dictionary) { return }   // decode only when it changed (spec 5.3)
        lastNode = dictionary
        let next = GraphicsState(node: node)
        let previousEpoch = state?.coreEpoch ?? ""
        state = next
        if !previousEpoch.isEmpty, previousEpoch != next.coreEpoch {
            playout.epochChanged()
            status = Self.restartedText
            designResyncPending = true
        }
        reproject()
        resyncDesignsIfDue()
    }

    /// A new core process: its graphics are gone until its first snapshot says otherwise.
    func coreGenerationChanged() {
        capable = false
        if state != nil { status = Self.restartedText }
        playout.epochChanged()
        state = nil
        lastNode = nil
        reproject()
        designResyncPending = true   // GraphicsDesignResync.Arm: handled when the new core's graphics state arrives
    }

    // ── design reinstall per core generation (spec 5.3) ──────────────────────

    /// GraphicsDesignResync: armed on every core generation, fires exactly once per armed new
    /// epoch with graphics state present.
    private var designResyncPending = false
    private var designResyncEpoch = ""
    /// The running reinstall (tests await it).
    private(set) var designResync: Task<Void, Never>?

    func resyncDesignsIfDue() {
        // `available` too: a snapshot can carry the graphics node before the capability is
        // confirmed, and a reinstall sent then is refused by the gateway and never retried.
        // `documentLoaded` too: at launch the show loads off the main thread, and a resync run
        // before it would see no references and spend this core's one attempt on nothing.
        guard designResyncPending, available, documentLoaded, let epoch = state?.coreEpoch, !epoch.isEmpty, epoch != designResyncEpoch else { return }
        designResyncPending = false
        designResyncEpoch = epoch
        let references = document.referencedTemplates, designs = document.designPins
        guard references.contains(where: { GraphicsPlayout.isDesignTemplateId($0.templateId) }) else { return }
        designResync = Task { @MainActor [weak self] in
            guard let self else { return }
            let result = await self.playout.ensureDesignsInstalled(references, designs: designs)
            // A core that restarted again mid-way re-arms and reports for itself.
            if !result.accepted, self.state?.coreEpoch == epoch {
                // The status line is overwritten by the next report(); the log keeps it.
                ShellLog.write("graphics: design reinstall failed epoch=\(epoch): \(result.reason)")
                self.status = "Graphics designs: " + result.reason
            }
        }
    }

    func reproject() {
        let next = state.map { GraphicsStrip.project($0, rows: document.playlist.map(\.stripRow)) } ?? .empty
        if next != strip { strip = next }
    }

    // ── strip actions ────────────────────────────────────────────────────────

    func hide(_ instanceId: String) { perform { await $0.hide(instanceId) } }
    func clearCue(_ instanceId: String) { perform { await $0.clearCue(instanceId) } }
    func clearAll() { perform { await $0.clearAll() } }
    func clearCued() { perform { await $0.clearCued() } }

    /// Runs one playout action and puts its outcome on the status line.
    @discardableResult
    func perform(_ action: @escaping (GraphicsPlayout) async -> GraphicsOperationResult) -> Task<GraphicsOperationResult, Never> {
        Task { @MainActor in
            let result = await action(playout)
            report(result)
            return result
        }
    }

    func report(_ result: GraphicsOperationResult) {
        status = result.reason == "graphics-command-overloaded" ? Self.busyText : result.reason
    }

    private func playoutChanged() {
        if busy != playout.busy { busy = playout.busy }
    }
}
