// GraphicsDataDesigns — data designs before the Builder (G4-G7 Task 8, plan D4/D6):
// design template ids (port of MC/Models/GraphicsDesign.cs GraphicsDesignIds) and
// the saved-design check the document store applies (GraphicsDocumentStore.ValidDesign).

import Foundation

enum GraphicsDesignIds {
    static let maxLength = 64, maxDesigns = 32, maxNameLength = 80
    private static let prefix = "design-"
    private static let slugBudget = maxLength - 7 - 7   // "design-" + slug + "-" + 6 hex

    /// The native id rule: [a-z0-9-], leading letter, at most 64 characters.
    static func isNativeId(_ id: String) -> Bool {
        guard (1...maxLength).contains(id.utf16.count), let first = id.unicodeScalars.first, ("a"..."z").contains(first) else { return false }
        return id.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" }
    }

    /// ^design-[a-z0-9]+(-[a-z0-9]+)*-[0-9a-f]{6}$, at most 64 characters.
    static func isDesignTemplateId(_ id: String) -> Bool {
        guard id.utf16.count <= maxLength, id.hasPrefix(prefix) else { return false }
        let parts = id.dropFirst(prefix.count).split(separator: "-", omittingEmptySubsequences: false)
        guard parts.count >= 2, let last = parts.last, last.count == 6,
              last.unicodeScalars.allSatisfy({ ("0"..."9").contains($0) || ("a"..."f").contains($0) }) else { return false }
        return parts.dropLast().allSatisfy { part in
            !part.isEmpty && part.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) }
        }
    }

    static func slug(_ name: String) -> String {
        var out = ""
        for scalar in GraphicsNamesList.lowerInvariant(name).unicodeScalars {
            if ("a"..."z").contains(scalar) || ("0"..."9").contains(scalar) { out.unicodeScalars.append(scalar) }
            else if !out.isEmpty && !out.hasSuffix("-") { out += "-" }
        }
        if out.count > slugBudget { out = String(out.prefix(slugBudget)) }
        out = out.trimmingCharacters(in: CharacterSet(charactersIn: "-"))
        return out.isEmpty ? "untitled" : out
    }

    struct Exhausted: Error {}

    static func newTemplateId(name: String, existing: Set<String>, hex6: () -> String) throws -> String {
        let base = slug(name)
        for _ in 0..<1000 {
            let id = prefix + base + "-" + hex6()
            if isDesignTemplateId(id) && !existing.contains(id) { return id }
        }
        throw Exhausted()
    }

    /// Six random lower-case hex digits (production hex6).
    static func randomHex6() -> String { String(format: "%06x", UInt32.random(in: 0..<0x1000000)) }

    /// GraphicsDocumentStore.ValidDesign.
    static func validDesign(_ d: JSONObject) -> Bool {
        typealias V = GraphicsValue
        guard let designId = d["designId"] as? String, (1...64).contains(designId.utf16.count),
              let name = d["name"] as? String, (1...maxNameLength).contains(name.utf16.count),
              let family = d["family"] as? String, (1...32).contains(family.utf16.count),
              let templateId = d["templateId"] as? String, isDesignTemplateId(templateId),
              let version = V.int(d, "templateVersion", 0), version >= 1,
              let definition = d["definition"] as? JSONObject,
              let bytes = try? JSONSerialization.data(withJSONObject: definition), bytes.count <= 256 * 1024,
              let assets = V.array(d, "assets", []), assets.count <= 32 else { return false }
        var seen = Set<String>()
        for item in assets {
            guard let asset = item as? JSONObject, let assetId = asset["assetId"] as? String, isNativeId(assetId), seen.insert(assetId).inserted,
                  let sha = asset["sha256"] as? String, sha.utf16.count == 64, sha.allSatisfy(\.isHexDigit),
                  let path = asset["relativePath"] as? String, (1...260).contains(path.utf16.count), !path.hasPrefix("/"),
                  path.split(separator: "/", omittingEmptySubsequences: false).flatMap({ $0.split(separator: "\\", omittingEmptySubsequences: false) })
                    .allSatisfy({ !$0.isEmpty && $0 != ".." && !$0.contains("\0") && !$0.contains(":") })
            else { return false }
        }
        return true
    }
}

/// What a design action did (GraphicsDesignResult, plus the playlist row a data design was added to).
struct GraphicsDesignOutcome: Equatable {
    let ok: Bool
    let message: String
    let rowId: String?
}

enum GraphicsDataDesigns {
    static let nameRule = "Use a design name of 1–80 characters."
    static let tooMany = "A show holds at most 32 designs. Delete one you no longer use."
    static let sourceChanged = "The data source changed or is unavailable. Choose the source again."

    /// GraphicsBuilder.NextFreeHotkey: F1..F12, the first no row holds ("" when all are taken).
    static func nextFreeHotkey(_ rows: [GraphicsRow]) -> String {
        let taken = Set(rows.map(\.hotkey))
        return (1...12).map { "F\($0)" }.first { !taken.contains($0) } ?? ""
    }

    /// GraphicsBuilder.Upsert: replace by id, else insert before the empty sponsor placeholder (or append);
    /// the row's F-key moves to it from any other row.
    static func upsert(_ rows: [GraphicsRow], _ row: GraphicsRow) -> [GraphicsRow] {
        var list = rows.map { other -> GraphicsRow in
            var other = other
            if other.id != row.id && !row.hotkey.isEmpty && other.hotkey == row.hotkey { other.hotkey = "" }
            return other
        }
        if let index = list.firstIndex(where: { $0.id == row.id }) { list[index] = row; return list }
        if let sponsor = list.firstIndex(where: { $0.kind == GraphicsRow.sponsor && $0.templateId.isEmpty && $0.image == nil }) {
            list.insert(row, at: sponsor)
        } else {
            list.append(row)
        }
        return list
    }

    /// The playlist row a NEW data design gets (GraphicsDesignWorkflow.PlaylistRow with no existing row,
    /// via GraphicsBuilder.RowFrom): kind from the family, the design's name, the next free F-key, no
    /// auto-out, the template's field defaults, the binding the manifest carries.
    static func playlistRow(name: String, templateId: String, family: String, manifest: JSONObject,
                            binding: GraphicsDataBinding, rows: [GraphicsRow]) -> GraphicsRow {
        let kind: String
        switch family {
        case "bug": kind = GraphicsRow.logo
        case "clock": kind = GraphicsRow.liveBug
        default: kind = GraphicsRow.title   // title, full screen and a lower third not following Sources
        }
        var fields: JSONObject = [:]
        for field in manifest["fields"] as? [JSONObject] ?? [] {
            if let id = field["id"] as? String, let value = field["default"] { fields[id] = value }
        }
        let trimmed = GraphicsRowFields.clamp(GraphicsLocalData.trim(name), 128)
        let hasClock = family == "clock" || fields["time"] != nil
        var row = GraphicsRow(json: [
            "id": GraphicsNamesList.newRowId(trimmed, taken: rows.map(\.id)), "kind": kind, "name": trimmed,
            "hotkey": nextFreeHotkey(rows), "playOutAfterMs": 0, "templateId": templateId, "templateVersion": 1,
            "fields": fields, "followSources": false, "showClock": hasClock,
        ])
        row.json["dataBinding"] = (GraphicsDataTable.binding(fromDefinition: manifest) ?? binding).json
        return row
    }
}

extension GraphicsModel {
    /// Create a data table or crawl design from a local dataset and add its playlist row (plan D4: the
    /// Mac has no Builder "Add to playlist" until G8). Port of GraphicsDesignService.CreateDataTableAsync
    /// + Create, then GraphicsDesignWorkflow.PlaylistRow + GraphicsBuilder.Upsert. Nothing is written
    /// before the install is confirmed; a failed save removes the installed template again.
    func createDataDesign(name: String, data: GraphicsLocalData, binding: GraphicsDataBinding) async -> GraphicsDesignOutcome {
        func fail(_ message: String) -> GraphicsDesignOutcome {
            status = message
            return GraphicsDesignOutcome(ok: false, message: message, rowId: nil)
        }
        let clean = GraphicsLocalData.trim(name)
        guard (1...GraphicsDesignIds.maxNameLength).contains(clean.utf16.count) else { return fail(GraphicsDataDesigns.nameRule) }
        guard document.designs.count < GraphicsDesignIds.maxDesigns else { return fail(GraphicsDataDesigns.tooMany) }
        let existing = Set(document.designs.compactMap { $0["templateId"] as? String } + document.templates.map(\.templateId)
                           + (state?.templates.map(\.templateId) ?? []))
        guard let templateId = try? GraphicsDesignIds.newTemplateId(name: clean, existing: existing, hex6: designHex6) else {
            return fail("Could not generate a unique design template id.")
        }
        let source = GraphicsDataSourceRecord(id: binding.sourceId, name: clean, revision: 1, receivedAt: playout.now(), data: data)
        let feeds = (document.json["feeds"] as? [JSONObject] ?? []).compactMap { $0["id"] as? String }
        guard source.isValid, binding.revision == source.revision, !binding.remote,
              !document.dataSources.contains(where: { $0.id == source.id }), !feeds.contains(source.id) else {
            return fail(GraphicsDataDesigns.sourceChanged)
        }
        let manifest: JSONObject
        do { manifest = try GraphicsDataTable.manifest(templateId: templateId, name: clean, binding: binding, data: data) }
        catch { return fail((error as? GraphicsDataError)?.message ?? "\(error)") }

        // Install through the playout gate; the design is stored only once the core lists it.
        let pin = GraphicsTemplatePin(templateId: templateId, templateVersion: 1, manifestHash: "", definition: manifest, packageRoot: "")
        var installed: GraphicsTemplate?
        let result = await playout.run { [playout] in
            guard let playout else { return GraphicsOperationResult.refused(GraphicsCommandRequest.unavailable) }
            let epoch = try playout.available().coreEpoch
            playout.epoch = epoch
            if let present = try playout.current(epoch).template(templateId, 1) { installed = present; return GraphicsOperationResult(accepted: true) }
            let (landed, refusal) = try await playout.install(pin, epoch: epoch)
            if let landed { installed = landed; return GraphicsOperationResult(accepted: true) }
            return refusal ?? playout.refuse(GraphicsPlayout.designInstallTimeout)
        }
        guard result.accepted, let installed else { return fail(result.reason) }

        let family = !installed.family.isEmpty ? installed.family : manifest["family"] as? String ?? ""
        let design: JSONObject = ["designId": templateId, "name": clean, "family": family, "templateId": templateId, "templateVersion": 1,
                                  "definition": installed.definition ?? manifest, "assets": [] as [Any],
                                  "updatedAt": GraphicsDataSourceRecord.iso8601(playout.now()), "packageRoot": NSNull()]
        let before = document
        document.appendDesign(design)
        document.upsertDataSource(source)
        guard await saveNow() else {
            let message = documentStatus
            document = before
            _ = await playout.gated { [playout] in try? await playout?.removeTemplate(templateId, 1) }
            return fail(message)
        }
        var message = "Created \(clean)."
        let row = GraphicsDataDesigns.playlistRow(name: clean, templateId: templateId, family: family, manifest: manifest, binding: binding, rows: rows)
        // As Windows' SavePlaylistAsync: the row counts only once it is on disk (review I1).
        let previousRows = document.playlist
        setRows(GraphicsDataDesigns.upsert(rows, row))
        saveTask?.cancel()
        guard document.playlist != previousRows, await saveNow() else {
            return fail(message + " Not added to the playlist: " + documentStatus)
        }
        message += " \"\(row.name)\" is in the playlist" + (row.hotkey.isEmpty ? "" : " on " + row.hotkey) + ". Show it from Operate when you want it on air."
        status = message
        return GraphicsDesignOutcome(ok: true, message: message, rowId: row.id)
    }
}
