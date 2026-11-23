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
}
