// Graphics G4-G7 Slice B — local datasets, data tables and crawls, data designs.
// Vectors: contracts/graphics-shell/data.vectors.json (GraphicsShellVectorTests.Data*),
// inputs under contracts/graphics-shell/data/.

import Foundation

extension ShellTests {
    static var graphicsDataCases: [(String, () -> Void)] {
        [
            ("graphics-data/parse-vectors", testDataParseVectorsReplay),
            ("graphics-data/table-vectors", testDataTableVectorsReplay),
            ("graphics-data/seed-matches-contract", testTitleSeedMatchesTheContractFile),
            ("graphics-data/windows-data-document-round-trips", testWindowsDataDocumentRoundTrips),
            ("graphics-data/document-vectors", testDataDocumentVectorsReplay),
            ("graphics-data/mac-writes-a-data-source-windows-can-read", testMacWritesADataSourceWindowsCanRead),
            ("graphics-data/data-sources-setter-keeps-other-sections", testDataSourcesSetterKeepsOtherSections),
            ("graphics-data/save-refuses-an-invalid-binding", testSaveRefusesAnInvalidBinding),
        ]
    }

    /// The bytes a generated vector input stands for (GraphicsShellVectorTests.GeneratedBytes).
    static func generatedDataBytes(_ recipe: JSONObject) -> Data {
        if let b64 = recipe["base64"] as? String { return Data(base64Encoded: b64) ?? Data() }
        var text = recipe["prefix"] as? String ?? ""
        let unit = recipe["unit"] as? String ?? "", count = (recipe["count"] as? NSNumber)?.intValue ?? 0
        for i in 0..<count { text += unit.replacingOccurrences(of: "{i}", with: String(i)) }
        text += recipe["suffix"] as? String ?? ""
        return Data(text.utf8)
    }

    /// Reads a vector's input through GraphicsLocalData.read (the byte path), as Windows' ReadAsync.
    static func readDataInput(_ input: JSONObject) -> Result<GraphicsLocalData, GraphicsDataError> {
        let key = input["key"] as? String
        if let file = input["file"] as? String {
            return Result { try GraphicsLocalData.read(url: repoRoot.appendingPathComponent("contracts/graphics-shell/" + file), key: key) }
                .mapError { $0 as? GraphicsDataError ?? GraphicsDataError(message: "\($0)") }
        }
        let bytes: Data = (input["text"] as? String).map { Data($0.utf8) } ?? generatedDataBytes(input["generate"] as? JSONObject ?? [:])
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let url = URL(fileURLWithPath: dir).appendingPathComponent("input" + (input["extension"] as? String ?? ".csv"))
        try? bytes.write(to: url)
        return Result { try GraphicsLocalData.read(url: url, key: key) }.mapError { $0 as? GraphicsDataError ?? GraphicsDataError(message: "\($0)") }
    }

    static func dataResultJSON(_ result: Result<GraphicsLocalData, GraphicsDataError>) -> JSONObject {
        switch result {
        case .success(let data):
            return ["columns": data.columns.map { ["id": $0.id, "kind": $0.kind] as JSONObject },
                    "rows": data.rows.map { ["key": $0.key, "cells": $0.cells] as JSONObject }]
        case .failure(let error): return ["error": error.message]
        }
    }

    static func testDataParseVectorsReplay() {
        var checked = 0
        let scores = try? GraphicsLocalData.read(url: repoRoot.appendingPathComponent("contracts/graphics-shell/data/scores.csv"), key: nil)
        for vector in loadGraphicsVectors("data") {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            switch vector["operation"] as? String {
            case "read":
                let got = dataResultJSON(readDataInput(input))
                expect(jsonEqual(got, expected), "\(id): got \(String(describing: got).prefix(400))")
            case "withKey":
                guard let scores else { expect(false, "\(id): scores.csv must read"); continue }
                let got = dataResultJSON(Result { try scores.withKey(input["key"] as? String) }
                    .mapError { $0 as? GraphicsDataError ?? GraphicsDataError(message: "\($0)") })
                expect(jsonEqual(got, expected), "\(id): got \(String(describing: got).prefix(400))")
            case "project":
                guard let keyed = try? scores?.withKey("team") else { expect(false, "\(id): keyed scores"); continue }
                let got: JSONObject
                do { got = ["rows": try keyed.project(columns: input["columns"] as? [String] ?? [], orderBy: input["orderBy"] as? String,
                                                       limit: (input["limit"] as? NSNumber)?.intValue ?? 0)] }
                catch { got = ["error": (error as? GraphicsDataError)?.message ?? "\(error)"] }
                expect(jsonEqual(got, expected), "\(id): got \(got)")
            default:
                continue   // other operations belong to later tasks' tests
            }
            checked += 1
        }
        expect(checked >= 150, "every data parse vector replayed (\(checked))")
    }

    static func outcome(_ body: () throws -> Any?) -> JSONObject {
        do { return ["value": try body() ?? NSNull()] }
        catch { return ["error": (error as? GraphicsDataError)?.message ?? "\(error)"] }
    }

    static func testDataTableVectorsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("data-table") {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            let data = (input["data"] as? JSONObject).flatMap(GraphicsLocalData.init(json:))
            let binding = (input["binding"] as? JSONObject).flatMap(GraphicsDataBinding.init(json:))
            let got: JSONObject
            switch vector["operation"] as? String {
            case "fields":
                guard let data, let binding else { expect(false, "\(id): input decodes"); continue }
                got = outcome { try GraphicsDataTable.fields(data, binding) }
            case "manifest":
                guard let data, let binding else { expect(false, "\(id): input decodes"); continue }
                got = outcome { try GraphicsDataTable.manifest(templateId: input["templateId"] as? String ?? "", name: input["name"] as? String ?? "",
                                                               binding: binding, data: data) }
            case "bindingFromDefinition":
                got = ["binding": GraphicsDataTable.binding(fromDefinition: input["definition"] as? JSONObject ?? [:])?.json ?? NSNull()]
            case "bindingValid":
                guard let binding else { expect(false, "\(id): binding decodes"); continue }
                got = ["valid": binding.isValid]
            case "crawlItems":
                guard let data else { expect(false, "\(id): data decodes"); continue }
                got = outcome { try GraphicsCrawlData.items(data, column: input["column"] as? String ?? "", limit: (input["limit"] as? NSNumber)?.intValue ?? 0) }
            case "seed":
                got = ["manifest": GraphicsDesignSeeds.manifest(kind: input["kind"] as? String ?? "", templateId: input["templateId"] as? String ?? "",
                                                                templateVersion: (input["templateVersion"] as? NSNumber)?.intValue ?? 0, name: input["name"] as? String ?? "") ?? NSNull()]
            default: expect(false, "\(id): unknown operation"); continue
            }
            expect(jsonEqual(got, expected), "\(id): got \(String(describing: got).prefix(500))")
            checked += 1
        }
        expect(checked >= 90, "every data-table vector replayed (\(checked))")
    }

    /// The embedded title seed is the contract file Windows embeds (contracts/graphics-seeds/title.json).
    static func testTitleSeedMatchesTheContractFile() {
        let url = repoRoot.appendingPathComponent("contracts/graphics-seeds/title.json")
        let file = (try? Data(contentsOf: url)).flatMap { try? JSONSerialization.jsonObject(with: $0) }
        expect(jsonEqual(GraphicsDesignSeeds.titleSeed, file), "embedded title seed equals the contract file")
    }

    static func documentJSON(_ relative: String) -> JSONObject { loadGraphicsNode("documents/" + relative) }

    /// A show the real Windows serializer wrote with datasets and data designs loads writable and
    /// re-saves to the same JSON (sections the Mac does not edit are kept verbatim).
    static func testWindowsDataDocumentRoundTrips() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        try? FileManager.default.copyItem(at: repoRoot.appendingPathComponent("contracts/graphics-shell/documents/windows-data.json"), to: URL(fileURLWithPath: path))
        let store = GraphicsDocumentStore(path: path)
        let load = store.load()
        expect(!load.readOnly, "windows-data loads writable: \(load.status)")
        expectEqual(load.document.dataSources.map(\.id), ["scores", "headlines"], "both datasets decode")
        expectEqual(load.document.dataSources.first?.revision, 2, "scores at revision 2")
        expectEqual(load.document.dataSources.first?.data.rows.count, 6, "scores rows")
        expectEqual(load.document.designs.count, 2, "both designs")
        do { try store.save(load.document) } catch { expect(false, "re-save: \(error)") }
        let saved = (try? Data(contentsOf: URL(fileURLWithPath: path))).flatMap { try? JSONSerialization.jsonObject(with: $0) }
        expect(jsonEqual(saved, documentJSON("windows-data.json")), "re-saved document equals the Windows original")
    }

    static func testDataDocumentVectorsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("data-document") {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            switch vector["operation"] as? String {
            case "load":
                let dir = graphicsTempDir()
                defer { try? FileManager.default.removeItem(atPath: dir) }
                let path = (dir as NSString).appendingPathComponent("default.json")
                if let primary = input["primary"] as? String { graphicsWrite(primary, path) }
                if let file = input["primaryFile"] as? String {
                    try? FileManager.default.copyItem(at: repoRoot.appendingPathComponent("contracts/graphics-shell/" + file), to: URL(fileURLWithPath: path))
                }
                let load = GraphicsDocumentStore(path: path).load()
                expectEqual(load.readOnly, expected["readOnly"] as? Bool ?? false, "\(id): readOnly")
                expectEqual(load.status, expected["status"] as? String ?? "", "\(id): status")
                expectEqual(load.document.playlist.map(\.id), expected["playlistIds"] as? [String] ?? [], "\(id): playlist")
                expectEqual(load.document.dataSources.map(\.id), expected["dataSourceIds"] as? [String] ?? [], "\(id): datasets")
                expectEqual(load.document.designs.map { $0["designId"] as? String ?? "" }, expected["designIds"] as? [String] ?? [], "\(id): designs")
            case "newTemplateId":
                var hex = input["hex6"] as? [String] ?? []
                let got = try? GraphicsDesignIds.newTemplateId(name: input["name"] as? String ?? "", existing: Set(input["existing"] as? [String] ?? []),
                                                              hex6: { hex.isEmpty ? "ffffff" : hex.removeFirst() })
                expectEqual(got, expected["templateId"] as? String, "\(id)")
            default: expect(false, "\(id): unknown operation"); continue
            }
            checked += 1
        }
        expect(checked >= 18, "every data-document vector replayed (\(checked))")
    }

    static let macFixtureDate = Date(timeIntervalSince1970: 1_795_867_200)   // 2026-11-28T12:00:00Z

    /// The Mac adds a dataset, a data design and a bound row to the Windows show and saves it; the
    /// C# GraphicsShellMacDocumentTests then loads that file with the real Windows store.
    static func testMacWritesADataSourceWindowsCanRead() {
        guard let data = try? GraphicsLocalData.read(url: repoRoot.appendingPathComponent("contracts/graphics-shell/data/scores.csv"), key: "team")
        else { expect(false, "scores.csv reads"); return }
        let binding = GraphicsDataBinding(sourceId: "mac-scores", keyColumn: "team", columns: ["team", "points"], sortColumn: "points", limit: 5, revision: 1)
        guard let manifest = try? GraphicsDataTable.manifest(templateId: "design-mac-scores-0a1b2c", name: "Mac scores", binding: binding, data: data),
              let fields = try? GraphicsDataTable.fields(data, binding)
        else { expect(false, "manifest builds"); return }
        var document = GraphicsDocument(json: documentJSON("windows-show.json"))
        document.upsertDataSource(GraphicsDataSourceRecord(id: "mac-scores", name: "scores", revision: 1, receivedAt: macFixtureDate, data: data))
        document.appendDesign(["designId": "design-mac-scores-0a1b2c", "name": "Mac scores", "family": "title", "templateId": "design-mac-scores-0a1b2c",
                               "templateVersion": 1, "definition": manifest, "assets": [] as [Any], "updatedAt": GraphicsDataSourceRecord.iso8601(macFixtureDate)])
        var row = GraphicsRow(json: ["id": "mac-scores", "kind": "title", "name": "Mac scores", "hotkey": "", "playOutAfterMs": 0,
                                     "templateId": "design-mac-scores-0a1b2c", "templateVersion": 1, "fields": fields, "followSources": false, "showClock": false])
        row.json["dataBinding"] = binding.json
        document.playlist = document.playlist + [row]
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        do { try GraphicsDocumentStore(path: path).save(document) } catch { expect(false, "Mac save: \(error)"); return }
        let fixture = repoRoot.appendingPathComponent("contracts/graphics-shell/documents/mac-data-roundtrip.json")
        if ProcessInfo.processInfo.environment["COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS"] == "1" {
            try? FileManager.default.removeItem(at: fixture)
            try? FileManager.default.copyItem(at: URL(fileURLWithPath: path), to: fixture)
        }
        let written = (try? Data(contentsOf: URL(fileURLWithPath: path))).flatMap { try? JSONSerialization.jsonObject(with: $0) }
        let committed = (try? Data(contentsOf: fixture)).flatMap { try? JSONSerialization.jsonObject(with: $0) }
        expect(jsonEqual(written, committed), "the Mac writes what mac-data-roundtrip.json holds (regenerate with COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1)")
        let source = (written as? JSONObject).flatMap { ($0["dataSources"] as? [JSONObject])?.first }
        expectEqual(source?["receivedAt"] as? String, "2026-11-28T12:00:00.000Z", "receivedAt is ISO 8601 text System.Text.Json reads")
    }

    /// Setting dataSources rewrites only that section: designs, feeds and unknown sections survive byte-equal.
    static func testDataSourcesSetterKeepsOtherSections() {
        var json = documentJSON("windows-data.json")
        json["feeds"] = [["id": "feed-1", "keep": true]] as [JSONObject]
        var document = GraphicsDocument(json: json)
        var sources = document.dataSources
        sources.removeLast()
        document.dataSources = sources
        expectEqual(document.dataSources.map(\.id), ["scores"], "the section is replaced")
        for key in json.keys where key != "dataSources" {
            expect(jsonEqual(document.json[key], json[key]), "\(key) survives the dataSources setter")
        }
        expect(jsonEqual((document.json["dataSources"] as? [JSONObject])?.first, (json["dataSources"] as? [JSONObject])?.first),
               "an unchanged dataset keeps its Windows-written JSON")
    }

    /// A row whose binding Windows would refuse is never saved (the 9-column copy).
    static func testSaveRefusesAnInvalidBinding() {
        var json = documentJSON("windows-data.json")
        var playlist = json["playlist"] as? [JSONObject] ?? []
        var binding = playlist[0]["dataBinding"] as? JSONObject ?? [:]
        binding["columns"] = (1...9).map { "c\($0)" }
        binding["sortColumn"] = NSNull()
        playlist[0]["dataBinding"] = binding
        json["playlist"] = playlist
        expect((try? GraphicsDocument.validate(json)) == nil, "a 9-column binding fails validation")
        json = documentJSON("windows-data.json")
        var sources = json["dataSources"] as? [JSONObject] ?? []
        sources[0]["revision"] = 0
        json["dataSources"] = sources
        expect((try? GraphicsDocument.validate(json)) == nil, "a dataset at revision 0 fails validation")
    }
}
