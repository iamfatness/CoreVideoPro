// macOS control parity, Phase 4 RC7 (docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md,
// Batch C). Every refusal sentence and effect asserted here comes from
// contracts/control.action-vectors.json, which the WINDOWS tests write by running the Windows helpers
// (CT/WT/MCT ControlActionVectorExportTests). Nothing in a vector is typed on this side.
// ContractParity skips *Tests.swift files.

import Foundation

/// One case of contracts/control.action-vectors.json.
struct ActionVector {
    let name: String
    let action: String
    /// JSON args as the router would bind them: Bool, String, Int, Double or nil.
    let args: [Any?]
    let given: [String: Any]
    let ok: Bool
    let error: String?
    /// When set, only this prefix of `error` is portable (runtime exception text follows it).
    let errorPrefix: String?
    let effects: [String: Any]
}

extension ShellTests {

    @MainActor
    static var controlRC7Cases: [(String, () -> Void)] {
        [
            ("control/action-vectors-shape", testActionVectorsShape),
        ]
    }

    /// A JSON scalar as the control binder sees it (CFBoolean is a Bool, never 0/1).
    static func vectorScalar(_ value: Any) -> Any? {
        if value is NSNull { return nil }
        if let number = value as? NSNumber {
            if CFGetTypeID(number) == CFBooleanGetTypeID() { return number.boolValue }
            if CFNumberIsFloatType(number) { return number.doubleValue }
            return number.intValue
        }
        return value
    }

    static func actionVectorSections() -> [String: Any]? {
        repoJSON("contracts/control.action-vectors.json")?["sections"] as? [String: Any]
    }

    /// The cases of one section, in file order. Empty (and a failed check) when the section is missing.
    static func loadActionVectors(section: String) -> [ActionVector] {
        guard let body = actionVectorSections()?[section] as? [String: Any],
              let cases = body["cases"] as? [[String: Any]] else {
            expect(false, "contracts/control.action-vectors.json has section '\(section)'")
            return []
        }
        return cases.map { row in
            let expectation = row["expect"] as? [String: Any] ?? [:]
            return ActionVector(
                name: row["name"] as? String ?? "?",
                action: row["action"] as? String ?? "",
                args: (row["args"] as? [Any] ?? []).map(vectorScalar),
                given: row["given"] as? [String: Any] ?? [:],
                ok: expectation["ok"] as? Bool ?? false,
                error: expectation["error"] as? String,
                errorPrefix: expectation["errorPrefix"] as? String,
                effects: expectation["effects"] as? [String: Any] ?? [:])
        }
    }

    /// The reply matches the vector's ok/error (exactly, or by its declared portable prefix).
    static func expectVectorReply(_ result: ControlInvokeResult?, _ vector: ActionVector, _ label: String) {
        guard let result else { expect(false, "\(label): answered"); return }
        expectEqual(result.ok, vector.ok, "\(label): ok")
        guard let error = vector.error else {
            expect(result.error == nil, "\(label): no error (got \(result.error ?? ""))")
            return
        }
        if let prefix = vector.errorPrefix {
            expect(error.hasPrefix(prefix) && (result.error ?? "").hasPrefix(prefix)
                   && (result.error ?? "").count > prefix.count,
                   "\(label): error starts with '\(prefix)' and says why (got \(result.error ?? "nil"))")
        } else {
            expectEqual(result.error, error, "\(label): Windows sentence")
        }
    }

    /// Drift guard for the shared file: parses, every section names its source and writer and has
    /// cases, every action is a Windows registry id, and no case name repeats within a section.
    /// Mutations: a case naming `scene.bogus`, or a deleted `writer`, turn this red.
    static func testActionVectorsShape() {
        guard let file = repoJSON("contracts/control.action-vectors.json") else {
            expect(false, "contracts/control.action-vectors.json parses"); return
        }
        expectEqual(file["version"] as? Int, 1, "version 1")
        guard let sections = file["sections"] as? [String: Any], !sections.isEmpty else {
            expect(false, "the file has sections"); return
        }
        let registry = Set((repoJSON("contracts/control.manifest.json")?["Actions"] as? [[String: Any]] ?? [])
            .compactMap { $0["Id"] as? String })
        expect(registry.count == 110, "the Windows registry has 110 actions (fixture-derived operand)")
        for (name, value) in sections.sorted(by: { $0.key < $1.key }) {
            guard let section = value as? [String: Any] else { expect(false, "\(name) is an object"); continue }
            expect(!((section["source"] as? String) ?? "").isEmpty, "\(name) names its Windows source")
            expect(["CT", "WT", "MCT"].contains(section["writer"] as? String ?? ""), "\(name) names its writer (CT|WT|MCT)")
            let cases = section["cases"] as? [[String: Any]] ?? []
            expect(!cases.isEmpty, "\(name) has at least one case")
            var seen = Set<String>()
            for row in cases {
                let caseName = row["name"] as? String ?? ""
                expect(!caseName.isEmpty && seen.insert(caseName).inserted, "\(name)/\(caseName): a unique case name")
                let action = row["action"] as? String ?? ""
                expect(registry.contains(action), "\(name)/\(caseName): '\(action)' is a Windows registry id")
                expect((row["expect"] as? [String: Any])?["ok"] is Bool, "\(name)/\(caseName): expect.ok is a bool")
            }
        }
        for required in ["scene-routes"] {
            expect(sections[required] != nil, "section \(required) is present")
        }
    }
}
