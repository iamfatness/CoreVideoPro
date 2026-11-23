// GraphicsPlayout — playlist playout over native graphics instances (graphics
// G1-G3). Port of MC/Services/GraphicsPlayoutService.cs.
//
// G1 (this file's first slice) carries the plumbing and the strip's safety
// actions: Hide (:309-316), Clear cue (:318-324), Clear all (:178-185) and Clear
// cued (:186-194). Every operation runs one at a time through `run` (:359-370)
// and every command goes out through `send` (:374-379), which stamps the
// identity (schemaVersion 1, clientId graphics-playout, a fresh requestId and
// the epoch the operation STARTED on). A core whose epoch moved mid-operation
// refuses the rest of the sequence: "no automatic replay" (:372-373).
//
// ON-AIR TRUTH IS ONLY THE CORE'S INSTANCES. A command's acceptance never puts
// a chip on the strip; the next snapshot does.

import Foundation

struct GraphicsPlayoutFailure: Error { let message: String }

/// A roster slot as playout sees it (GraphicsRosterSlot): slot number, canonical
/// source id (nil when unassigned) and the derived name.
struct GraphicsRosterSlot: Equatable {
    var slotNumber: Int
    var sourceId: String?
    var derivedName: String
}

/// A playable item (GraphicsPlaylistEntry): one row (sourceId ""), one guest of a
/// Guest row, or one names-list line (sourceId "variant:<id>"). Never persisted.
struct GraphicsPlaylistEntry: Equatable {
    static let variantPrefix = "variant:"
    var rowId: String
    var sourceId: String
    var bindings: [String: String]
    var slotNumber: Int?

    init(rowId: String, sourceId: String, name: String, title: String, slotNumber: Int? = nil) {
        self.rowId = rowId; self.sourceId = sourceId; self.bindings = ["name": name, "title": title]; self.slotNumber = slotNumber
    }
    var isVariant: Bool { sourceId.hasPrefix(Self.variantPrefix) }
    var label: String { bindings["name"] ?? "" }
    var title: String { bindings["title"] ?? "" }
    var instanceId: String { GraphicsPlayoutIds.instanceId(rowId: rowId, sourceId: sourceId) }
}

@MainActor
final class GraphicsPlayout {
    static let clientId = "graphics-playout"
    static let unavailableMessage = "Native Graphics is unavailable."
    static let coreChangedMessage = "Native core changed. Graphics playout cancelled; no automatic replay."

    /// The one gateway call (GraphicsGateway.invoke).
    let invoke: (String, JSONObject) async -> GraphicsOperationResult
    /// The latest graphics node.
    let observe: () -> GraphicsState?
    /// Injected sleep (nanoseconds) for the cue wait and auto-out (plan D10).
    var sleep: (UInt64) async -> Void = { try? await Task.sleep(nanoseconds: $0) }
    var newRequestId: () -> String = { "playout-" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "") }
    /// Called whenever busy or status changes.
    var onChange: () -> Void = {}
    /// The playlist rows and template pins of the loaded show document.
    var rows: () -> [GraphicsRow] = { [] }
    var pins: () -> [GraphicsTemplatePin] = { [] }
    /// Every template version the show document references (eviction keeps them; GraphicsModel supplies it).
    var references: () -> [GraphicsTemplateReference] = { [] }
    /// Clock for the cue and install budgets (tests advance it from `sleep`).
    var now: () -> Date = { Date() }
    /// Auto-out timers by instance id; the token makes a late fire a no-op.
    private(set) var autoOut: [String: (token: UUID, task: Task<Void, Never>)] = [:]

    private(set) var pending = 0
    var busy: Bool { pending > 0 }
    private(set) var status = ""
    /// The epoch the running operation started on ("" between operations).
    var epoch = ""
    private var gateHeld = false
    private var gateWaiters: [CheckedContinuation<Void, Never>] = []

    init(invoke: @escaping (String, JSONObject) async -> GraphicsOperationResult, observe: @escaping () -> GraphicsState?) {
        self.invoke = invoke
        self.observe = observe
    }

    // ── strip actions (G1) ────────────────────────────────────────────────

    func hide(_ instanceId: String) async -> GraphicsOperationResult { await run { try await self.hideCore(instanceId) } }
    func clearCue(_ instanceId: String) async -> GraphicsOperationResult { await run { try await self.uncueCore(instanceId) } }

    /// Hide every instance on Program or still show-pending (:178-185).
    func clearAll() async -> GraphicsOperationResult {
        await run {
            let state = try self.available()
            var result = GraphicsOperationResult(accepted: true, reason: "Nothing is on Program.")
            for instance in state.instances where instance.isOnProgram || instance.showPending {
                result = try await self.hideCore(instance.instanceId)
                if !result.accepted { return result }
            }
            // Windows reports "All graphics hidden." for any accepted run, even an empty one (:184).
            if result.accepted { result.reason = "All graphics hidden." }
            return result
        }
    }

    /// Uncue every cued instance except a social card (:186-194).
    func clearCued() async -> GraphicsOperationResult {
        await run {
            let state = try self.available()
            var result = GraphicsOperationResult(accepted: true, reason: "Preview is already clear.")
            for instance in state.instances where instance.isOnPreview && !instance.instanceId.hasPrefix(GraphicsStrip.socialPrefix) {
                result = try await self.uncueCore(instance.instanceId)
                if !result.accepted { return result }
            }
            if result.accepted { result.reason = "Preview cleared." }   // as Windows (:193)
            return result
        }
    }

    // ── cores (callers already hold the gate) ────────────────────────────────

    func hideCore(_ instanceId: String) async throws -> GraphicsOperationResult {
        cancelAutoOut(instanceId)
        let state = try available()
        epoch = state.coreEpoch
        guard let instance = state.instance(instanceId) else { return refuse("That graphic no longer exists.") }
        guard instance.programRevision > 0 || instance.showPending else { return refuse("That graphic is not on Program.") }
        return try await send("graphics.instance.trigger",
                              ["instanceId": instanceId, "expectedRevision": instance.revision, "action": "hide"])
    }

    func uncueCore(_ instanceId: String) async throws -> GraphicsOperationResult {
        let state = try available()
        epoch = state.coreEpoch
        guard let instance = state.instance(instanceId) else { return refuse("That graphic no longer exists.") }
        guard instance.isOnPreview else { return refuse("That graphic is not on Preview.") }
        return try await send("graphics.instance.trigger",
                              ["instanceId": instanceId, "expectedRevision": instance.revision, "action": "uncue"])
    }

    // ── auto-out (:326-356) ───────────────────────────────────────────────────

    func armAutoOut(_ row: GraphicsRow, _ instanceId: String, epoch: String) {
        cancelAutoOut(instanceId)
        guard row.playOutAfterMs > 0 else { return }
        let token = UUID(), afterMs = UInt64(row.playOutAfterMs)
        let task = Task { @MainActor [weak self] in
            guard let self else { return }
            await self.sleep(afterMs * 1_000_000)
            guard !Task.isCancelled, self.autoOut[instanceId]?.token == token else { return }
            self.autoOut[instanceId] = nil
            guard let state = self.observe(), state.coreEpoch == epoch,
                  let instance = state.instance(instanceId), instance.programRevision > 0 || instance.showPending else { return }
            _ = await self.run { try await self.hideCore(instanceId) }
        }
        autoOut[instanceId] = (token, task)
    }

    func cancelAutoOut(_ instanceId: String) {
        autoOut.removeValue(forKey: instanceId)?.task.cancel()
    }

    func cancelAllAutoOut() {
        let all = autoOut
        autoOut = [:]
        all.values.forEach { $0.task.cancel() }
    }

    /// A new core epoch: drop every local timer (:126). In-flight sequences
    /// refuse on their own at the next `current(epoch)` check.
    func epochChanged() { cancelAllAutoOut() }

    // ── playlist operations (G2) ────────────────────────────────────────────

    nonisolated static let defaultMotion: JSONObject = [
        "layers": "own",
        "in": ["type": "fade", "direction": "left", "easing": "ease-out", "durationMs": 250],
        "out": ["type": "fade", "direction": "left", "easing": "ease-in", "durationMs": 200],
    ]

    func row(_ id: String) -> GraphicsRow? { rows().first { $0.id == id } }
    nonisolated static func entry(for row: GraphicsRow) -> GraphicsPlaylistEntry { GraphicsPlaylistEntry(rowId: row.id, sourceId: "", name: row.name, title: "") }

    /// One entry per bound roster slot, in slot order, de-duplicated by source id
    /// (GuestEntries, :100-111). `text` names a source (owner decision: the roster
    /// or device name, an empty title, until the Sources rebuild).
    nonisolated static func guestEntries(_ row: GraphicsRow, _ slots: [GraphicsRosterSlot],
                             text: (String, String) -> (name: String, title: String)) -> [GraphicsPlaylistEntry] {
        var seen = Set<String>(), entries: [GraphicsPlaylistEntry] = []
        for slot in slots.sorted(by: { $0.slotNumber < $1.slotNumber }) {
            guard let sourceId = slot.sourceId, !sourceId.isEmpty, seen.insert(sourceId).inserted else { continue }
            let copy = text(sourceId, slot.derivedName)
            entries.append(GraphicsPlaylistEntry(rowId: row.id, sourceId: sourceId, name: copy.name, title: copy.title, slotNumber: slot.slotNumber))
        }
        return entries
    }

    func ensure(_ entry: GraphicsPlaylistEntry) async -> GraphicsOperationResult { await run { try await self.ensureCore(entry) } }
    func cue(_ entry: GraphicsPlaylistEntry) async -> GraphicsOperationResult { await run { try await self.cueCore(entry) } }

    /// Show = Cue, then trigger show FROM THE CUE at the cue's revision (:161-174).
    func show(_ entry: GraphicsPlaylistEntry) async -> GraphicsOperationResult {
        await run {
            let cued = try await self.cueCore(entry)
            guard cued.accepted else { return cued }
            // A names list is one lower third with many lines: showing a line takes the
            // row's other lines off Program first (PlayInAsync, :164-170).
            if entry.isVariant, let list = self.row(entry.rowId), list.hasVariants {
                let stripRows = self.rows().map(\.stripRow)
                let siblings = (self.observe()?.instances ?? []).filter {
                    $0.isOnProgram && $0.instanceId != cued.instanceId && GraphicsStrip.row(of: $0.instanceId, in: stripRows)?.id == list.id
                }
                for sibling in siblings {
                    let hidden = try await self.hideCore(sibling.instanceId)
                    if !hidden.accepted { return self.refuse("Could not take the previous name off: " + hidden.reason) }
                }
            }
            var shown = try await self.send("graphics.instance.trigger", ["instanceId": cued.instanceId, "expectedRevision": cued.currentRevision,
                                                                          "target": "cue", "action": "show"])
            if shown.accepted, let row = self.row(entry.rowId) { self.armAutoOut(row, cued.instanceId, epoch: self.epoch) }
            if shown.accepted { shown.instanceId = cued.instanceId; shown.reason = "Shown from Preview." }
            return shown
        }
    }

    func ensureCore(_ entry: GraphicsPlaylistEntry) async throws -> GraphicsOperationResult {
        guard let row = row(entry.rowId) else { return refuse("That playlist row no longer exists.") }
        if row.needsDataPlayback { return refuse(Self.unsupportedDataText(row.name)) }
        var state = try available()
        let epoch = state.coreEpoch
        self.epoch = epoch
        let image = row.image
        let templateId = (image?["templateId"] as? String) ?? row.templateId
        let version = (image?["templateVersion"] as? NSNumber)?.intValue ?? row.templateVersion
        if templateId.isEmpty { return refuse("\(row.name) has no graphic yet. Choose an image or template for it first.") }
        let pin = pins().first { $0.templateId == templateId && $0.templateVersion == version }
        var installed = state.template(templateId, version)
        if installed == nil, let pin {
            let (landed, refusal) = try await install(pin, epoch: epoch)
            if let refusal { return refusal }
            installed = landed
        }
        guard let installed else {
            return refuse("\(row.name): template \(templateId) v\(version) is not installed in this core. Open it in Design to import it.")
        }
        if let pin, pin.manifestHash != installed.manifestHash {
            return refuse("\(row.name): the installed template differs from its saved pin. Program is unchanged.")
        }
        state = try current(epoch)
        let declarative = installed.runtime == "declarative"
        let values = Self.cueFields(row, entry, declarative: declarative, descriptors: installed.fields)
        let identity = entry.sourceId.isEmpty ? nil
            : state.sources.first { $0.videoOn && $0.identity?.sourceId == entry.sourceId }?.identity
        let id = entry.instanceId
        let motion: JSONObject
        if var rowMotion = row.motion { rowMotion["layers"] = "whole"; motion = Self.normalizedMotion(rowMotion) }
        else if Self.isDesignTemplateId(templateId), var own = installed.animations { own["layers"] = "own"; motion = Self.normalizedMotion(own) }
        else { motion = Self.defaultMotion }
        let placement: Any = (row.placement ?? installed.defaultPlacement).map { $0 as Any } ?? NSNull()
        guard let existing = state.instance(id) else {
            var body: JSONObject = ["instanceId": id, "templateId": templateId, "templateVersion": version, "scope": "global", "sceneId": "",
                                    "fields": values, "style": GraphicsStyleWire.wire(row.style, declarative: !Self.curated(templateId)),
                                    "animations": motion, "placement": placement, "image": image ?? NSNull()]
            if let identity { body["binding"] = identity.json }
            var created = try await send("graphics.instance.create", body)
            guard created.accepted else { return created }
            guard created.currentRevision >= 1, created.instanceId == id else { return refuse("The native create result has no matching revision identity.") }
            var fresh = (try current(epoch)).instance(id) ?? GraphicsInstance()
            if fresh.instanceId.isEmpty { fresh.instanceId = id; fresh.revision = created.currentRevision }
            try await configureClock(row, fresh)
            created.reason = "Ready."
            return created
        }
        try await configureClock(row, existing)
        // Only the cue copy is refreshed; a Program copy changes on the next Show, never here.
        let designChanged = existing.templateVersion != version || !Self.sameMotion(existing.animations, motion)
        if !designChanged && Self.sameFields(existing.fields, values) && (identity == nil || existing.binding == identity) {
            return GraphicsOperationResult(instanceId: id, currentRevision: existing.revision, accepted: true, reason: "Ready.")
        }
        var update: JSONObject = ["instanceId": id, "expectedRevision": existing.revision, "target": "cue", "fields": values]
        if designChanged { update["templateVersion"] = version; update["animations"] = motion }
        if let identity { update["binding"] = identity.json }
        var updated = try await send("graphics.instance.update", update)
        guard updated.accepted else { return updated }
        let revision = updated.currentRevision > 0 ? updated.currentRevision : (try current(epoch)).instance(id)?.revision ?? 0
        guard revision >= 1 else { return refuse("The native update result has no revision.") }
        updated.instanceId = id; updated.currentRevision = revision; updated.reason = "Ready."
        return updated
    }

    /// Cue with rideTake, then wait for the cue revision to land (:280-307): 5 s
    /// budget polled every 50 ms on the injected sleep.
    func cueCore(_ entry: GraphicsPlaylistEntry) async throws -> GraphicsOperationResult {
        let ensured = try await ensureCore(entry)
        guard ensured.accepted else { return ensured }
        let id = ensured.instanceId, revision = ensured.currentRevision, epoch = self.epoch
        let cued = try await send("graphics.instance.cue", ["instanceId": id, "expectedRevision": revision, "rideTake": true])
        guard cued.accepted else { return cued }
        let until = now().addingTimeInterval(5)
        while true {
            guard let current = try current(epoch).instance(id), current.revision == revision else {
                return refuse("The graphic changed while cueing. Retry explicitly.")
            }
            if current.cueRevision == revision && !current.cuePending { break }
            if now() >= until { return refuse("The graphic is still preparing. Retry when ready.") }
            await sleep(50_000_000)
            try Task.checkCancellation()
        }
        return GraphicsOperationResult(instanceId: id, currentRevision: revision, accepted: true,
                                       reason: "Cued to Preview; it rides the next Take.")
    }

    /// Install a pinned template, then list until it appears, its install fails,
    /// or 5 s pass (GraphicsTemplateInstall.AttemptAsync).
    static let libraryFull = "The graphics library is full. Delete designs you no longer use, or remove graphics from the playlist."

    /// GraphicsTemplateInstall.RunAsync: one attempt; when the library is full and an eviction of unused
    /// design versions removed at least one, one more attempt (plan D6; data designs and pin reinstalls alike).
    func install(_ pin: GraphicsTemplatePin, epoch: String) async throws -> (GraphicsTemplate?, GraphicsOperationResult?) {
        let attempt = try await installAttempt(pin, epoch: epoch)
        if attempt.1?.reason == Self.libraryFull, try await evictUnusedDesigns(epoch: epoch) > 0 {
            return try await installAttempt(pin, epoch: epoch)
        }
        return attempt
    }

    /// GraphicsDesignEviction.InUse: document references plus every live instance's draft, cue and
    /// program versions and images (no editor draft or unsaved edits on the Mac until G8).
    nonisolated static func inUse(references: [GraphicsTemplateReference], state: GraphicsState) -> Set<String> {
        var keep = Set(references.map { "\($0.templateId)|\($0.templateVersion)" })
        for instance in state.instances {
            for version in [instance.templateVersion, instance.cueTemplateVersion, instance.programTemplateVersion] where version > 0 {
                keep.insert("\(instance.templateId)|\(version)")
            }
            for image in [instance.image, instance.cueImage, instance.programImage].compactMap({ $0 }) {
                keep.insert("\(image.templateId)|\(image.templateVersion)")
            }
        }
        return keep
    }

    /// GraphicsDesignEviction.EvictUnusedAsync: removes every installed DESIGN version nothing needs.
    func evictUnusedDesigns(epoch: String) async throws -> Int {
        let state = try current(epoch)
        let keep = Self.inUse(references: references(), state: state)
        var removed = 0
        for template in state.templates where Self.isDesignTemplateId(template.templateId)
            && !keep.contains("\(template.templateId)|\(template.templateVersion)") {
            if try await removeTemplate(template.templateId, template.templateVersion).accepted { removed += 1 }
        }
        return removed
    }

    /// Native remove; template-library-busy never blocks, so retry it briefly (3 x 100 ms, RemoveAsync).
    func removeTemplate(_ templateId: String, _ version: Int) async throws -> GraphicsOperationResult {
        var attempt = 0
        while true {
            let result = try await send("graphics.template.remove", ["templateId": templateId, "templateVersion": version])
            if result.accepted || result.reason != "template-library-busy" || attempt == 3 { return result }
            attempt += 1
            await sleep(100_000_000)
        }
    }

    func installAttempt(_ pin: GraphicsTemplatePin, epoch: String) async throws -> (GraphicsTemplate?, GraphicsOperationResult?) {
        guard let manifestData = try? JSONSerialization.data(withJSONObject: pin.definition, options: [.sortedKeys]),
              let manifest = String(data: manifestData, encoding: .utf8) else { return (nil, refuse("The saved template pin is unreadable.")) }
        let requestId = "install-" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "")
        let imported = try await send("graphics.template.install", ["manifest": manifest, "packageRoot": pin.packageRoot, "requestId": requestId])
        guard imported.accepted else { return (nil, imported) }
        let limit = now().addingTimeInterval(5)
        repeat {
            await sleep(50_000_000)
            let listed = try await send("graphics.template.list", [:])
            guard listed.accepted else { return (nil, listed) }
            let state = try current(epoch)
            if let template = state.template(pin.templateId, pin.templateVersion) { return (template, nil) }
            if let failed = state.templateInstalls.first(where: { $0.requestId == requestId && $0.state == "failed" }) {
                let reason = failed.reason == "template-library-capacity"
                    ? Self.libraryFull
                    : "The template could not be installed (\(failed.reason))."
                return (nil, GraphicsOperationResult(requestId: requestId, accepted: false, reason: reason))
            }
        } while now() < limit
        return (nil, nil)
    }

    static let designInstallTimeout = "The design did not finish installing. Check the graphics library and try again."

    /// Spec 5.3 / GraphicsDesignService.EnsureInstalledForPlaylistAsync (:179-190): after a core
    /// restart, install the DESIGN template versions the show references that this core lacks,
    /// from the pin, else the show's design entry. Non-design pins stay lazy (ensureCore), as on
    /// Windows. Not an operator action: no busy flag, no status (the model reports a failure).
    /// A full library evicts unused design versions and retries once (install, plan D6).
    ///
    /// The gate is held for ONE design at a time (review round 1). Windows runs this under the
    /// design service's own lock, but both platforms share one single-slot gateway, so a separate
    /// lock would only turn a concurrent Show into "Graphics busy"; holding the playout gate across
    /// every design instead blocked Show/Hide/F-keys for up to 5 s per design on a stuck core. Per
    /// design, an operator action queued behind the reinstall waits for at most one install. The
    /// loop stops at a transport failure or a core change: the rest would only fail the same way.
    func ensureDesignsInstalled(_ references: [GraphicsTemplateReference], designs: [GraphicsTemplatePin]) async -> GraphicsOperationResult {
        guard let epoch = observe()?.coreEpoch, !epoch.isEmpty else { return refuse(Self.unavailableMessage) }
        var problems: [String] = []
        for reference in references where Self.isDesignTemplateId(reference.templateId) {
            let (problem, abort) = await installDesign(reference, designs: designs, epoch: epoch)
            if let problem, !problems.contains(problem) { problems.append(problem) }
            if abort { break }
        }
        return problems.isEmpty ? GraphicsOperationResult(accepted: true, reason: "Designs in use are installed.")
                                : refuse(problems.joined(separator: " "))
    }

    /// One design under one gate hold. `abort` when the rest of the reinstall cannot succeed.
    private func installDesign(_ reference: GraphicsTemplateReference, designs: [GraphicsTemplatePin],
                               epoch: String) async -> (problem: String?, abort: Bool) {
        await acquire()
        defer { self.epoch = ""; release() }
        do {
            self.epoch = epoch
            if try current(epoch).template(reference.templateId, reference.templateVersion) != nil { return (nil, false) }
            let matches = { (pin: GraphicsTemplatePin) in
                pin.templateId == reference.templateId && pin.templateVersion == reference.templateVersion
            }
            guard let pin = pins().first(where: matches) ?? designs.first(where: matches) else { return (nil, false) }
            let (installed, refusal) = try await install(pin, epoch: epoch)
            if installed != nil { return (nil, false) }
            guard let refusal else { return (Self.designInstallTimeout, false) }
            return (refusal.reason, refusal.reason.hasPrefix(GraphicsGateway.transportFailedPrefix))
        } catch let failure as GraphicsPlayoutFailure {
            return (failure.message, true)   // the core changed or graphics went away
        } catch {
            return (error.localizedDescription, true)
        }
    }

    /// Runs `body` holding the playout gate, without the busy flag or a status: for a gateway
    /// command that is part of an operator action but issued after its playout step returned.
    func gated<T>(_ body: () async -> T) async -> T {
        await acquire()
        defer { release() }
        return await body()
    }

    /// Countdown transport (ClockAsync, GraphicsPlayoutService.cs:420-427): start | pause | resume | reset
    /// on the row's instance. Never shows or hides the graphic; refused when the row has no instance yet.
    func clock(_ entry: GraphicsPlaylistEntry, action: String) async -> GraphicsOperationResult {
        await run {
            guard ["start", "pause", "resume", "reset"].contains(action) else {
                return self.refuse("Clock action must be start, pause, resume or reset.")
            }
            let id = entry.instanceId
            guard let instance = self.observe()?.instance(id) else {
                return self.refuse("That row is not on Preview or Program yet. Cue or Show it first.")
            }
            var result = try await self.send("graphics.clock.control", ["instanceId": id, "expectedRevision": max(1, instance.revision), "action": action])
            if result.accepted { result.instanceId = id; result.reason = "Clock " + action + "." }
            return result
        }
    }

    /// ConfigureClock (:256-266): apply the row's clock when it differs from what the
    /// core already has. Non-fatal: a template without a clock refuses and the graphic still plays.
    func configureClock(_ row: GraphicsRow, _ instance: GraphicsInstance) async throws {
        guard let clock = row.clock, GraphicsRow.validClock(clock) else { return }
        if let requested = instance.clockRequestedConfiguration, Self.sameClock(requested, clock) { return }
        _ = try await send("graphics.clock.control", [
            "instanceId": instance.instanceId, "expectedRevision": max(1, instance.revision), "action": "configure",
            "configuration": Self.clockWire(clock), "expectedClockRevision": max(1, instance.clockConfigurationRevision),
        ])
    }

    // ── pure helpers ─────────────────────────────────────────────────────────

    nonisolated static func unsupportedDataText(_ rowName: String) -> String {
        "\(rowName): crawls and live data feeds arrive in a later Mac release, so this row cannot play here yet."
    }

    /// One entry per names-list line, in list order (VariantEntries, :117-119).
    nonisolated static func variantEntries(_ row: GraphicsRow) -> [GraphicsPlaylistEntry] {
        row.variants.compactMap { v in
            guard let id = v["id"] as? String else { return nil }
            return GraphicsPlaylistEntry(rowId: row.id, sourceId: GraphicsPlaylistEntry.variantPrefix + id,
                                         name: v["name"] as? String ?? "", title: v["title"] as? String ?? "")
        }
    }

    /// The line a bare hotkey plays: the one after the last line of this row on
    /// Program (wrapping), else the first (NextVariant, :121-127).
    nonisolated static func nextVariant(_ row: GraphicsRow, onProgramSourceIds: [String]) -> GraphicsPlaylistEntry? {
        let entries = variantEntries(row)
        guard !entries.isEmpty else { return nil }
        let onAir = Set(onProgramSourceIds)
        var current = -1
        for (index, entry) in entries.enumerated() where onAir.contains(entry.sourceId) { current = index }
        return entries[(current + 1) % entries.count]
    }

    /// NativeGraphicsClockConfiguration as Windows serializes it (defaults filled).
    nonisolated static func clockWire(_ clock: JSONObject) -> JSONObject {
        [
            "mode": clock["mode"] as? String ?? "wall", "zone": clock["zone"] as? String ?? "local",
            "format": clock["format"] as? String ?? "hms", "finish": clock["finish"] as? String ?? "hold-zero",
            "offsetMinutes": (clock["offsetMinutes"] as? NSNumber)?.intValue ?? 0,
            "durationSeconds": (clock["durationSeconds"] as? NSNumber)?.intValue ?? 300,
            "restoreRemainingSeconds": clock["restoreRemainingSeconds"] ?? NSNull(),
        ]
    }

    nonisolated static func sameClock(_ a: JSONObject, _ b: JSONObject) -> Bool {
        let x = clockWire(a), y = clockWire(b)
        return ["mode", "zone", "format", "finish", "offsetMinutes", "durationSeconds"].allSatisfy {
            (x[$0] as AnyObject).isEqual(y[$0])
        }
    }

    /// GraphicsMappedValues.ConvertCell for one names-list cell, or nil when the
    /// cell does not fit the field (the row's own value then stands).
    nonisolated static func convertCell(_ field: GraphicsTemplateField, _ cell: String) -> Any? {
        let value: Any?
        switch field.type {
        case "string":
            value = (!field.required || !cell.isEmpty) && cell.utf8.count <= (field.maxLength ?? 512) ? cell : nil
        case "boolean":
            switch cell.trimmingCharacters(in: .whitespaces).lowercased() { case "true": value = true; case "false": value = false; default: value = nil }
        case "number":
            let text = cell.trimmingCharacters(in: .whitespaces)
            if !text.lowercased().contains("x"), let number = Double(text), number.isFinite,
               number >= (field.minimum ?? -1e9), number <= (field.maximum ?? 1e9) { value = number } else { value = nil }
        case "color":
            value = (cell.utf16.count == 7 || cell.utf16.count == 9) && cell.utf16.count <= (field.maxLength ?? 128)
                && cell.hasPrefix("#") && cell.dropFirst().allSatisfy(\.isHexDigit) ? cell : nil
        default:
            value = nil
        }
        guard let value else { return nil }
        return field.choices.isEmpty || field.choices.contains { ($0 as AnyObject).isEqual(value) } ? value : nil
    }

    nonisolated static func curated(_ id: String) -> Bool { ["compact-solid", "minimal-accent", "broadcast"].contains(id) }

    nonisolated static func isDesignTemplateId(_ id: String) -> Bool {
        id.utf16.count <= 64 && id.range(of: #"^design-[a-z0-9]+(-[a-z0-9]+)*-[0-9a-f]{6}$"#, options: .regularExpression) != nil
    }

    /// GraphicsLayoutResolver.CueFields minus the Sources-column mapping (a later
    /// slice): the row's fields, the entry's name/title for a Guest (followSources)
    /// or names-list entry, then only the keys the template takes.
    nonisolated static func cueFields(_ row: GraphicsRow, _ entry: GraphicsPlaylistEntry, declarative: Bool,
                          descriptors: [GraphicsTemplateField]) -> JSONObject {
        var values = row.fields
        if entry.isVariant || (row.followSources && !entry.sourceId.isEmpty) {
            for (key, value) in entry.bindings
            where declarative ? descriptors.contains(where: { $0.id == key && $0.type == "string" }) : (key == "name" || key == "title") {
                values[key] = value
            }
        }
        // GraphicsMappedValues.Apply for a names-list line's cells (Sources columns: a later slice).
        if declarative, entry.isVariant,
           let variant = row.variants.first(where: { GraphicsPlaylistEntry.variantPrefix + ($0["id"] as? String ?? "") == entry.sourceId }) {
            let cells = variant["cells"] as? JSONObject ?? [:]
            for field in descriptors where !["name", "title", "time", "label"].contains(field.id)
                && ["string", "number", "color", "boolean"].contains(field.type) {
                if let cell = cells[field.id] as? String, let value = convertCell(field, cell) { values[field.id] = value }
            }
        }
        for key in values.keys where declarative ? !descriptors.contains(where: { $0.id == key }) : !(key == "name" || key == "title") {
            values.removeValue(forKey: key)
        }
        return values
    }

    /// NativeGraphicsAnimations with every default filled, so two copies compare by value.
    nonisolated static func normalizedMotion(_ m: JSONObject) -> JSONObject {
        func transition(_ t: Any?) -> JSONObject {
            let t = t as? JSONObject ?? [:]
            return ["type": t["type"] as? String ?? "none", "direction": t["direction"] as? String ?? "left",
                    "easing": t["easing"] as? String ?? "linear", "durationMs": (t["durationMs"] as? NSNumber)?.doubleValue ?? 0]
        }
        return ["layers": m["layers"] as? String ?? "own", "in": transition(m["in"]), "out": transition(m["out"])]
    }

    nonisolated static func sameMotion(_ existing: JSONObject?, _ motion: JSONObject) -> Bool {
        (normalizedMotion(existing ?? [:]) as NSDictionary).isEqual(to: normalizedMotion(motion))
    }

    /// Every cue value equals what the instance already carries (SameFields, :380-384).
    nonisolated static func sameFields(_ existing: JSONObject, _ values: JSONObject) -> Bool {
        values.allSatisfy { key, value in
            guard let current = existing[key] else { return false }
            return (current as AnyObject).isEqual(value)
        }
    }

    // ── plumbing ─────────────────────────────────────────────────────────────

    func run(_ action: @escaping () async throws -> GraphicsOperationResult) async -> GraphicsOperationResult {
        pending += 1
        onChange()
        await acquire()
        let result: GraphicsOperationResult
        do { result = try await action() }
        catch let failure as GraphicsPlayoutFailure { result = refuse(failure.message) }
        catch is CancellationError { result = refuse("Graphics playout cancelled. Retry explicitly when ready.") }
        catch { result = refuse(error.localizedDescription) }
        epoch = ""
        release()
        pending -= 1
        status = result.reason
        onChange()
        return result
    }

    func available() throws -> GraphicsState {
        guard let state = observe(), !state.coreEpoch.isEmpty else { throw GraphicsPlayoutFailure(message: Self.unavailableMessage) }
        return state
    }

    func current(_ epoch: String) throws -> GraphicsState {
        guard let state = observe(), state.coreEpoch == epoch else { throw GraphicsPlayoutFailure(message: Self.coreChangedMessage) }
        return state
    }

    func send(_ action: String, _ body: JSONObject) async throws -> GraphicsOperationResult {
        try await sendAs(Self.clientId, action, body)
    }

    /// `send` under another Windows client id (the managed lower third speaks as
    /// "legacy-lower-third", StudioViewModel.GraphicsCompatibility.cs:56).
    func sendAs(_ clientId: String, _ action: String, _ body: JSONObject) async throws -> GraphicsOperationResult {
        let epoch = self.epoch.isEmpty ? try available().coreEpoch : self.epoch
        _ = try current(epoch)
        var body = body
        body["schemaVersion"] = 1
        body["clientId"] = clientId
        if body["requestId"] == nil { body["requestId"] = newRequestId() }
        body["coreEpoch"] = epoch
        return await invoke(action, body)
    }

    func refuse(_ reason: String) -> GraphicsOperationResult { GraphicsOperationResult(accepted: false, reason: reason) }

    private func acquire() async {
        if !gateHeld { gateHeld = true; return }
        await withCheckedContinuation { gateWaiters.append($0) }
    }

    private func release() {
        if gateWaiters.isEmpty { gateHeld = false } else { gateWaiters.removeFirst().resume() }
    }
}

/// NativeGraphicsDraft.StyleWire (MC/Models/LowerThirdPreview.cs:16-24): the row's
/// LowerThirdAppearance with every default filled; a curated (compatibility)
/// template takes all of it, a declarative one only the keys it consumes.
enum GraphicsStyleWire {
    static let defaults: JSONObject = [
        "version": 1, "preset": "compact-solid", "anchor": "lower-left", "fontFamily": "Segoe UI",
        "nameSize": 42, "titleSize": 28, "nameColor": "#F4F7FA", "titleColor": "#44C1A1",
        "backgroundColor": "#0C1118", "accentColor": "#44C1A1", "backgroundOpacity": 0.9, "padding": 20,
        "width": 0.6, "cornerRadius": 8, "safeOffsetX": 0.05, "safeOffsetY": 0.06, "showLogo": true, "logoScale": 1,
    ]
    static let declarativeKeys: Set<String> = ["version", "anchor", "fontFamily", "nameColor", "titleColor", "backgroundColor",
                                                "accentColor", "backgroundOpacity", "safeOffsetX", "safeOffsetY"]

    static func wire(_ style: JSONObject?, declarative: Bool) -> JSONObject {
        var full = defaults
        for (key, value) in style ?? [:] where defaults[key] != nil { full[key] = value }
        return declarative ? full.filter { declarativeKeys.contains($0.key) } : full
    }
}
