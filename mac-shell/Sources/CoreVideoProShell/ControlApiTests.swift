// ControlApiTests.swift — the local control API's cases for the in-binary
// harness (COREVIDEO_SHELL_TESTS=1; registered in ShellTests.runAndExit).
//
// Golden data comes from the WINDOWS build, not from this port: the manifest (contracts/control.manifest.json)
// JSON and every router status/error string below were produced by running
// native-shell/CoreVideoPro.Control (HttpControlRouter / ControlManifest) under
// dotnet on this Mac. The snapshot and redaction cases are ports of
// HttpSnapshotRouteTests, CoreSnapshotObserverTests and
// SupportBundleLogRedactorTests; the auth cases of HttpControlServerTests.

import Combine
import Foundation
import Network

extension Data {
    /// Test helper: bytes from a hex string ("" -> empty).
    init(hex: String) {
        var bytes: [UInt8] = []
        var index = hex.startIndex
        while let next = hex.index(index, offsetBy: 2, limitedBy: hex.endIndex), index < hex.endIndex {
            bytes.append(UInt8(hex[index..<next], radix: 16) ?? 0)
            index = next
        }
        self.init(bytes)
    }

    var hexString: String { map { String(format: "%02x", $0) }.joined() }
}

extension ShellTests {
    // ── fakes ────────────────────────────────────────────────────────────────

    final class FakeSurface: ControlSurface, @unchecked Sendable {
        private let lock = NSLock()
        private var _invocations: [(String, [Any?])] = []
        private var _observationReads = 0
        var handler: (String, [Any?]) -> ControlInvokeResult = { _, _ in .success }
        var state: [String: Any] = ["recording": false, "activeSceneId": ""]
        var observesCore = true
        var observation = ControlSnapshotObservation.unavailable("no snapshot")
        var observationError: Error?

        var invocations: [(String, [Any?])] { lock.lock(); defer { lock.unlock() }; return _invocations }
        var observationReads: Int { lock.lock(); defer { lock.unlock() }; return _observationReads }

        func invoke(_ actionId: String, _ args: [Any?]) async -> ControlInvokeResult {
            lock.withLock { _invocations.append((actionId, args)) }
            return handler(actionId, args)
        }
        func controlState() async -> [String: Any] { state }
        func snapshotObservation() throws -> ControlSnapshotObservation {
            lock.lock(); _observationReads += 1; lock.unlock()
            if let observationError { throw observationError }
            return observation
        }
    }

    final class Box<T>: @unchecked Sendable { var value: T? }

    @MainActor
    static func route(_ router: ControlRouter, _ method: String, _ path: String,
                      _ body: String? = nil) -> ControlResponse {
        let box = Box<ControlResponse>()
        Task.detached { box.value = await router.handle(method: method, path: path, body: body.map { Data($0.utf8) }) }
        spin(until: { box.value != nil }, limit: 3)
        return box.value ?? ControlResponse(status: -1, body: "")
    }

    static func json(_ text: String) -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any] ?? [:]
    }

    // ── contract: the shared manifest (contracts/control.*.json) ──────────────

    /// The repo checkout, from this source file (mac-shell/Sources/CoreVideoProShell/…).
    static func repoFile(_ relative: String) -> Data? {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        return try? Data(contentsOf: root.appendingPathComponent(relative))
    }

    static func repoJSON(_ relative: String) -> [String: Any]? {
        repoFile(relative).flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
    }

    /// Supported ids from the platform file ON DISK, independent of the generated Swift.
    static func fileSupportedIds() -> Set<String>? {
        guard let rows = repoJSON("contracts/control.platforms.json")?["actions"] as? [[String: Any]] else { return nil }
        return Set(rows.filter { $0["mac"] as? String == "supported" }.compactMap { $0["id"] as? String })
    }

    @MainActor
    static func testControlClosure() {
        guard let fileSupported = fileSupportedIds() else {
            expect(false, "contracts/control.platforms.json is readable from the checkout"); return
        }
        // Each slice updates this number: RC1 13, after RC2 25, after RC3 36.
        expectEqual(fileSupported.count, 36, "supported rows in the platform file")
        expectEqual(AppModelControlSurface.supportedActionIds, fileSupported,
                    "the dispatcher's ids equal the platform file's supported rows (Windows SupportedActionIds twin)")
        expectEqual(Set(ControlCatalog.actions.map(\.id)), fileSupported,
                    "the generated catalog's supported set equals the platform file (regenerate if not)")
        let fileIds = (repoJSON("contracts/control.manifest.json")?["Actions"] as? [[String: Any]] ?? [])
            .compactMap { $0["Id"] as? String }
        expectEqual(ControlCatalog.all.map(\.action.id), fileIds, "all 110 entries, Windows order")
    }

    /// Authoring rule 10: drive every supported id through the REAL dispatcher.
    @MainActor
    static func testControlEverySupportedIdIsDispatched() {
        let model = AppModel()   // no core: actions refuse, but none may fall through
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        guard let ids = fileSupportedIds() else { expect(false, "platform file"); return }
        for id in ids.sorted() {
            guard let action = ControlCatalog.action(id) else { expect(false, "\(id) is in the catalog"); continue }
            let args: [Any?] = action.params.map {
                switch $0.type { case .bool: return true; case .string: return "s1"; case .int: return 1; case .double: return 0.5 }
            }
            let result = invoke(surface, id, args)
            expect(result != nil, "\(id) answered within 3 s")
            expect(result?.error != AppModelControlSurface.undispatched(id), "\(id) has a dispatch case")
        }
        // The fallthrough text is exactly what this test looks for (a confirmatory pin).
        expectEqual(invoke(surface, "zz.not.a.case")?.error, "Unknown action 'zz.not.a.case'.", "default branch text")
    }

    @MainActor
    static func testControlUnavailableActionsAnswer422() {
        guard let platforms = repoJSON("contracts/control.platforms.json"),
              let rows = platforms["actions"] as? [[String: Any]],
              let labels = platforms["workstreams"] as? [String: String] else { expect(false, "platform file"); return }
        let surface = FakeSurface()
        let router = ControlRouter(surface: surface)
        var checked = 0
        for row in rows where row["mac"] as? String != "supported" {
            let id = row["id"] as? String ?? ""
            let expected = row["mac"] as? String == "blocked"
                ? "'\(id)' is not available on macOS yet (\(labels[row["blockedOn"] as? String ?? ""] ?? "?"))."
                : "'\(id)' is not available on macOS (\(row["reason"] as? String ?? "?"))."
            let response = route(router, "POST", "/invoke", #"{"action":"\#(id)","args":[]}"#)
            expectEqual(response.status, 422, "\(id) is 422")
            expectEqual(json(response.body)["error"] as? String, expected, "\(id) says why, in the operator's words")
            checked += 1
        }
        expectEqual(checked, 74, "110 - 36 unavailable ids checked (each slice updates this)")
        expectEqual(surface.invocations.count, 0, "an unavailable action never reaches the surface")
        let bogus = route(router, "POST", "/invoke", #"{"action":"bogus.id"}"#)
        expectEqual(bogus.status, 400, "an id in neither file keeps Windows' 400")
        expectEqual(json(bogus.body)["error"] as? String, "Unknown action 'bogus.id'.", "Windows text")
        let showEngine = route(router, "POST", "/invoke", #"{"action":"showEngine.restart"}"#)
        expectEqual(json(showEngine.body)["error"] as? String,
                    "'showEngine.restart' is not available on macOS (the OHG show engine is not part of macOS 1.0).",
                    "a hand-checked N/A sentence (fixture-derived operand)")
    }

    @MainActor
    static func testControlManifestIsTheSharedFileFiltered() {
        guard let file = repoJSON("contracts/control.manifest.json"), let ids = fileSupportedIds(),
              let feedbackRows = repoJSON("contracts/control.platforms.json")?["feedbackFields"] as? [[String: Any]] else {
            expect(false, "shared files"); return
        }
        let supportedFields = feedbackRows.filter { $0["mac"] as? String == "supported" }.compactMap { $0["field"] as? String }
        var expected = file
        expected["Actions"] = (file["Actions"] as? [[String: Any]] ?? []).filter { ids.contains($0["Id"] as? String ?? "") }
        expected["FeedbackFields"] = (file["FeedbackFields"] as? [String] ?? []).filter { supportedFields.contains($0) }
        let response = route(ControlRouter(surface: FakeSurface()), "GET", "/manifest")
        expectEqual(response.status, 200, "GET /manifest")
        expect(NSDictionary(dictionary: json(response.body)).isEqual(to: expected),
               "GET /manifest is the shared file with only the Mac-supported actions and feedback fields")
        expectEqual(supportedFields.count, 21, "RC3: twenty-one feedback fields (each slice updates this)")
        for retired in ["automation.magic", "automation.toggle", "automation.autoTake.set"] {
            expect(!response.body.contains(retired), "retired \(retired) is not advertised")
        }
    }

    struct FixtureError: LocalizedError { let errorDescription: String? }

    /// contracts/control.fixtures.json through ControlRouter and ControlSnapshotRedactor. Windows runs
    /// the same file (CT ControlFixtureTests, MCT ControlRedactionFixtureTests); editing an
    /// expectation turns both red. A missing file is a failure, never a skip.
    @MainActor
    static func testControlSharedFixtures() {
        guard let fixtures = repoJSON("contracts/control.fixtures.json"),
              let http = fixtures["http"] as? [[String: Any]],
              let snapshots = fixtures["snapshot"] as? [[String: Any]],
              let redaction = fixtures["redaction"] as? [String: Any] else {
            expect(false, "contracts/control.fixtures.json is readable from the checkout"); return
        }
        func same(_ a: Any?, _ b: Any?) -> Bool {
            if a == nil || a is NSNull { return b == nil || b is NSNull }
            return NSArray(array: [a!]).isEqual(to: [b ?? NSNull()])
        }
        var ran = 0
        for test in http where (test["hosts"] as? [String])?.contains("mac") ?? true {
            let name = test["name"] as? String ?? "?"
            let request = test["request"] as? [String: Any] ?? [:]
            let reply = test["surfaceReply"] as? [String: Any] ?? ["ok": true]
            let surface = FakeSurface()
            surface.handler = { _, _ in
                ControlInvokeResult(ok: reply["ok"] as? Bool ?? false, error: reply["error"] as? String,
                                    previewSceneId: reply["previewSceneId"] as? String, data: reply["data"] as? String)
            }
            let response = route(ControlRouter(surface: surface), request["method"] as? String ?? "",
                                 request["path"] as? String ?? "", request["body"] as? String)
            let expected = test["expect"] as? [String: Any] ?? [:]
            expectEqual(response.status, expected["status"] as? Int, "\(name): status")
            var body = json(response.body)
            if let prefix = expected["errorPrefix"] as? String {
                expect((body["error"] as? String ?? "").hasPrefix(prefix), "\(name): error starts with \(prefix)")
                body["error"] = nil
            }
            if let want = expected["body"] as? [String: Any] {
                expect(NSDictionary(dictionary: body).isEqual(to: want), "\(name): body \(response.body)")
            }
            if let want = expected["invocations"] as? [[String: Any]] {
                let got = surface.invocations.map { ["action": $0.0, "args": $0.1.map { $0 ?? NSNull() }] as [String: Any] }
                expect(NSArray(array: got).isEqual(to: want), "\(name): invocations \(got)")
            }
            ran += 1
        }
        for test in snapshots {
            let name = test["name"] as? String ?? "?"
            let observation = test["observation"] as? [String: Any] ?? [:]
            let surface = FakeSurface()
            switch observation["kind"] as? String {
            case "available":
                let age = observation["ageMs"] as? Double
                surface.observation = ControlSnapshotObservation(
                    state: observation["state"] as? [String: Any],
                    receivedAt: age.map { Date().addingTimeInterval(-$0 / 1000) })
            case "unavailable": surface.observation = .unavailable(observation["reason"] as? String ?? "")
            case "notObserved": surface.observesCore = false
            default: surface.observationError = FixtureError(errorDescription: observation["message"] as? String)
            }
            let root = json(route(ControlRouter(surface: surface), "GET", "/snapshot").body)
            for (key, want) in test["expect"] as? [String: Any] ?? [:] where key != "ageMsNear" {
                // Presence first: an absent key must not pass as an expected null.
                expect(root.keys.contains(key), "\(name): \(key) is present")
                expect(same(root[key], want), "\(name): \(key) = \(String(describing: root[key]))")
            }
            if let near = (test["expect"] as? [String: Any])?["ageMsNear"] as? Double {
                expect(abs(((root["ageMs"] as? NSNumber)?.doubleValue ?? -1e9) - near) <= 400, "\(name): ageMs near \(near)")
            }
            ran += 1
        }
        for pair in redaction["names"] as? [[String: Any]] ?? [] {
            let name = pair["name"] as? String ?? ""
            expectEqual(ControlSnapshotRedactor.isSecretName(name), pair["secret"] as? Bool, "isSecretName(\(name))"); ran += 1
        }
        for pair in redaction["texts"] as? [[String: Any]] ?? [] {
            let input = pair["input"] as? String ?? ""
            expectEqual(ControlSnapshotRedactor.redactText(input), pair["expected"] as? String, "redactText(\(input))"); ran += 1
        }
        for pair in redaction["documents"] as? [[String: Any]] ?? [] {
            let got = ControlSnapshotRedactor.redact(pair["input"] ?? [:])
            expect(same(got, pair["expected"]), "redact(document) equals the fixture"); ran += 1
        }
        let osc = fixtures["osc"] as? [String: Any] ?? [:]
        func arg(_ j: [String: Any]) -> ControlOscArg {
            if let v = j["i"] as? Int { return .int(Int32(v)) }
            if let v = j["f"] as? Double { return .float(Float(v)) }
            if let v = j["s"] as? String { return .string(v) }
            if let v = j["bool"] as? Bool { return .bool(v) }
            if let v = j["blob"] as? String { return .blob(Data(hex: v)) }
            return .null
        }
        func msg(_ j: [String: Any]) -> ControlOscMessage {
            ControlOscMessage(address: j["address"] as? String ?? "", args: (j["args"] as? [[String: Any]] ?? []).map(arg))
        }
        for c in osc["encode"] as? [[String: Any]] ?? [] {
            expectEqual(ControlOscCodec.encode(msg(c["message"] as? [String: Any] ?? [:])).hexString, c["hex"] as? String, "osc encode \(c["name"] ?? "")"); ran += 1
        }
        for c in osc["decode"] as? [[String: Any]] ?? [] {
            let want = (c["messages"] as? [[String: Any]] ?? []).map(msg)
            expectEqual(ControlOscCodec.decode(Data(hex: c["hex"] as? String ?? "")), want, "osc decode \(c["name"] ?? "")"); ran += 1
        }
        expect(ControlOscCodec.decode(Data(count: ControlOscCodec.maxDatagram + 1)).isEmpty, "an oversize datagram is dropped")
        // OSC `f` arrives as Float (D14); coercion follows Windows' float cases.
        expectEqual(ControlCatalog.coerce(Float(0.1), to: .string) as? String, "0.1", "string from Float 0.1 is \"0.1\" (not the widened double)")
        expectEqual(ControlCatalog.coerce(Float(2.5), to: .int) as? Int, 2, "int from Float rounds to even")
        expectEqual(ControlCatalog.coerce(Float(0), to: .bool) as? Bool, false, "bool from Float 0 is false")
        // 36 mac http (RC1 31 + T8 int cases 5) + 6 snapshot + 21 names + 11 texts + 1 document
        // + 7 osc encode + 12 osc decode = 94 (each slice that appends cases updates this).
        expectEqual(ran, 94, "every fixture case ran")
    }

    // ── contract: /invoke statuses and errors (Windows router output) ────────

    @MainActor
    static func testControlInvokeRejectsLikeWindows() {
        let surface = FakeSurface()
        let router = ControlRouter(surface: surface)
        // The bind/route cases both hosts share are in contracts/control.fixtures.json
        // (control/fixtures). These rows are the ones that file does not carry: the
        // other two retired ids and an absent body.
        let cases: [(String?, Int, String)] = [
            (#"{"action":"automation.toggle","args":[true]}"#, 400, "Unknown action 'automation.toggle'."),
            (#"{"action":"automation.autoTake.set","args":[true]}"#, 400, "Unknown action 'automation.autoTake.set'."),
            (nil, 400, "Request body is required."),
        ]
        for (body, status, error) in cases {
            let response = route(router, "POST", "/invoke", body)
            expectEqual(response.status, status, "status for \(body ?? "nil")")
            expectEqual(json(response.body)["ok"] as? Bool, false, "ok:false for \(body ?? "nil")")
            expectEqual(json(response.body)["error"] as? String, error, "Windows error text for \(body ?? "nil")")
        }
        let invalid = route(router, "POST", "/invoke", "not json")
        expectEqual(invalid.status, 400, "invalid JSON is 400")
        expect((json(invalid.body)["error"] as? String ?? "").hasPrefix("Invalid JSON: "), "Windows 'Invalid JSON: ' prefix")
        expectEqual(surface.invocations.count, 0, "no rejected request reached the surface")
        let notFound = route(router, "GET", "/nope")
        expectEqual(notFound.status, 404, "unknown route 404")
        expectEqual(json(notFound.body)["error"] as? String, "Not found", "Windows 404 body")
        expectEqual(route(router, "GET", "/invoke").status, 404, "GET /invoke is not a route")
        expectEqual(route(router, "POST", "/state").status, 404, "POST /state is not a route")
    }

    @MainActor
    static func testControlInvokeBindsAndShapesReplies() {
        let surface = FakeSurface()
        let router = ControlRouter(surface: surface)
        // Windows TryCoerce: bool from 1 / "true" / "0"; string from a number.
        _ = route(router, "POST", "/invoke", #"{"action":"transport.ftb.set","args":[1]}"#)
        _ = route(router, "POST", "/invoke", #"{"action":"transport.record.set","args":["TRUE"]}"#)
        _ = route(router, "POST", "/invoke", #"{"action":"transport.stream.set","args":["0"]}"#)
        _ = route(router, "POST", "/invoke", #"{"action":"scene.select","args":[3, "extra ignored"]}"#)
        let calls = surface.invocations
        expectEqual(calls.count, 4, "four bound invocations")
        if calls.count == 4 {
            expectEqual(calls[0].0, "transport.ftb.set", "id passes through")
            expectEqual(calls[0].1.first as? Bool, true, "1 -> true")
            expectEqual(calls[1].1.first as? Bool, true, "\"TRUE\" -> true (bool.TryParse is case-insensitive)")
            expectEqual(calls[2].1.first as? Bool, false, "\"0\" -> false")
            expectEqual(calls[3].1.first as? String, "3", "number -> invariant string")
            expectEqual(calls[3].1.count, 1, "extra trailing args are ignored")
        }
        surface.handler = { id, _ in
            switch id {
            case "scene.select": return .appliedPreview("pip")
            case "scene.list": return .withData(#"{"scenes":[],"previewSceneId":"","activeSceneId":""}"#)
            case "transport.take": return .fail("Take is unavailable.")
            default: return .success
            }
        }
        let applied = route(router, "POST", "/invoke", #"{"action":"scene.select","args":["pip"]}"#)
        expectEqual(applied.status, 200, "applied preview 200")
        expectEqual(json(applied.body)["previewSceneId"] as? String, "pip", "reply carries previewSceneId")
        expectEqual(json(applied.body)["ok"] as? Bool, true, "ok:true")
        let data = route(router, "POST", "/invoke", #"{"action":"scene.list"}"#)
        expectEqual(data.body, #"{"ok":true,"data":{"scenes":[],"previewSceneId":"","activeSceneId":""}}"#,
                    "read action data is embedded raw (Windows string concat)")
        let failed = route(router, "POST", "/invoke", #"{"action":"transport.take"}"#)
        expectEqual(failed.status, 422, "surface failure is 422")
        expectEqual(json(failed.body)["error"] as? String, "Take is unavailable.", "failure text passes through")
        expectEqual(route(router, "POST", "/invoke", #"{"action":"transport.ftb.toggle"}"#).body,
                    #"{"ok":true}"#, "plain success body")
        let state = route(router, "GET", "/state")
        expectEqual(state.status, 200, "GET /state 200")
        expectEqual(json(state.body)["recording"] as? Bool, false, "state is the surface's camelCase state")
    }

    // ── contract: GET /snapshot (port of HttpSnapshotRouteTests) ─────────────

    static let coreSnapshot: [String: Any] = json(#"""
    {"sceneId":"scene:solo",
     "programBuffer":{"underruns":3,"overflows":0,"gpuNotReady":1,"occupancy":4,"delivered":5100,"produced":5104,"outputSequenceGaps":0},
     "encoderEvidence":{"metricVersion":"async-encoder-evidence-v1","lifecycleState":"producing","queueDepth":2,"oldestQueuedAgeMs":18,"droppedVideo":0,"droppedAudio":0,"programVideoWritten":5099},
     "realtimeEvidence":{"metricVersion":"realtime-worker-evidence-v1","render":{"observed":true,"progressAgeMs":12.5,"completedSlots":5100,"skippedSlots":1,"deadlineMisses":2},"audio":{"observed":true,"progressAgeMs":4.0,"completedTicks":9900},"videoOutput":{"observed":true,"progressAgeMs":9.0}},
     "tiles":{"layerId":"wall","members":[{"sourceId":"zoom:1","rect":{"x":0,"y":0,"width":960,"height":540}}]},
     "multiviewer":{"layoutMode":"pgmPvwTop","tileCount":8},
     "browserSources":{"sources":[]}}
    """#)

    @MainActor
    static func snapshotRoot(_ surface: FakeSurface) -> [String: Any] {
        let response = route(ControlRouter(surface: surface), "GET", "/snapshot")
        expectEqual(response.status, 200, "GET /snapshot is always 200")
        return json(response.body)
    }

    @MainActor
    static func testControlSnapshotEnvelopeIsWindows() {
        let surface = FakeSurface()
        let received = Date().addingTimeInterval(-0.25)
        surface.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: received)
        let root = snapshotRoot(surface)
        expectEqual(root["ok"] as? Bool, true, "ok")
        expectEqual(root["available"] as? Bool, true, "available")
        expectEqual(root["redacted"] as? Bool, true, "redacted:true")
        expectEqual((root["staleAfterMs"] as? NSNumber)?.doubleValue, 2000, "staleAfterMs 2000")
        let nodes = root["nodes"] as? [String] ?? []
        for node in ["encoderEvidence", "realtimeEvidence", "programBuffer", "tiles", "multiviewer"] {
            expect(nodes.contains(node), "nodes lists \(node)")
        }
        let snapshot = root["snapshot"] as? [String: Any] ?? [:]
        expectEqual(((snapshot["encoderEvidence"] as? [String: Any])?["queueDepth"] as? NSNumber)?.intValue, 2,
                    "an unbound node passes through verbatim")
        let render = (snapshot["realtimeEvidence"] as? [String: Any])?["render"] as? [String: Any]
        expectEqual((render?["deadlineMisses"] as? NSNumber)?.intValue, 2, "nested values survive")
        expectEqual((render?["progressAgeMs"] as? NSNumber)?.doubleValue, 12.5, "doubles survive")
        expectEqual((render?["observed"] as? NSNumber).map { CFGetTypeID($0) == CFBooleanGetTypeID() }, true,
                    "booleans stay booleans")
        let iso = ISO8601DateFormatter()
        iso.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let receivedUtc = iso.date(from: root["receivedUtc"] as? String ?? "")
        let servedUtc = iso.date(from: root["servedUtc"] as? String ?? "")
        expect(receivedUtc.map { abs($0.timeIntervalSince(received)) < 0.002 } ?? false,
               "receivedUtc is the shell's receipt time")
        expect(servedUtc.map { $0 >= received } ?? false, "servedUtc is request time")
        expect(((root["ageMs"] as? NSNumber)?.doubleValue ?? 0) >= 250, "ageMs measures receipt age")
        expectEqual(root["stale"] as? Bool, false, "fresh snapshot is not stale")

        surface.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: Date().addingTimeInterval(-3))
        let old = snapshotRoot(surface)
        expectEqual(old["available"] as? Bool, true, "old snapshot still available")
        expectEqual(old["stale"] as? Bool, true, "> 2 s is stale")

        surface.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: nil)
        let undated = snapshotRoot(surface)
        expectEqual(undated["stale"] as? Bool, true, "an undated snapshot is stale")
        expect(undated["ageMs"] is NSNull && undated["receivedUtc"] is NSNull, "undated: null age and receipt")
        expect(!((undated["staleReason"] as? String) ?? "").isEmpty, "undated says why")

        surface.observation = .unavailable("engine off")
        let absent = snapshotRoot(surface)
        expectEqual(absent["available"] as? Bool, false, "absent: available false")
        expectEqual(absent["stale"] as? Bool, true, "absent: stale")
        expectEqual(absent["reasonCode"] as? String, "no-snapshot", "absent: reasonCode")
        expectEqual(absent["reason"] as? String, "engine off", "absent: reason passes through")
        expect(absent["snapshot"] is NSNull, "absent: snapshot null, not {}")
        expectEqual((absent["nodes"] as? [Any])?.count, 0, "absent: nodes []")

        let unobserved = FakeSurface()
        unobserved.observesCore = false
        expectEqual(snapshotRoot(unobserved)["reasonCode"] as? String, "not-observed", "no core -> not-observed")

        let throwing = FakeSurface()
        throwing.observationError = BridgeError.remote("bridge gone")
        expectEqual(snapshotRoot(throwing)["reasonCode"] as? String, "observer-failed", "throwing observer answered")

        let counting = FakeSurface()
        counting.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: Date())
        let router = ControlRouter(surface: counting)
        _ = route(router, "GET", "/state")
        expectEqual(counting.observationReads, 0, "/state never reads the snapshot")
        _ = route(router, "GET", "/snapshot")
        expectEqual(counting.observationReads, 1, "/snapshot reads the held snapshot once (no core round-trip)")
    }

    // ── redaction (ports of CoreSnapshotObserverTests / SupportBundleLogRedactorTests) ──

    @MainActor
    static func testControlSnapshotRedactionIsWindows() {
        let state = json(#"""
        {"sceneId":"scene:solo",
         "outputSenderSession":{"status":"live","senders":[
           {"senderId":"rtmp-0","destination":"rtmp","status":"failed",
            "streamKey":"live_1234_SUPERSECRETKEY",
            "lastError":"ffmpeg exited: rtmps://live.example.com/app/live_1234_SUPERSECRETKEY",
            "runtimeDetail":"srtPassphrase=hunter2hunter2 accepted"}]},
         "browserSources":{"sources":[{"id":"b1","url":"https://scores.example.com/board?access_token=abc123def456"}],
                           "credential":{"value":"hidden"}},
         "warnings":["configure-outputs echoed {\"streamKey\":\"live_1234_SUPERSECRETKEY\"}"],
         "recording":{"programPath":"/Users/op/Movies/show-program-0.mp4"},
         "overlay":{"keyPhase":"visible","keyer":"luma","keyPosition":3,"keyLength":"16","layerId":"lt"}}
        """#)
        let surface = FakeSurface()
        surface.observation = ControlSnapshotObservation(state: state, receivedAt: Date())
        let body = route(ControlRouter(surface: surface), "GET", "/snapshot").body
        for secret in ["SUPERSECRETKEY", "hunter2hunter2", "abc123def456", "hidden"] {
            expect(!body.contains(secret), "\(secret) never leaves the process")
        }
        let snapshot = json(body)["snapshot"] as? [String: Any] ?? [:]
        let sender = ((snapshot["outputSenderSession"] as? [String: Any])?["senders"] as? [[String: Any]])?.first ?? [:]
        expectEqual(sender["senderId"] as? String, "rtmp-0", "non-secret sender fields survive")
        expectEqual(sender["status"] as? String, "failed", "status survives")
        expectEqual(sender["streamKey"] as? String, "[redacted]", "secret-NAMED value replaced")
        expectEqual(sender["lastError"] as? String, "ffmpeg exited: rtmps://live.example.com/[redacted]",
                    "rtmp path redacted inside its own string; host survives")
        expect((sender["runtimeDetail"] as? String ?? "").hasSuffix(" accepted"),
               "a redaction does not eat the rest of the value or the properties after it")
        expectEqual((snapshot["browserSources"] as? [String: Any])?["credential"] as? String, "[redacted]",
                    "a secret-named OBJECT is redacted as a unit")
        expectEqual((snapshot["recording"] as? [String: Any])?["programPath"] as? String,
                    "/Users/op/Movies/show-program-0.mp4", "recording paths are deliberately kept")
        let overlay = snapshot["overlay"] as? [String: Any] ?? [:]
        expectEqual(overlay["keyPhase"] as? String, "visible", "keyPhase survives (suffix rule)")
        expectEqual(overlay["keyer"] as? String, "luma", "keyer survives")
        expectEqual((overlay["keyPosition"] as? NSNumber)?.intValue, 3, "keyPosition survives")
        expectEqual(snapshot["sceneId"] as? String, "scene:solo", "unrelated root survives")

        // The name and text cases (CoreSnapshotObserverTests / SupportBundleLogRedactorTests)
        // live in contracts/control.fixtures.json and run in control/fixtures on both hosts.
        // #998: long ordinary strings must redact in bounded time.
        let longCases = [String(repeating: "a", count: 200_000),
                         String(repeating: "key", count: 66_666),
                         "rtmp://h/" + String(repeating: "p", count: 200_000),
                         String(repeating: "eyJ-", count: 50_000)]
        for input in longCases {
            let start = Date()
            _ = ControlSnapshotRedactor.redactText(input)
            expect(Date().timeIntervalSince(start) < 5, "long input redacts in bounded time (\(input.prefix(8))…)")
        }
        let filler = String(repeating: "a", count: 200_000)
        let around = ControlSnapshotRedactor.redactText(filler + " streamKey=abc123&x=1 " + filler)
        expect(around.contains("streamKey=[redacted]&x=1") && !around.contains("abc123"),
               "a secret inside long text is still redacted")
    }

    // ── auth & policy (ports of HttpControlServerTests) ──────────────────────

    @MainActor
    static func testControlAuthAndBindPolicyAreWindows() {
        let refused: [(String, String?)] = [("+", nil), ("*", ""), ("0.0.0.0", "   "), ("[::]", nil),
                                             ("192.168.1.20", nil), ("studio.local", nil)]
        for (host, token) in refused {
            do {
                try ControlServerOptions(port: 8011, host: host, authToken: token).validate()
                expect(false, "LAN bind \(host) without a token must refuse")
            } catch {
                expect("\(error)".contains("COREVIDEO_CONTROL_TOKEN"), "refusal names COREVIDEO_CONTROL_TOKEN (\(host))")
            }
        }
        let allowed: [(String, String?)] = [("127.0.0.1", nil), ("127.0.0.2", ""), ("localhost", nil),
                                             ("LOCALHOST", nil), ("[::1]", nil), ("+", "secret"),
                                             ("192.168.1.20", "secret")]
        for (host, token) in allowed {
            do { try ControlServerOptions(port: 8011, host: host, authToken: token).validate() }
            catch { expect(false, "\(host) with token \(token ?? "nil") is valid policy: \(error)") }
        }
        expect(ControlServerOptions(host: "127.0.0.1").isLoopback, "127.0.0.1 is loopback")
        expect(!ControlServerOptions(host: "+").isLoopback, "+ is a network bind")
        // Defaults: loopback 8011, Windows env names.
        let defaults = ControlServerOptions.fromEnvironment([:])
        expectEqual(defaults, ControlServerOptions(port: 8011, host: "127.0.0.1", authToken: nil), "default 127.0.0.1:8011")
        let lan = ControlServerOptions.fromEnvironment(["COREVIDEO_HTTP_LAN": "1", "COREVIDEO_HTTP_PORT": "9011",
                                                        "COREVIDEO_CONTROL_TOKEN": "t"])
        expectEqual(lan, ControlServerOptions(port: 9011, host: "+", authToken: "t"), "LAN opt-in from env")
        expectEqual(ControlServerOptions.fromEnvironment(["COREVIDEO_HTTP_LAN": "true", "COREVIDEO_HTTP_PORT": "70000"]),
                    ControlServerOptions(port: 8011, host: "127.0.0.1", authToken: nil),
                    "only LAN=1 opts in; an out-of-range port keeps 8011 (Windows parse)")

        // IsAuthorized: no token -> open (loopback); token -> Bearer (scheme case-insensitive),
        // query token ONLY for the /ws upgrade.
        expect(ControlAuth.isAuthorized(token: nil, authorization: nil, isWebSocketUpgrade: false, queryToken: nil),
               "no token configured: authorized")
        expect(ControlAuth.isAuthorized(token: "  ", authorization: nil, isWebSocketUpgrade: false, queryToken: nil),
               "blank token is no token (IsNullOrWhiteSpace)")
        expect(!ControlAuth.isAuthorized(token: "s3cret", authorization: nil, isWebSocketUpgrade: false, queryToken: nil),
               "missing bearer refused")
        expect(!ControlAuth.isAuthorized(token: "s3cret", authorization: "Bearer wrong", isWebSocketUpgrade: false, queryToken: nil),
               "wrong bearer refused")
        expect(!ControlAuth.isAuthorized(token: "s3cret", authorization: "Bearer s3cre", isWebSocketUpgrade: false, queryToken: nil),
               "prefix of the token refused")
        expect(ControlAuth.isAuthorized(token: "s3cret", authorization: "bearer s3cret", isWebSocketUpgrade: false, queryToken: nil),
               "scheme is case-insensitive")
        expect(!ControlAuth.isAuthorized(token: "s3cret", authorization: "Basic s3cret", isWebSocketUpgrade: false, queryToken: nil),
               "non-Bearer scheme refused")
        expect(!ControlAuth.isAuthorized(token: "s3cret", authorization: nil, isWebSocketUpgrade: false, queryToken: "s3cret"),
               "query token never authorizes an ordinary route")
        expect(ControlAuth.isAuthorized(token: "s3cret", authorization: nil, isWebSocketUpgrade: true, queryToken: "s3cret"),
               "query token authorizes the /ws upgrade")
        expect(ControlAuth.constantTimeEquals("abc", "abc") && !ControlAuth.constantTimeEquals("abc", "abd")
               && !ControlAuth.constantTimeEquals("abc", "abcd") && !ControlAuth.constantTimeEquals("", "a"),
               "constant-time equality is still equality")
        // DNS-rebinding guard on a loopback bind (stricter than Windows; see report).
        expect(ControlAuth.hostAllowed("127.0.0.1:8011", loopbackBind: true), "127.0.0.1 Host ok")
        expect(ControlAuth.hostAllowed("localhost:8011", loopbackBind: true), "localhost Host ok")
        expect(ControlAuth.hostAllowed("[::1]:8011", loopbackBind: true), "[::1] Host ok")
        expect(!ControlAuth.hostAllowed("evil.example:8011", loopbackBind: true), "rebinding Host refused")
        expect(ControlAuth.hostAllowed("studio.local:8011", loopbackBind: false), "LAN bind: token is the guard")
    }

    // ── HTTP/1.1 parsing bounds ──────────────────────────────────────────────

    @MainActor
    static func testControlHttpParserBounds() {
        func parse(_ text: String) -> ControlHttpParse { ControlHttpParser.parseHead(Data(text.utf8)) }
        guard case .head(let head, let offset) = parse("POST /invoke?x=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\nContent-Type: application/json\r\n\r\n{}")
        else { expect(false, "a complete head parses"); return }
        expectEqual(head.method, "POST", "method")
        expectEqual(head.path, "/invoke", "path without query")
        expectEqual(head.query["x"], "1", "query parsed")
        expectEqual(head.headers["content-type"], "application/json", "headers are case-insensitive")
        expectEqual(head.contentLength, 2, "content length")
        expectEqual(offset, head.target.isEmpty ? -1 : offset, "body offset reported")
        expectEqual(parse("GET /state HTTP/1.1\r\nHost: x\r\n"), .incomplete, "an unfinished head is incomplete")
        let big = "GET / HTTP/1.1\r\nX: " + String(repeating: "a", count: ControlHttpParser.maxHeaderBytes) + "\r\n\r\n"
        if case .reject(let status, _) = parse(big) { expectEqual(status, 431, "oversize head 431") }
        else { expect(false, "oversize head rejected") }
        let unfinishedBig = "GET / HTTP/1.1\r\nX: " + String(repeating: "a", count: ControlHttpParser.maxHeaderBytes + 10)
        if case .reject(let status, _) = parse(unfinishedBig) { expectEqual(status, 431, "oversize unfinished head 431") }
        else { expect(false, "oversize unfinished head rejected (no unbounded buffer)") }
        let tooLong = "POST /invoke HTTP/1.1\r\nContent-Length: \(ControlHttpParser.maxBodyBytes + 1)\r\n\r\n"
        if case .reject(let status, _) = parse(tooLong) { expectEqual(status, 413, "oversize body 413") }
        else { expect(false, "oversize body rejected before it is read") }
        for bad in ["NONSENSE\r\n\r\n", "GET\r\n\r\n", "GET /x HTTP/2.0\r\n\r\n",
                    "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                    "POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n",
                    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"] {
            if case .reject = parse(bad) { expect(true, "rejected") }
            else { expect(false, "malformed head rejected: \(bad.debugDescription)") }
        }
        expectEqual(ControlWebSocket.acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                    "RFC 6455 accept key")
    }

    // ── AppModel adapter: the same paths as the UI ───────────────────────────

    @MainActor
    static func invoke(_ surface: ControlSurface, _ id: String, _ args: [Any?] = []) -> ControlInvokeResult? {
        let box = Box<ControlInvokeResult>()
        Task { @MainActor in box.value = await surface.invoke(id, args) }
        spin(until: { box.value != nil }, limit: 3)
        return box.value
    }

    @MainActor
    static func testControlSurfaceRoutesThroughTheModel() {
        // Take: the model's own take() (one batch, swap, Windows status).
        let core = FakeTakeCore()
        let model = takeModel(core)
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(surface, "transport.take"), .success, "remote Take succeeds")
        expectEqual(core.bodies.count, 1, "remote Take is the model's one media-core-sync batch")
        expectEqual(core.bodies.isEmpty ? nil : core.types().first, "begin-take-transition", "same batch as the button")
        expectEqual(model.programSceneId, "s1", "Program is the taken scene")
        model.previewSceneId = model.programSceneId   // nothing to take
        expectEqual(invoke(surface, "transport.take"),
                    .fail("Take is unavailable. Queue a valid Preview scene or a changed draft and wait for any current Take to finish."),
                    "Windows RunTake refusal when CanTake is false")
        expectEqual(core.bodies.count, 1, "a refused Take sends nothing")

        // Scene select: refuse-closed (SceneSelectArgs), reply carries previewSceneId.
        let scenes = sceneEditModel()
        scenes.scenes.append(SceneDef(id: AppModel.soloSceneA, name: "Solo", layout: "single"))
        let sceneSurface = AppModelControlSurface(model: scenes, box: scenes.controlSnapshotBox)
        expectEqual(invoke(sceneSurface, "scene.select", ["nope"]), .fail("Unknown scene 'nope'."), "unknown scene refused")
        expectEqual(invoke(sceneSurface, "scene.select", [""]), .fail("Unknown scene ''."), "blank scene refused")
        expectEqual(scenes.previewSceneId, "s1", "a refused select changes nothing")
        expectEqual(invoke(sceneSurface, "scene.select", ["s3"]), .appliedPreview("s3"), "known scene applied")
        expectEqual(scenes.previewSceneId, "s3", "Preview is the selected scene")
        expectEqual(scenes.programSceneId, "s2", "selection never touches Program")
        let list = invoke(sceneSurface, "scene.list")
        let data = json(list?.data ?? "")
        let rows = data["scenes"] as? [[String: Any]] ?? []
        expectEqual(rows.compactMap { $0["id"] as? String }, ["s1", "s2", "s3"], "the rail only (solo scenes excluded)")
        expectEqual(rows.first?.count, 3, "rows carry id, name, layout")
        expectEqual(rows.first?["name"] as? String, "Intro", "name")
        expectEqual(rows.first?["layout"] as? String, "single", "layout")
        expectEqual(data["previewSceneId"] as? String, "s3", "previewSceneId")
        expectEqual(data["activeSceneId"] as? String, "s2", "activeSceneId is Program")

        // Record / stream: refused offline and while quitting (Windows CanSetRecording).
        let offline = sceneEditModel()
        let offlineSurface = AppModelControlSurface(model: offline, box: offline.controlSnapshotBox)
        expectEqual(invoke(offlineSurface, "transport.record.toggle"),
                    .fail("Cannot toggle recording right now (the control is unavailable in the current state)."),
                    "record toggle refused with no core")
        let quitting = takeModel(FakeTakeCore())
        quitting.quitFinishing = true
        let quitSurface = AppModelControlSurface(model: quitting, box: quitting.controlSnapshotBox)
        expectEqual(invoke(quitSurface, "transport.record.toggle"),
                    .fail("Cannot toggle recording right now (the control is unavailable in the current state)."),
                    "record toggle refused while quitting")
        expectEqual(invoke(quitSurface, "transport.stream.set", [true]),
                    .fail("Cannot set streaming right now (the control is unavailable in the current state)."),
                    "stream set refused while quitting")
        expect(!quitting.recordingDesired && !quitting.streamingDesired, "nothing started while quitting")
        let idle = takeModel(FakeTakeCore())
        let idleSurface = AppModelControlSurface(model: idle, box: idle.controlSnapshotBox)
        expectEqual(invoke(idleSurface, "transport.record.set", [false]), .success,
                    "record.set(false) while idle is an idempotent no-op (RunOutputSet)")
        idle.streamKey = ""
        let noKey = invoke(idleSurface, "transport.stream.toggle")
        expect(noKey?.ok == false, "stream start without URL/key is refused, not reported as success")
        expect(!idle.streamingDesired, "no stream intent was armed")

        // State: the fields /state and /ws publish.
        let stateBox = Box<[String: Any]>()
        Task { @MainActor in stateBox.value = await sceneSurface.controlState() }
        spin(until: { stateBox.value != nil }, limit: 3)
        let state = stateBox.value ?? [:]
        expectEqual(state["activeSceneId"] as? String, "s2", "state activeSceneId")
        expectEqual(state["previewSceneId"] as? String, "s3", "state previewSceneId")
        expectEqual(state["canTake"] as? Bool, true, "state canTake")
        expectEqual(state["programBlackout"] as? Bool, false, "state programBlackout")
        expectEqual(state["recording"] as? Bool, false, "state recording")
        expectEqual(state["automationOn"] as? Bool, false, "scene automation is retired: false")
        for field in ControlCatalog.feedbackFields {
            // input/{slot}/* are the rows of `inputs` (Windows ControlState.Inputs).
            let key = field.hasPrefix("input/") ? "inputs" : field
            expect(state[key] != nil, "advertised feedback field \(field) is published")
        }
    }

    @MainActor
    static func testControlSurfaceVirtualCameraActions() {
        let model = AppModel()
        model.cameraExtension = FakeCameraExtensionActivator(script: [.active])
        var sent: [JSONObject] = []
        model.coreCommandCaptureForTesting = { sent.append($0) }
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        func syncs() -> Int {
            sent.flatMap { ($0["commands"] as? [JSONObject]) ?? [] }
                .filter { $0["type"] as? String == "sync-virtual-camera" }.count
        }
        expectEqual(invoke(surface, "transport.virtualcam.set", [true]), .success, "set true succeeds")
        expect(model.virtualCamera.enabled, "set true arms the intent")
        expectEqual(syncs(), 1, "one sync for the change")
        expectEqual(invoke(surface, "transport.virtualcam.set", [true]), .success, "set true again succeeds")
        expect(model.virtualCamera.enabled, "set true twice does not toggle off")
        expectEqual(syncs(), 1, "a repeated set sends nothing (per change, not per call)")
        expectEqual(invoke(surface, "transport.virtualcam.toggle"), .success, "toggle succeeds")
        expect(!model.virtualCamera.enabled, "toggle flips the intent off")
        expectEqual(syncs(), 2, "toggle sends one sync")
        expectEqual(invoke(surface, "transport.virtualcam.set", [false]), .success, "set false when off")
        expectEqual(syncs(), 2, "set false when already off sends nothing")
        expectEqual(invoke(surface, "transport.virtualcam.mirror.set", [true]), .success, "mirror on")
        expect(model.virtualCamera.mirror, "mirror intent")
        expectEqual(syncs(), 3, "mirror change sends one sync")
        _ = invoke(surface, "transport.virtualcam.mirror.set", [true])
        expectEqual(syncs(), 3, "repeated mirror sends nothing")
        expectEqual(invoke(surface, "transport.virtualcam.name.set", ["Studio Cam"]), .success, "name set")
        expectEqual(model.virtualCamera.deviceName, "Studio Cam", "name stored")
        expectEqual(syncs(), 4, "name change sends one sync")
        _ = invoke(surface, "transport.virtualcam.name.set", ["Studio Cam"])
        expectEqual(syncs(), 4, "unchanged name sends nothing")
        expectEqual(invoke(surface, "transport.virtualcam.name.set"), .success, "omitted name is blank")
        expectEqual(model.virtualCamera.deviceName, "", "blank name restores the default")
        // Every registered id is dispatchable (no id falls through to 'Unknown action').
        for action in ControlCatalog.actions {
            let args: [Any?] = action.params.map { $0.type == .bool ? true : ($0.type == .string ? "s1" : 0) }
            let r = invoke(surface, action.id, args)
            if (r?.error ?? "").hasPrefix("Unknown action") { expect(false, "\(action.id) is not dispatched") }
        }
    }

    @MainActor
    static func testControlSurfaceFtbAndSnapshot() {
        let core = FakeFtbCore()
        let model = ftbModel(core)
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(surface, "transport.ftb.set", [true]), .success, "ftb.set(true)")
        expectEqual(core.commands.last?["enabled"] as? Bool, true, "explicit target sent")
        expect(model.programBlackout, "the echo was applied by the model")
        expectEqual(invoke(surface, "transport.ftb.toggle"), .success, "ftb.toggle")
        core.pollSnapshot = ["recording": [:]]
        expectEqual(invoke(surface, "transport.ftb.set", [true]),
                    .fail("This media core does not support Fade to Black."), "FTB admission refusal passes through")

        // The snapshot the model applied is what /snapshot serves, with its receipt time.
        let fresh = sceneEditModel()
        let freshSurface = AppModelControlSurface(model: fresh, box: fresh.controlSnapshotBox)
        let before = (try? freshSurface.snapshotObservation())
        expect(before?.state == nil && before?.unavailableReason != nil, "before any sync: unavailable with a reason")
        fresh.applySnapshotForTesting(["sceneId": "scene:solo", "outputSenderSession": ["senders": [["streamKey": "SECRETKEY99"]]]])
        let after = try? freshSurface.snapshotObservation()
        expectEqual(after?.state?["sceneId"] as? String, "scene:solo", "the applied snapshot is observed")
        expect(after?.receivedAt.map { abs($0.timeIntervalSinceNow) < 2 } ?? false, "receipt time recorded")
        let body = route(ControlRouter(surface: freshSurface), "GET", "/snapshot").body
        expect(body.contains("scene:solo") && !body.contains("SECRETKEY99"), "served through the redactor")
        expect(!ControlApiHost.shouldAutoStart(environment: ["COREVIDEO_SHELL_TESTS": "1"]), "never auto-starts in the test harness")
        expect(!ControlApiHost.shouldAutoStart(environment: ["COREVIDEO_SHELL_SELFCHECK": "1"]), "never auto-starts in selfcheck")
        expect(!ControlApiHost.shouldAutoStart(environment: ["COREVIDEO_SHELL_SNAPSHOT": "/tmp/x.png"]), "never auto-starts in a snapshot proof run")
        expect(ControlApiHost.shouldAutoStart(environment: [:]), "auto-starts with the app")
    }

    // ── end to end over real sockets ─────────────────────────────────────────

    struct HttpResult { var status: Int; var body: String; var headers: [String: String] }

    @MainActor
    static func http(_ method: String, _ url: String, body: String? = nil,
                     headers: [String: String] = [:]) -> HttpResult {
        var request = URLRequest(url: URL(string: url)!)
        request.httpMethod = method
        request.timeoutInterval = 5
        for (name, value) in headers { request.setValue(value, forHTTPHeaderField: name) }
        if let body { request.httpBody = Data(body.utf8) }
        let box = Box<HttpResult>()
        let session = URLSession(configuration: .ephemeral)
        session.dataTask(with: request) { data, response, error in
            let http = response as? HTTPURLResponse
            var fields: [String: String] = [:]
            for (name, value) in http?.allHeaderFields ?? [:] {
                fields["\(name)".lowercased()] = "\(value)"
            }
            box.value = HttpResult(status: http?.statusCode ?? -1,
                                   body: data.map { String(decoding: $0, as: UTF8.self) } ?? "\(error.map { "\($0)" } ?? "")",
                                   headers: fields)
        }.resume()
        spin(until: { box.value != nil }, limit: 6)
        session.invalidateAndCancel()
        return box.value ?? HttpResult(status: -2, body: "timeout", headers: [:])
    }

    /// Raw TCP: send bytes exactly, read until the server closes.
    final class RawClient: @unchecked Sendable {
        let connection: NWConnection
        let box = Box<String>()
        private var received = Data()
        private let lock = NSLock()
        var soFar: String { lock.lock(); defer { lock.unlock() }; return String(decoding: received, as: UTF8.self) }
        init(port: UInt16) {
            connection = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
        }
        func run(_ payload: Data, halfClose: Bool = false) {
            connection.stateUpdateHandler = { [self] state in
                if case .ready = state {
                    connection.send(content: payload, contentContext: halfClose ? .finalMessage : .defaultMessage, isComplete: true, completion: .contentProcessed { _ in })
                    read()
                }
                if case .failed(let error) = state { box.value = box.value ?? "failed: \(error) after \(received.count) bytes" }
            }
            connection.start(queue: DispatchQueue(label: "control-test-raw"))
        }
        private func read() {
            connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [self] data, _, complete, error in
                if let data { lock.lock(); received.append(data); lock.unlock() }
                if complete || error != nil { box.value = String(decoding: received, as: UTF8.self) }
                else { read() }
            }
        }
    }

    @MainActor
    static func raw(_ port: UInt16, _ payload: Data) -> String {
        let client = RawClient(port: port)
        client.run(payload)
        spin(until: { client.box.value != nil }, limit: 6)
        client.connection.cancel()
        return client.box.value ?? "timeout"
    }

    @MainActor
    static func startServer(_ router: ControlRouter, _ options: ControlServerOptions) -> (ControlHttpServer, Result<UInt16, ControlServerError>?) {
        let server = ControlHttpServer(router: router, options: options)
        let box = Box<Result<UInt16, ControlServerError>>()
        server.start { box.value = $0 }
        spin(until: { box.value != nil }, limit: 3)
        return (server, box.value)
    }

    // ── hardening: the Mac's stricter-than-Windows rules stay pinned (T5) ──

    @MainActor
    static func testControlHardeningTimeouts() {
        let surface = FakeSurface()
        var options = ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil)
        options.requestTimeout = 0.4
        let (server, started) = startServer(ControlRouter(surface: surface), options)
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        // A client that sends half a head and stalls is answered 408 by the request timer.
        let stalled = RawClient(port: port)
        stalled.run(Data("GET /state HTTP/1.1\r\nHost: 127.0.0.1\r\n".utf8))
        spin(until: { stalled.box.value != nil }, limit: 3)
        stalled.connection.cancel()
        expect((stalled.box.value ?? "").hasPrefix("HTTP/1.1 408"), "a stalled request is 408: \((stalled.box.value ?? "").prefix(20))")
        // The timer stops once the request reaches the router: an invoke that runs 3x the timeout is answered 200.
        surface.handler = { _, _ in Thread.sleep(forTimeInterval: 1.2); return .success }
        let slow = raw(port, Data("POST /invoke HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"action\":\"transport.take\"}".utf8))
        expect(slow.hasPrefix("HTTP/1.1 200"), "a long invoke is not cut off by the request timer: \(slow.prefix(20))")
        expectEqual(ControlServerOptions.fromEnvironment([:]).requestTimeout, 10, "the default is 10 s")
    }

    @MainActor
    static func testControlHardeningConnectionCap() {
        var options = ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil)
        options.requestTimeout = 5   // idle clients must outlive the test
        let (server, started) = startServer(ControlRouter(surface: FakeSurface()), options)
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        let queue = DispatchQueue(label: "control-test-cap")
        var idle: [NWConnection] = []
        for _ in 0..<64 {
            let c = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
            c.start(queue: queue)
            idle.append(c)
        }
        spin(until: { server.openConnectionCount == 64 }, limit: 3)
        expectEqual(server.openConnectionCount, 64, "64 idle connections are held")
        let refused = raw(port, Data("GET /state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".utf8))
        expect(!refused.hasPrefix("HTTP/1.1"), "the 65th connection is closed without an answer: \(refused.prefix(30))")
        expectEqual(server.openConnectionCount, 64, "the refused connection was never admitted")
        idle.forEach { $0.cancel() }
        spin(until: { server.openConnectionCount == 0 }, limit: 3)
        expectEqual(http("GET", "http://127.0.0.1:\(port)/state").status, 200, "serving again once clients leave")
    }

    @MainActor
    static func testControlHardeningNoPreflight() {
        let (server, started) = startServer(ControlRouter(surface: FakeSurface()),
                                            ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil))
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        let preflight = raw(port, Data("OPTIONS /invoke HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://evil.example\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type\r\n\r\n".utf8))
        expect(preflight.hasPrefix("HTTP/1.1 404"), "a CORS preflight is not a route")
        expect(!preflight.lowercased().contains("access-control-"), "no Access-Control-* header on any answer")
        for path in ["/manifest", "/state", "/snapshot", "/nope"] {
            let response = raw(port, Data("GET \(path) HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://evil.example\r\n\r\n".utf8))
            expect(!response.lowercased().contains("access-control-"), "\(path): no CORS header even with an Origin")
        }
    }

    @MainActor
    static func testControlServerEndToEnd() {
        let surface = FakeSurface()
        surface.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: Date())
        let router = ControlRouter(surface: surface)
        let (server, started) = startServer(router, ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil))
        defer { server.stop() }
        guard case .success(let port)? = started, port != 0 else { expect(false, "server starts on an ephemeral loopback port: \(String(describing: started))"); return }
        let base = "http://127.0.0.1:\(port)"
        let manifest = http("GET", base + "/manifest")
        expectEqual(manifest.status, 200, "GET /manifest over the wire")
        expect(manifest.body.contains("transport.take"), "manifest body")
        expectEqual(manifest.headers["content-type"], "application/json; charset=utf-8", "Windows content type")
        expect(manifest.headers["access-control-allow-origin"] == nil, "no CORS wildcard")
        expectEqual(http("GET", base + "/state").status, 200, "GET /state over the wire")
        let snapshot = json(http("GET", base + "/snapshot").body)
        expectEqual(snapshot["available"] as? Bool, true, "GET /snapshot over the wire")
        let take = http("POST", base + "/invoke", body: #"{"action":"transport.take","args":[]}"#,
                        headers: ["Content-Type": "application/json"])
        expectEqual(take.status, 200, "POST /invoke over the wire")
        expectEqual(surface.invocations.last?.0, "transport.take", "the action reached the surface")
        let retired = http("POST", base + "/invoke", body: #"{"action":"automation.magic","args":[true]}"#,
                           headers: ["Content-Type": "application/json"])
        expectEqual(retired.status, 400, "retired id 400 over the wire")
        let plain = http("POST", base + "/invoke", body: #"{"action":"transport.take"}"#,
                         headers: ["Content-Type": "text/plain"])
        expectEqual(plain.status, 415, "a browser 'simple' POST (no JSON content type) is refused")
        expectEqual(surface.invocations.count, 1, "refused requests never invoke")
        // Oversize: header claims a body over the bound; refused before it is read.
        let oversize = raw(port, Data("POST /invoke HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 70000\r\n\r\n{}".utf8))
        expect(oversize.hasPrefix("HTTP/1.1 413"), "oversize body 413 over the wire: \(oversize.prefix(40))")
        let bigHead = raw(port, Data(("GET /state HTTP/1.1\r\nX: " + String(repeating: "a", count: 20_000) + "\r\n\r\n").utf8))
        expect(bigHead.hasPrefix("HTTP/1.1 431"), "oversize head 431 over the wire: \(bigHead.prefix(40))")
        let rebound = raw(port, Data("GET /snapshot HTTP/1.1\r\nHost: evil.example:\(port)\r\n\r\n".utf8))
        expect(rebound.hasPrefix("HTTP/1.1 403") && !rebound.contains("realtimeEvidence"),
               "DNS-rebinding Host refused on a loopback bind")
        // Bytes after a complete body (pipelining / a hostile client) must never
        // dispatch the action again while the first one is still running.
        surface.handler = { _, _ in Thread.sleep(forTimeInterval: 0.3); return .success }
        let before = surface.invocations.count
        let pipelined = RawClient(port: port)
        let request = "POST /invoke HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"action\":\"transport.take\"}"
        pipelined.run(Data(request.utf8))
        spin(until: { surface.invocations.count > before }, limit: 3)
        pipelined.connection.send(content: Data(String(repeating: "x", count: 4096).utf8), completion: .contentProcessed { _ in })
        pipelined.connection.send(content: Data(String(repeating: "y", count: 4096).utf8), completion: .contentProcessed { _ in })
        spin(until: { pipelined.box.value != nil }, limit: 4)
        pipelined.connection.cancel()
        expect((pipelined.box.value ?? "").hasPrefix("HTTP/1.1 200"), "the pipelined request is answered once")
        expectEqual(surface.invocations.count, before + 1, "extra bytes after the body never re-dispatch the action")
        surface.handler = { _, _ in .success }
        // A client that half-closes after its request (nc, some tooling) still gets its answer.
        let halfClosed = RawClient(port: port)
        halfClosed.run(Data(request.utf8), halfClose: true)
        spin(until: { halfClosed.box.value != nil }, limit: 4)
        halfClosed.connection.cancel()
        expect((halfClosed.box.value ?? "").hasPrefix("HTTP/1.1 200"),
               "a half-closed request is still answered: \((halfClosed.box.value ?? "").prefix(30))")
        let garbage = raw(port, Data("HELLO\r\n\r\n".utf8))
        expect(garbage.hasPrefix("HTTP/1.1 400"), "garbage answered 400, server survives")
        expectEqual(http("GET", base + "/state").status, 200, "server still serving after bad requests")

        // WebSocket: state on connect, pushed on change (Companion's feedback channel).
        let session = URLSession(configuration: .ephemeral)
        let socket = session.webSocketTask(with: URL(string: "ws://127.0.0.1:\(port)/ws")!)
        socket.resume()
        let first = Box<String>()
        socket.receive { if case .success(.string(let text)) = $0 { first.value = text } else { first.value = "error" } }
        spin(until: { first.value != nil }, limit: 5)
        expectEqual(json(first.value ?? "")["recording"] as? Bool, false, "WS sends the current state on connect")
        let second = Box<String>()
        socket.receive { if case .success(.string(let text)) = $0 { second.value = text } else { second.value = "error" } }
        server.broadcastState(#"{"recording":true,"activeSceneId":"live"}"#)
        spin(until: { second.value != nil }, limit: 5)
        expectEqual(json(second.value ?? "")["activeSceneId"] as? String, "live", "a state change is pushed")
        socket.cancel(with: .normalClosure, reason: nil)
        session.invalidateAndCancel()
        let originWs = raw(port, Data("GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nOrigin: https://evil.example\r\n\r\n".utf8))
        expect(originWs.hasPrefix("HTTP/1.1 403"), "a browser (Origin) WebSocket is refused")

        // Port in use: reported, not a crash.
        let (second8011, clash) = startServer(router, ControlServerOptions(port: port, host: "127.0.0.1", authToken: nil))
        second8011.stop()
        if case .failure(.portInUse)? = clash { expect(true, "port in use") }
        else { expect(false, "a taken port is reported as port-in-use: \(String(describing: clash))") }
        // Stop releases the port.
        server.stop()
        expectEqual(http("GET", base + "/state").status < 0, true, "stopped server no longer answers")
    }

    @MainActor
    static func testControlServerAuthOverTheWire() {
        let surface = FakeSurface()
        surface.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: Date())
        let router = ControlRouter(surface: surface)
        // LAN without a token never listens.
        let (lanServer, lan) = startServer(router, ControlServerOptions(port: 0, host: "+", authToken: nil))
        lanServer.stop()
        if case .failure(.policy(let why))? = lan { expect(why.contains("COREVIDEO_CONTROL_TOKEN"), "LAN refusal names the token") }
        else { expect(false, "LAN without token refuses to start: \(String(describing: lan))") }
        // Token configured (loopback): every route needs it, snapshot included.
        let (server, started) = startServer(router, ControlServerOptions(port: 0, host: "127.0.0.1", authToken: "s3cret"))
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "token server starts"); return }
        let base = "http://127.0.0.1:\(port)"
        let denied = http("GET", base + "/snapshot")
        expectEqual(denied.status, 401, "no bearer: 401")
        expect(!denied.body.contains("realtimeEvidence"), "401 carries no snapshot")
        expectEqual(json(denied.body)["error"] as? String, "Unauthorized", "Windows 401 body")
        expectEqual(http("GET", base + "/snapshot", headers: ["Authorization": "Bearer wrong"]).status, 401, "bad token 401")
        let invoked = http("POST", base + "/invoke?token=s3cret", body: #"{"action":"transport.take"}"#,
                           headers: ["Content-Type": "application/json"])
        expectEqual(invoked.status, 401, "query token never authorizes POST /invoke")
        expectEqual(surface.invocations.count, 0, "an unauthorized POST never invokes")
        expectEqual(http("GET", base + "/snapshot", headers: ["Authorization": "Bearer s3cret"]).status, 200, "good token 200")
        let ok = http("POST", base + "/invoke", body: #"{"action":"transport.take"}"#,
                      headers: ["Content-Type": "application/json", "Authorization": "bearer s3cret"])
        expectEqual(ok.status, 200, "authorized POST")
        expectEqual(surface.invocations.count, 1, "authorized POST invokes once")
    }

    /// A source that fires every 20 ms for 0.6 s never goes quiet for 100 ms:
    /// debounce emits nothing until it stops, a throttle keeps pushing.
    @MainActor
    static func testControlStateStreamIsAThrottleNotADebounce() {
        let source = PassthroughSubject<Int, Never>()
        let count = Box<Int>()
        count.value = 0
        let sink = ControlApiHost.stateChangeStream(source).sink { _ in count.value = (count.value ?? 0) + 1 }
        let end = Date().addingTimeInterval(0.6)
        var i = 0
        while Date() < end {
            source.send(i); i += 1
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        let during = count.value ?? 0
        expect(during >= 3, "a never-quiet source still pushes while it runs (got \(during))")
        spin(until: { false }, limit: 0.3)
        sink.cancel()
    }

    /// Menu tracking, live resize and the .terminateLater quit window run the main
    /// run loop in a NON-default mode. RunLoop.main as a Combine scheduler only fires
    /// in .default, so WebSocket feedback froze exactly while the operator was
    /// holding a menu open or the app was finishing a recording on quit.
    @MainActor
    static func testControlStateStreamDeliversDuringEventTracking() {
        // Precondition, established explicitly: DispatchQueue.main drains only in the
        // main run loop's COMMON modes. AppKit adds .eventTracking to them when
        // NSApplication connects to the window server, which a GUI session always
        // does but this harness (run from the App initializer on a headless CI
        // runner) may not have yet. Idempotent; mirrors what AppKit registers.
        CFRunLoopAddCommonMode(CFRunLoopGetMain(), CFRunLoopMode(RunLoop.Mode.eventTracking.rawValue as CFString))
        let source = PassthroughSubject<Int, Never>()
        let count = Box<Int>()
        count.value = 0
        let sink = ControlApiHost.stateChangeStream(source).sink { _ in count.value = (count.value ?? 0) + 1 }
        let end = Date().addingTimeInterval(0.6)
        var i = 0
        while Date() < end {
            source.send(i); i += 1
            RunLoop.main.run(mode: .eventTracking, before: Date().addingTimeInterval(0.02))
        }
        let during = count.value ?? 0
        expect(during >= 3, "state pushes keep flowing while the run loop is in eventTracking mode (got \(during))")
        spin(until: { false }, limit: 0.3)
        sink.cancel()
    }

    @MainActor
    static func testControlWebSocketOriginRule() {
        let surface = FakeSurface()
        surface.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: Date())
        let router = ControlRouter(surface: surface)
        func upgrade(_ query: String, origin: Bool) -> Data {
            Data(("GET /ws\(query) HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                + (origin ? "Origin: http://studio.local\r\n" : "") + "\r\n").utf8)
        }
        let (tokenServer, started) = startServer(router, ControlServerOptions(port: 0, host: "127.0.0.1", authToken: "s3cret"))
        defer { tokenServer.stop() }
        guard case .success(let port)? = started else { expect(false, "token server starts"); return }
        func status(_ payload: Data) -> String {
            let client = RawClient(port: port)
            client.run(payload)
            spin(until: { !client.soFar.isEmpty }, limit: 2)
            client.connection.cancel()
            return String(client.soFar.prefix(12))
        }
        expectEqual(status(upgrade("?token=s3cret", origin: true)), "HTTP/1.1 101", "browser WS with a valid token is allowed")
        expectEqual(status(upgrade("?token=wrong", origin: true)), "HTTP/1.1 401", "browser WS with a wrong token is 401")
        let (openServer, openStarted) = startServer(router, ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil))
        defer { openServer.stop() }
        guard case .success(let openPort)? = openStarted else { expect(false, "open server starts"); return }
        let client = RawClient(port: openPort)
        client.run(upgrade("", origin: true))
        spin(until: { client.box.value != nil }, limit: 2)
        client.connection.cancel()
        expect((client.box.value ?? "").hasPrefix("HTTP/1.1 403"), "Origin with NO token configured (open mode) is refused")
    }

    /// A client that never reads must be dropped once its outbound queue is
    /// over the cap, not grow without bound.
    @MainActor
    static func testControlSlowWebSocketClientIsDropped() {
        let surface = FakeSurface()
        surface.observation = ControlSnapshotObservation(state: coreSnapshot, receivedAt: Date())
        let (server, started) = startServer(ControlRouter(surface: surface), ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil))
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        let slow = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
        slow.stateUpdateHandler = { state in
            if case .ready = state {
                let req = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
                slow.send(content: Data(req.utf8), completion: .contentProcessed { _ in })   // and never receive
            }
        }
        slow.start(queue: DispatchQueue(label: "control-test-slow"))
        spin(until: { server.webSocketClientCount == 1 }, limit: 3)
        expectEqual(server.webSocketClientCount, 1, "the slow client is connected")
        let big = #"{"pad":""# + String(repeating: "x", count: 60_000) + #""}"#
        for _ in 0..<600 { server.broadcastState(big) }
        spin(until: { server.webSocketClientCount == 0 }, limit: 5)
        expectEqual(server.webSocketClientCount, 0, "a client that never reads is dropped at the outbound cap")
        slow.cancel()
    }

    @MainActor
    static func testControlServerDrivesTheRealModel() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let (server, started) = startServer(ControlRouter(surface: surface),
                                            ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil))
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        let base = "http://127.0.0.1:\(port)"
        let take = http("POST", base + "/invoke", body: #"{"action":"transport.take","args":[]}"#,
                        headers: ["Content-Type": "application/json"])
        expectEqual(take.status, 200, "Take over a real socket")
        expectEqual(core.bodies.count, 1, "the fake core received the model's Take batch")
        expectEqual(model.programSceneId, "s1", "the model took")
        let select = json(http("POST", base + "/invoke", body: #"{"action":"scene.select","args":["s3"]}"#,
                               headers: ["Content-Type": "application/json"]).body)
        expectEqual(select["previewSceneId"] as? String, "s3", "scene.select over the wire")
        expectEqual(model.previewSceneId, "s3", "Preview armed")
        let state = json(http("GET", base + "/state").body)
        expectEqual(state["activeSceneId"] as? String, "s1", "/state reflects the model")
    }
}
