// Graphics G4-G7 Slice B — local datasets, data tables and crawls, data designs.
// Vectors: contracts/graphics-shell/data.vectors.json (GraphicsShellVectorTests.Data*),
// inputs under contracts/graphics-shell/data/.

import Foundation

extension ShellTests {
    static var graphicsDataCases: [(String, () -> Void)] {
        [
            ("graphics-data/parse-vectors", testDataParseVectorsReplay),
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
}
