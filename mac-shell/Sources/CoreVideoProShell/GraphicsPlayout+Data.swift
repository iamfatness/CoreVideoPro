// GraphicsPlayout+Data — stage, publish and crawl control for data rows (G4-G7 Tasks 10-11).
// Port of MC/Services/GraphicsPlayoutService.Data.cs (StageDataAsync, PublishData,
// ControlCrawlAsync, SetDataLiveFollowAsync, CrawlOnProgram) with Windows' reasons verbatim. The
// show document is read through `document` and written through `saveDocument` (the model applies
// the edit and saves; a failed save leaves the document as it was, as Windows' library does).
// Pinned by contracts/graphics-shell/data-transcripts.vectors.json.

import Foundation

extension GraphicsPlayout {
    // ── crawl state ──────────────────────────────────────────────────────────

    /// The row's crawl instance while it is on Program and playing or paused (FindCrawl).
    func crawlOnProgram(_ rowId: String) -> GraphicsInstance? {
        guard let instance = observe()?.instance(GraphicsPlayoutIds.instanceId(rowId: rowId, sourceId: "")),
              instance.isOnProgram, instance.visibility != "exiting", !instance.crawlPlayback.isEmpty else { return nil }
        return instance
    }

    /// The row's single instance while it is on Program (DataOnProgram).
    func dataOnProgram(_ rowId: String) -> GraphicsInstance? {
        guard let instance = observe()?.instance(GraphicsPlayoutIds.instanceId(rowId: rowId, sourceId: "")), instance.isOnProgram else { return nil }
        return instance
    }

    // ── operator actions ─────────────────────────────────────────────────────

    func controlCrawl(rowId: String, action: String) async -> GraphicsOperationResult {
        await run {
            guard ["pause", "resume", "restart"].contains(action), let state = self.observe(), let live = self.crawlOnProgram(rowId) else {
                return self.refuse("This crawl is not on Program.")
            }
            self.epoch = state.coreEpoch
            return try await self.send("graphics.instance.trigger", ["instanceId": live.instanceId, "expectedRevision": live.revision, "action": action])
        }
    }

    func setLiveFollow(rowId: String, enabled: Bool) async -> GraphicsOperationResult {
        await run {
            let document = self.document()
            guard var row = document.row(rowId), var binding = row.dataBinding.flatMap(GraphicsDataBinding.init(json:)),
                  binding.crawl != nil || binding.remote else { return self.refuse("Live follow is unavailable for this row.") }
            binding.liveFollow = enabled
            row.json["dataBinding"] = binding.json
            var next = document
            next.playlist = GraphicsDataDesigns.upsert(document.playlist, row)
            if let error = await self.saveDocument(next) { return self.refuse(error) }
            return GraphicsOperationResult(accepted: true, reason: enabled ? "Live follow enabled for subsequent reloads." : "Manual publication enabled.")
        }
    }

    func stageData(rowId: String, binding selected: GraphicsDataBinding, data: GraphicsLocalData) async -> GraphicsOperationResult {
        await run {
            let document = self.document()
            guard let row = document.row(rowId), let binding = row.dataBinding.flatMap(GraphicsDataBinding.init(json:)),
                  binding == selected, !binding.remote else {
                return self.refuse("The row changed while the data file was open. Choose the file again.")
            }
            do { _ = try GraphicsDataTable.fields(data, binding) }
            catch { return self.refuse((error as? GraphicsDataError)?.message ?? "\(error)") }
            // GraphicsWorkspaceLibrary.SaveLocalDataAsync: the next revision, appended after the other datasets.
            let pushFeed = (document.json["feeds"] as? [JSONObject] ?? []).contains { $0["id"] as? String == binding.sourceId && $0["format"] as? String == "wss" }
            if pushFeed { return self.refuse("Use a new dataset ID for local data.") }
            let previous = document.dataSources.first { $0.id == binding.sourceId }
            let staged = GraphicsDataSourceRecord(id: binding.sourceId, name: previous?.name ?? row.name, revision: (previous?.revision ?? 0) + 1,
                                                  receivedAt: self.now(), data: data)
            var next = document
            next.dataSources = document.dataSources.filter { $0.id != binding.sourceId } + [staged]
            if let error = await self.saveDocument(next) { return self.refuse(error) }
            if binding.liveFollow { return try await self.publishDataCore(rowId: rowId, waitForProgram: true) }
            return GraphicsOperationResult(accepted: true, reason: "Data revision \(staged.revision) loaded. Choose Publish data to apply it.")
        }
    }

    func publishData(rowId: String, waitForProgram: Bool = true) async -> GraphicsOperationResult {
        await run { try await self.publishDataCore(rowId: rowId, waitForProgram: waitForProgram) }
    }

    /// PublishData (Data.cs:122-178): project the source's revision into the row, save it, then (when
    /// the row's graphic is on Program at the row's own template version) update Program and, for a
    /// table, wait up to 5 s on the injected clock for Program to adopt it.
    func publishDataCore(rowId: String, waitForProgram: Bool) async throws -> GraphicsOperationResult {
        try Task.checkCancellation()
        let document = self.document()
        guard let row = document.row(rowId), let binding = row.dataBinding.flatMap(GraphicsDataBinding.init(json:)),
              let source = document.dataSources.first(where: { $0.id == binding.sourceId }) else {
            return refuse("This row has no local dataset.")
        }
        if source.revision < binding.revision { return refuse("The published dataset revision is unavailable.") }
        var current = binding
        current.revision = source.revision
        let cells: JSONObject
        do { cells = try GraphicsDataTable.fields(source.data, current) }
        catch { return refuse((error as? GraphicsDataError)?.message ?? "\(error)") }
        var values = row.fields
        for (key, value) in cells { values[key] = value }
        if source.revision != binding.revision {
            var published = row
            published.fields = values
            published.json["dataBinding"] = current.json
            var next = document
            next.playlist = GraphicsDataDesigns.upsert(document.playlist, published)
            if let error = await saveDocument(next) { return refuse(error) }
        }
        try Task.checkCancellation()
        let saved = GraphicsOperationResult(accepted: true, reason: "Data revision \(source.revision) saved for the next Cue or Show.")
        guard let state = observe(), !state.coreEpoch.isEmpty else { return saved }
        epoch = state.coreEpoch
        let id = GraphicsPlayoutIds.instanceId(rowId: row.id, sourceId: "")
        guard let live = state.instance(id), live.isOnProgram else { return saved }
        if live.templateId != row.templateId || live.templateVersion != row.templateVersion || live.programTemplateVersion != row.templateVersion {
            return refuse("Data is saved. Show the current design version to put it on Program.")
        }
        var updated = try await send("graphics.instance.update", ["instanceId": id, "expectedRevision": live.revision, "target": "program", "fields": values])
        guard updated.accepted else { updated.reason = "Data is saved. Program update refused. " + updated.reason; return updated }
        if let crawl = binding.crawl {
            updated.reason = crawl.mode == "loop" ? "Data revision \(source.revision) queued for a loop boundary."
                                                  : "Data revision \(source.revision) saved for the next Cue or Show."
            return updated
        }
        if !waitForProgram { updated.reason = "Data revision \(source.revision) submitted for Program preparation."; return updated }
        let until = now().addingTimeInterval(5)
        while true {
            let instance = try self.current(state.coreEpoch).instance(id)
            if let instance, updated.currentRevision > 0, instance.programRevision == updated.currentRevision {
                updated.reason = "Data revision \(source.revision) is on Program."
                return updated
            }
            if instance == nil || !instance!.isOnProgram || instance!.revision > updated.currentRevision || instance!.preparation == "failed" {
                return refuse("Data is saved. Program did not adopt the new revision. " + (instance?.warning ?? ""))
            }
            if now() >= until { updated.reason = "Data is saved. The Program update is still preparing."; return updated }
            await sleep(50_000_000)
            try Task.checkCancellation()
        }
    }
}

extension GraphicsModel {
    /// "Data · published <binding revision> · loaded <source revision | unavailable>" (GraphicsPlayoutControl.Data.cs:14-18).
    func dataLine(row: GraphicsRow) -> String {
        guard let binding = row.dataBinding.flatMap(GraphicsDataBinding.init(json:)) else { return "" }
        let loaded = document.dataSources.first { $0.id == binding.sourceId }.map { String($0.revision) } ?? "unavailable"
        return "Data · published \(binding.revision) · loaded \(loaded)"
    }

    @discardableResult func publishData(rowId: String) -> Task<GraphicsOperationResult, Never> { perform { await $0.publishData(rowId: rowId) } }
    @discardableResult func stageData(rowId: String, binding: GraphicsDataBinding, data: GraphicsLocalData) -> Task<GraphicsOperationResult, Never> {
        perform { await $0.stageData(rowId: rowId, binding: binding, data: data) }
    }
    @discardableResult func controlCrawl(rowId: String, action: String) -> Task<GraphicsOperationResult, Never> {
        perform { await $0.controlCrawl(rowId: rowId, action: action) }
    }
    @discardableResult func setLiveFollow(rowId: String, enabled: Bool) -> Task<GraphicsOperationResult, Never> {
        perform { await $0.setLiveFollow(rowId: rowId, enabled: enabled) }
    }

    /// The playout's document write: apply, save, and put the document back if the save fails.
    func applyAndSave(_ next: GraphicsDocument) async -> String? {
        let before = document
        document = next
        reproject()
        saveTask?.cancel()
        if await saveNow() { return nil }
        let message = documentStatus
        document = before
        reproject()
        return message
    }
}

/// The crawl inspector's live line and button rules (GraphicsPlayoutControl.Data.cs:122-144).
enum GraphicsCrawlControls {
    static func enabled(_ action: String, playback: String) -> Bool {
        action == "restart" || (action == "pause" ? playback == "playing" : playback == "paused")
    }

    static func programLine(_ live: GraphicsInstance) -> String {
        let playing = (live.programFields?[GraphicsCrawlData.revisionField]).map { "\($0)" } ?? "unknown"
        let pending = live.crawlPendingRevision > 0 ? (live.fields[GraphicsCrawlData.revisionField]).map { "\($0)" } : nil
        return "Crawl \(live.crawlPlayback) · Program data \(playing)"
            + (pending.map { " · pending \($0) (\(live.preparation))" } ?? "")
            + (live.warning.isEmpty ? "" : " · \(live.warning)")
    }

    /// "Loaded <n> item(s) · <first 3>" when the loaded source is newer than the published revision.
    static func loadedLine(_ source: GraphicsDataSourceRecord, _ binding: GraphicsDataBinding) -> String {
        do {
            let items = try GraphicsCrawlData.items(source.data, column: binding.columns.first ?? "", limit: binding.limit)
            return "Loaded \(items.count) \(items.count == 1 ? "item" : "items") · " + items.prefix(3).joined(separator: "  •  ")
        } catch { return (error as? GraphicsDataError)?.message ?? "\(error)" }
    }
}
