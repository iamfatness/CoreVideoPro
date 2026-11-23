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
            ("control/rc7-srt-vectors", testRC7SrtVectors),
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

    /// Task 2: every srt-configure vector against a real AppModel. `saved` is the
    /// save seam being called; `applied` is the model holding the new config after
    /// the call; `steps` is their order (the model must still hold the baseline
    /// when the save runs). Mutations: validate before the busy check (busy case:
    /// mode refusal instead of busy); treat an omitted passphrase as empty (kept
    /// case); apply before save (steps); interpolate the mode into the refusal
    /// (secret-leak assertion).
    @MainActor
    static func testRC7SrtVectors() {
        let vectors = loadActionVectors(section: "srt-configure")
        expect(vectors.count >= 19, "the srt-configure section carries the plan's cases (\(vectors.count))")
        // Fixture invariant: the baseline differs from every saved config in host and
        // port, so "applied" and "steps" can tell old from new.
        var baseline = StreamDestinationsConfig()
        baseline.srtHost = "baseline.example"
        baseline.srtPort = "7000"
        baseline.hlsPlaylistUrl = "https://hls.example/keep.m3u8"   // a non-SRT field is kept
        for vector in vectors {
            let label = "srt/\(vector.name)"
            let model = takeModel(FakeTakeCore())
            let stored = vector.given["storedPassphrase"] as? String ?? ""
            model.streamDestinations = baseline
            model.srtPassphrase = stored
            if vector.given["busy"] as? Bool == true { model.streamingDesired = true }
            var steps: [String] = []
            var savedPrefs: ShellPrefs?
            var savedPassphrase: String?
            model.srtDestinationSaver = { prefs, passphrase in
                steps.append(model.streamDestinations == baseline && model.srtPassphrase == stored ? "save" : "save-after-apply")
                savedPrefs = prefs
                savedPassphrase = passphrase
                return true
            }
            let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
            let result = invoke(surface, vector.action, vector.args)
            expectVectorReply(result, vector, label)
            for secret in ["invalid-mode-test-secret", "test-passphrase-sentinel"] {
                expect(!(result?.error ?? "").contains(secret), "\(label): the reply never echoes '\(secret)'")
            }
            let applied = model.streamDestinations != baseline || model.srtPassphrase != stored
            if applied { steps.append("apply") }
            let effects = vector.effects
            expectEqual(!steps.isEmpty, effects["saved"] as? Bool ?? false, "\(label): saved")
            expectEqual(applied, effects["applied"] as? Bool ?? false, "\(label): applied")
            expectEqual(steps, effects["steps"] as? [String] ?? [], "\(label): save runs before apply")
            expect(!model.streamingDesired || vector.given["busy"] as? Bool == true, "\(label): never starts streaming")
            guard effects["saved"] as? Bool == true else {
                expectEqual(model.streamDestinations, baseline, "\(label): a refusal changes nothing")
                expectEqual(model.srtPassphrase, stored, "\(label): passphrase untouched")
                continue
            }
            let config = model.streamDestinations
            expectEqual(config.srtEnabled, effects["enabled"] as? Bool ?? !config.srtEnabled, "\(label): enabled")
            for (key, value) in [("mode", config.srtMode), ("host", config.srtHost), ("port", config.srtPort),
                                 ("latencyMs", config.srtLatencyMs), ("streamId", config.srtStreamId),
                                 ("keyLength", config.srtKeyLength)] {
                expectEqual(value, effects[key] as? String ?? "<absent>", "\(label): \(key)")
            }
            let passphrase = model.srtPassphrase
            let kind = passphrase.isEmpty ? "cleared" : (!stored.isEmpty && passphrase == stored ? "kept" : "set")
            expectEqual(kind, effects["passphrase"] as? String ?? "<absent>", "\(label): passphrase")
            expectEqual(savedPassphrase, passphrase, "\(label): the saved passphrase is the applied one")
            if let savedPrefs { expectEqual(savedPrefs.streamDestinations, config, "\(label): the saved prefs carry the applied config") }
            expectEqual(config.hlsPlaylistUrl, baseline.hlsPlaylistUrl, "\(label): non-SRT fields are kept")
        }
        // Storage failure: Windows' sentence (StreamDestinationControl.cs:56), nothing applied.
        let model = takeModel(FakeTakeCore())
        model.streamDestinations = baseline
        model.srtDestinationSaver = { _, _ in false }
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let valid = vectors.first { $0.name == "valid-caller-with-passphrase" }?.args ?? []
        expectEqual(invoke(surface, "transport.stream.srt.configure", valid),
                    .fail("Destination settings were not saved. Previous settings remain active; retry after resolving preference storage."),
                    "srt: storage failure")
        expectEqual(model.streamDestinations, baseline, "srt: a failed save applies nothing")
        expectEqual(model.srtPassphrase, "", "srt: passphrase unchanged after a failed save")
    }
}
