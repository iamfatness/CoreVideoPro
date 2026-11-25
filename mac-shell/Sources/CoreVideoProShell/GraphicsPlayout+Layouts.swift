// GraphicsPlayout+Layouts — Shot Layout cue (G4-G7 Task 14). Ports of GraphicsPlayoutService.cs
// CueLayoutAsync / CueLayout (:177-258) and GraphicsPlayoutService.Layouts.cs (PrepareLayout, cancel,
// confirm, IsPreparedLayoutCurrent, ReleaseLayoutCues). A layout without rowSettings is the legacy
// path: scene to Preview, every row cued with rideTake, PlayOutOthers hides. A layout WITH rowSettings
// is guarded: every row is resolved first (GraphicsLayouts.resolve), then each is cued with
// rideTake:false and cueOwner = the attempt id, under one 5 s deadline; any failure releases the
// attempt's own cues (an owned uncue can never clear another attempt's cue). A prepared layout is
// carried by the guarded Take (Task 15). Pinned by contracts/graphics-shell/layout-transcripts.vectors.json.

import Foundation

struct GraphicsLayoutCue: Equatable {
    let instanceId: String
    let cueRevision: Int64
}

struct GraphicsPreparedLayout: Equatable {
    let layout: GraphicsShotLayout
    let coreEpoch: String
    let attemptId: String
    let cues: [GraphicsLayoutCue]
    let previewStamp: String?
}

struct GraphicsLayoutRowProgress: Equatable {
    var name: String
    var dataRevision: Int64?
    var dataReceivedAt: Date?
    var state: String
}

struct GraphicsLayoutProgress: Equatable {
    var layoutId: String
    var state: String
    var reason: String
    var rows: [GraphicsLayoutRowProgress]
}

extension GraphicsPlayout {
    func layout(_ id: String) -> GraphicsShotLayout? { document().shotLayouts.first { $0.id == id } }

    private func reportLayoutProgress(_ progress: GraphicsLayoutProgress?) {
        layoutProgress = progress
        onChange()
    }

    /// CueLayoutAsync: the cue under the one-at-a-time gate; a refused guarded layout leaves its progress
    /// at Blocked with the reason.
    func cueLayout(_ layoutId: String, sceneSelect: @escaping (String) async -> Bool, programSubject: GraphicsPlaylistEntry?,
                   sceneGuests: [GraphicsPlaylistEntry], previewStamp: @escaping () -> String) async -> GraphicsOperationResult {
        let result = await run { try await self.cueLayoutCore(layoutId, sceneSelect: sceneSelect, programSubject: programSubject,
                                                              sceneGuests: sceneGuests, previewStamp: previewStamp) }
        if !result.accepted, !(layout(layoutId)?.rowSettings.isEmpty ?? true), !layoutCancelRequested,
           var progress = layoutProgress, progress.layoutId == layoutId {
            progress.state = "Blocked"; progress.reason = result.reason
            reportLayoutProgress(progress)
        }
        return result
    }

    private func cueLayoutCore(_ layoutId: String, sceneSelect: @escaping (String) async -> Bool, programSubject: GraphicsPlaylistEntry?,
                               sceneGuests: [GraphicsPlaylistEntry], previewStamp: @escaping () -> String) async throws -> GraphicsOperationResult {
        guard let layout = layout(layoutId) else { return refuse(layoutId.isEmpty ? "Choose a shot layout." : "That shot layout no longer exists.") }
        if preparedLayout != nil { return refuse("Cancel the prepared Shot Layout before cueing another.") }
        let guarded = !layout.rowSettings.isEmpty
        if guarded { reportLayoutProgress(GraphicsLayoutProgress(layoutId: layout.id, state: "Checking", reason: "", rows: [])) }
        // Resolve everything before the first side effect so a broken layout is refused whole.
        var entries: [GraphicsPlaylistEntry] = []
        for rowId in layout.rowIds {
            guard let row = row(rowId) else { return refuse("\(layout.name): playlist row '\(rowId)' no longer exists. Edit the layout.") }
            if row.kind != GraphicsRow.guest { entries.append(Self.entry(for: row)); continue }
            let guests = sceneGuests.filter { $0.rowId == row.id }
            if !guests.isEmpty { entries += guests }
            else if var subject = programSubject { subject.rowId = row.id; entries.append(subject) }
            else { return refuse("\(layout.name): \(row.name) needs a Program subject bound to a Show Input. Route a guest first.") }
        }
        if guarded {
            let document = document()
            let state = try available()
            var required = Set(entries.map(\.instanceId))
            if required.count != entries.count { return refuse("\(layout.name): multiple rows resolve to the same graphic instance.") }
            required.formUnion(state.instances.map(\.instanceId))
            if required.count > 8 { return refuse("\(layout.name): the required graphics exceed the core's instance capacity.") }
            let resolved: [GraphicsResolvedRow]
            do { resolved = try entries.map { try GraphicsLayouts.resolve(document: document, state: state, layout: layout, entry: $0) } }
            catch let error as GraphicsDataError { return refuse("\(layout.name): \(error.message)") }
            return try await prepareLayout(layout, entries, resolved, sceneSelect: sceneSelect, previewStamp: previewStamp)
        }
        if !layout.sceneId.isEmpty, !(await sceneSelect(layout.sceneId)) {
            return refuse("\(layout.name): scene '\(layout.sceneId)' is not in this show, so the layout was not cued. Edit the layout to pick a scene.")
        }
        var carried = Set<String>()
        for entry in entries {
            var cued = try await cueCore(entry)
            if !cued.accepted { cued.reason = "\(layout.name): \(cued.reason)"; return cued }
            carried.insert(cued.instanceId)
        }
        if layout.playOutOthers {
            let state = try available()
            for instance in state.instances where (instance.isOnProgram || instance.showPending) && !carried.contains(instance.instanceId) {
                var hidden = try await hideCore(instance.instanceId)
                if !hidden.accepted { hidden.reason = "\(layout.name): \(hidden.reason)"; return hidden }
            }
        }
        func plural(_ n: Int) -> String { n == 1 ? "1 graphic" : "\(n) graphics" }
        let scene = layout.sceneId
        let what = !scene.isEmpty && !entries.isEmpty ? "scene \(scene) and \(plural(entries.count)) on Preview; Take carries both."
            : !scene.isEmpty ? "scene \(scene) on Preview." : !entries.isEmpty ? "\(plural(entries.count)) on Preview; they ride the next Take." : "Program graphics hidden."
        return GraphicsOperationResult(accepted: true, reason: "\(layout.name) cued: \(what)")
    }

    /// PrepareLayout (.Layouts.cs:62-117).
    private func prepareLayout(_ layout: GraphicsShotLayout, _ entries: [GraphicsPlaylistEntry], _ rows: [GraphicsResolvedRow],
                               sceneSelect: (String) async -> Bool, previewStamp: () -> String) async throws -> GraphicsOperationResult {
        if preparedLayout != nil { return refuse("Cancel the prepared Shot Layout before cueing another.") }
        let state = try available()
        let ids = Set(entries.map(\.instanceId))
        if state.instances.contains(where: { ids.contains($0.instanceId) && ($0.cued || $0.cuePending || $0.isOnPreview) }) {
            return refuse("\(layout.name): a required graphic already has a Preview cue. Clear it before cueing this layout.")
        }
        let epoch = state.coreEpoch
        let attemptId = newAttemptId()
        var cues: [GraphicsLayoutCue] = []
        layoutPreparing = true
        layoutCancelRequested = false
        var progress = rows.map { GraphicsLayoutRowProgress(name: $0.row.name, dataRevision: $0.dataRevision, dataReceivedAt: $0.dataReceivedAt, state: "Waiting") }
        func report(_ state: String) { reportLayoutProgress(GraphicsLayoutProgress(layoutId: layout.id, state: state, reason: "", rows: progress)) }
        func checkCancelled() throws { if layoutCancelRequested { throw CancellationError() } }
        report("Preparing")
        func attempt() async throws -> GraphicsOperationResult {
                if !layout.sceneId.isEmpty, !(await sceneSelect(layout.sceneId)) {
                    return refuse("\(layout.name): the selected scene is unavailable.")
                }
                let stamp = previewStamp()
                let deadline = now().addingTimeInterval(5)
                for index in entries.indices {
                    try checkCancelled()
                    _ = try current(epoch)
                    progress[index].state = "Preparing"
                    report("Preparing")
                    let cued = try await cueCore(entries[index], resolved: rows[index], owner: attemptId, deadline: deadline) { cues.append($0) }
                    if !cued.accepted {
                        progress[index].state = "Blocked"
                        report("Blocked")
                        return refuse("\(layout.name): \(rows[index].row.name): \(cued.reason)")
                    }
                    progress[index].state = "Ready"
                }
                try checkCancelled()
                let latest = try current(epoch)
                if cues.contains(where: { cue in !latest.instances.contains { Self.matchesLayoutCue($0, attemptId, cue) } }) {
                    return refuse("\(layout.name): a prepared graphic changed. Cue the layout again.")
                }
                if stamp != previewStamp() { return refuse("\(layout.name): Preview changed during preparation. Cue the layout again.") }
                preparedLayout = GraphicsPreparedLayout(layout: layout, coreEpoch: epoch, attemptId: attemptId, cues: cues, previewStamp: stamp)
                report("Ready for Take")
                return GraphicsOperationResult(accepted: true, reason: "\(layout.name) ready for Take.")
        }
        let outcome: Result<GraphicsOperationResult, Error>
        do { outcome = .success(try await attempt()) } catch { outcome = .failure(error) }
        // finally: anything not carried by a prepared layout is released, owned.
        if preparedLayout?.attemptId != attemptId { await releaseLayoutCues(epoch: epoch, attemptId: attemptId, cues: cues) }
        layoutPreparing = false
        onChange()
        return try outcome.get()
    }

    nonisolated static func matchesLayoutCue(_ instance: GraphicsInstance, _ owner: String, _ cue: GraphicsLayoutCue) -> Bool {
        instance.instanceId == cue.instanceId && instance.cueOwner == owner && instance.cueRevision == cue.cueRevision
            && instance.cued && !instance.cuePending && instance.cuePreparation != "failed"
    }

    /// ReleaseLayoutCues: an owned uncue per cue; a refusal or a dead core is not an error here.
    private func releaseLayoutCues(epoch: String, attemptId: String, cues: [GraphicsLayoutCue]) async {
        guard observe()?.coreEpoch == epoch else { return }
        self.epoch = epoch
        for cue in cues {
            _ = try? await send("graphics.instance.trigger", ["instanceId": cue.instanceId, "expectedRevision": cue.cueRevision,
                                                              "cueOwner": attemptId, "action": "uncue"])
        }
    }

    /// CancelLayoutAsync: stop a running preparation, release a prepared layout's own cues.
    func cancelLayout() async -> GraphicsOperationResult {
        if layoutPreparing { layoutCancelRequested = true }
        return await run {
            if let prepared = self.preparedLayout {
                self.preparedLayout = nil
                await self.releaseLayoutCues(epoch: prepared.coreEpoch, attemptId: prepared.attemptId, cues: prepared.cues)
            }
            self.reportLayoutProgress(nil)
            return GraphicsOperationResult(accepted: true, reason: "Shot Layout cue cancelled.")
        }
    }

    /// IsPreparedLayoutCurrent. Windows compares the layout by reference (any re-save invalidates); the
    /// Mac compares it by value against the document's current layout.
    func isPreparedLayoutCurrent(_ prepared: GraphicsPreparedLayout, previewStamp: String) -> Bool {
        guard prepared == preparedLayout, prepared.previewStamp == previewStamp, layout(prepared.layout.id) == prepared.layout,
              let state = observe(), state.coreEpoch == prepared.coreEpoch else { return false }
        return prepared.cues.allSatisfy { cue in state.instances.contains { Self.matchesLayoutCue($0, prepared.attemptId, cue) } }
    }

    /// ConfirmLayoutTake: the guarded Take committed; Program graphics the layout played out lose their auto-out.
    func confirmLayoutTake(attemptId: String) {
        guard let prepared = preparedLayout, prepared.attemptId == attemptId else { return }
        preparedLayout = nil
        layoutProgress = nil
        if prepared.layout.playOutOthers {
            let carried = Set(prepared.cues.map(\.instanceId))
            for instance in observe()?.instances ?? [] where instance.isOnProgram && !carried.contains(instance.instanceId) {
                cancelAutoOut(instance.instanceId)
            }
        }
        onChange()
    }
}

// ── the GraphicsModel side: scene select waits for the CORE's Preview ───────

extension GraphicsModel {
    /// graphics.layout.cue: the scene goes to Preview through AppModel.selectPreviewScene, and the cue
    /// waits until the core's snapshot shows that Preview scene (the Mac's scene sync is asynchronous,
    /// where Windows reads its own Preview id), 5 s on the injected sleep.
    @discardableResult
    func cueLayout(_ id: String) -> Task<GraphicsOperationResult, Never> {
        let sceneId = layoutSceneId(id)
        let guests = sceneId.isEmpty ? [] : sceneGuestEntries(sceneId)
        let subject = guestEntry(for: programSubjectSourceId())
        return perform { [weak self] playout in
            await playout.cueLayout(id, sceneSelect: { [weak self] sceneId in
                guard let self, self.selectPreviewScene(sceneId) else { return false }
                let until = playout.now().addingTimeInterval(5)
                while self.corePreviewSceneId() != sceneId {
                    if playout.now() >= until { return false }
                    await playout.sleep(50_000_000)
                }
                return true
            }, programSubject: subject, sceneGuests: guests, previewStamp: { [weak self] in self?.layoutPreviewStamp() ?? "" })
        }
    }

    func cancelLayout() { perform { await $0.cancelLayout() } }

    func layoutSceneId(_ id: String) -> String { document.shotLayouts.first { $0.id == id }?.sceneId ?? "" }

    /// GuestEntriesRoutedIn: the Guest entries bound to the sources a scene routes, in route order.
    func sceneGuestEntries(_ sceneId: String) -> [GraphicsPlaylistEntry] {
        guard let guest = rows.first(where: { $0.kind == GraphicsRow.guest }) else { return [] }
        let entries = guestEntries(guest)
        var seen = Set<String>(), result: [GraphicsPlaylistEntry] = []
        for sourceId in sceneSourceIds(sceneId) where seen.insert(sourceId).inserted {
            result += entries.filter { $0.sourceId == sourceId }
        }
        return result
    }
}
