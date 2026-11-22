// Graphics G1 Task 4 — the graphics snapshot decode and the gateway.
// Vectors: contracts/graphics-shell/{gateway,state}.vectors.json, exported by
// native-shell/CoreVideoPro.MediaCore.Tests/GraphicsShellVectorTests.cs.
// Scripted-core ports of MC.Tests/GraphicsCommandGatewayTests.cs.

import Foundation

extension ShellTests {
    static var graphicsGatewayCases: [(String, () -> Void)] {
        [
            ("graphics/gateway-vectors", testGraphicsGatewayVectors),
            ("graphics/state-vectors", testGraphicsStateVectors),
            ("graphics/top-level-keys", testGraphicsTopLevelKeys),
            ("graphics/gateway-unavailable-sends-nothing", testGraphicsGatewayUnavailableSendsNothing),
            ("graphics/gateway-refusal-and-uncorrelated", testGraphicsGatewayRefusalAndUncorrelated),
            ("graphics/gateway-busy-then-recovers", testGraphicsGatewayBusyThenRecovers),
            ("graphics/gateway-correlates-in-history", testGraphicsGatewayCorrelatesInOperationsHistory),
            ("graphics/gateway-social-client-refused", testGraphicsGatewaySocialClientRefused),
        ]
    }

    // ── harness ────────────────────────────────────────────────────────────

    static var repoRoot: URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    }

    static func loadGraphicsVectors(_ area: String) -> [JSONObject] {
        let url = repoRoot.appendingPathComponent("contracts/graphics-shell/\(area).vectors.json")
        guard let data = try? Data(contentsOf: url),
              let document = try? JSONSerialization.jsonObject(with: data) as? JSONObject,
              let rows = document["rows"] as? [JSONObject], !rows.isEmpty
        else { expect(false, "\(url.path) must be readable and non-empty"); return [] }
        return rows
    }

    static func loadGraphicsNode(_ relative: String) -> JSONObject {
        let url = repoRoot.appendingPathComponent("contracts/graphics-shell/" + relative)
        guard let data = try? Data(contentsOf: url), let node = try? JSONSerialization.jsonObject(with: data) as? JSONObject
        else { expect(false, "\(url.path) must be readable"); return [:] }
        return node
    }

    /// Deep JSON equality (NSNumber-aware, key-order-free).
    static func jsonEqual(_ a: Any?, _ b: Any?) -> Bool {
        switch (a, b) {
        case (nil, nil): return true
        case let (x as NSObject, y as NSObject): return x.isEqual(y)
        default: return false
        }
    }

    /// Runs an async main-actor body to completion on the harness run loop.
    @MainActor
    static func awaitMain<T>(_ limit: TimeInterval = 5, _ body: @escaping @MainActor () async -> T) -> T? {
        var out: T?
        Task { @MainActor in out = await body() }
        spin(until: { out != nil }, limit: limit)
        return out
    }

    static let gatewayObservedNode: JSONObject = [
        "schemaVersion": 1, "coreEpoch": "epoch-1",
        "instances": [["instanceId": "featured", "revision": 3, "templateId": "compact-solid", "templateVersion": 1]],
        "templates": [] as [Any],
    ]

    static func cueRequest(id: String = "req-1", client: String = "graphics-playout") -> JSONObject {
        ["schemaVersion": 1, "requestId": id, "clientId": client, "coreEpoch": "epoch-1",
         "instanceId": "featured", "expectedRevision": 3, "rideTake": true]
    }

    /// A reply carrying one operation result for `command` (GraphicsCommandGatewayTests.Ack).
    static func graphicsAck(_ body: JSONObject, accepted: Bool = true, reason: String = "", requestId: String? = nil,
                            epoch: String = "epoch-1", extraOperations: [JSONObject] = []) -> JSONObject {
        let commands = body["commands"] as? [JSONObject] ?? []
        let graphics = commands.first?["graphics"] as? JSONObject ?? [:]
        let result: JSONObject = ["requestId": requestId ?? graphics["requestId"] as? String ?? "",
                                  "clientId": graphics["clientId"] as? String ?? "",
                                  "instanceId": graphics["instanceId"] as? String ?? "",
                                  "accepted": accepted, "reason": reason, "currentRevision": 4]
        return ["snapshot": ["graphics": ["schemaVersion": 1, "coreEpoch": epoch, "instances": [] as [Any],
                                          "operations": [result] + extraOperations, "lastResult": extraOperations.last ?? result]]]
    }

    // ── vectors ────────────────────────────────────────────────────────────

    static func testGraphicsGatewayVectors() {
        var checked = 0
        for row in loadGraphicsVectors("gateway") {
            let id = row["id"] as? String ?? "?"
            guard let input = row["input"] as? JSONObject, let expected = row["expected"] as? JSONObject,
                  let action = input["action"] as? String, let request = input["request"] as? String,
                  let observed = input["observed"] as? JSONObject
            else { expect(false, "gateway row \(id) malformed"); continue }
            let result = GraphicsCommandRequest.build(action: action, request: request, observed: GraphicsState(node: observed))
            let ok = expected["ok"] as? Bool ?? false
            switch result {
            case .success(let built):
                expect(ok, "gateway \(id): Mac accepted what Windows refuses (\(expected["error"] ?? ""))")
                if ok {
                    expectEqual(built.wire["type"] as? String, expected["type"] as? String, "gateway \(id): wire type")
                    expect(jsonEqual(built.wire["graphics"], expected["graphics"]), "gateway \(id): body forwarded unchanged")
                }
            case .failure(let refusal):
                expect(!ok, "gateway \(id): Mac refused (\(refusal.error)) what Windows accepts")
                if !ok {
                    expectEqual(refusal.error, expected["error"] as? String ?? "", "gateway \(id): refusal reason")
                    expectEqual(refusal.requestId, expected["requestId"] as? String ?? "", "gateway \(id): reported requestId")
                }
            }
            checked += 1
        }
        expect(checked >= 40, "every gateway vector replayed (\(checked))")
    }

    static func testGraphicsStateVectors() {
        var checked = 0
        for row in loadGraphicsVectors("state") {
            let id = row["id"] as? String ?? "?"
            guard let input = row["input"] as? JSONObject, let expected = row["expected"] as? JSONObject,
                  let strip = expected["strip"] as? JSONObject
            else { expect(false, "state row \(id) malformed"); continue }
            let node = (input["node"] as? JSONObject) ?? (input["nodeFile"] as? String).map(loadGraphicsNode) ?? [:]
            let state = GraphicsState(node: node)
            expectEqual(state.isUsable, expected["available"] as? Bool ?? false, "state \(id): available")
            expectEqual(state.instances.count, expected["instanceCount"] as? Int ?? -1, "state \(id): instances")
            expectEqual(state.templates.count, expected["templateCount"] as? Int ?? -1, "state \(id): templates")
            let projected = GraphicsStrip.project(state, rows: [])
            expectEqual(projected.coreEpoch, strip["coreEpoch"] as? String ?? "", "state \(id): strip epoch")
            for (name, items) in [("onProgram", projected.onProgram), ("onPreview", projected.onPreview)] {
                let want = strip[name] as? [JSONObject] ?? []
                expectEqual(items.count, want.count, "state \(id): \(name) chip count")
                for (got, wanted) in zip(items, want) {
                    expectEqual(got.instanceId, wanted["instanceId"] as? String ?? "", "state \(id): \(name) instance")
                    expectEqual(got.label, wanted["label"] as? String ?? "", "state \(id): \(name) label")
                    expectEqual(got.owner, wanted["owner"] as? String ?? "", "state \(id): \(name) owner")
                    expectEqual(got.rowId, wanted["rowId"] as? String ?? "", "state \(id): \(name) row")
                    expectEqual(got.sourceId, wanted["sourceId"] as? String ?? "", "state \(id): \(name) source")
                    expectEqual(got.revision, (wanted["revision"] as? NSNumber)?.int64Value ?? -1, "state \(id): \(name) revision")
                    expectEqual(got.visibility, wanted["visibility"] as? String ?? "", "state \(id): \(name) visibility")
                    expectEqual(got.cuePending, wanted["cuePending"] as? Bool ?? false, "state \(id): \(name) cuePending")
                }
            }
            checked += 1
        }
        expect(checked >= 5, "every state vector replayed (\(checked))")
    }

    static func testGraphicsTopLevelKeys() {
        expectEqual(GraphicsJSON.topLevelKeys(#"{"b":1,"a":{"x":[1,{"y":"}"}]},"b":"q\"","c":null}"#) ?? [],
                    ["b", "a", "b", "c"], "order and duplicates kept; nested and quoted braces skipped")
        expectEqual(GraphicsJSON.topLevelKeys(#"  { }  "#) ?? ["x"], [], "empty object")
        expectEqual(GraphicsJSON.topLevelKeys(#"{"ab":true}"#) ?? [], ["ab"], "escaped key decoded")
        expect(GraphicsJSON.topLevelKeys("[1,2]") == nil, "not an object")
    }

    // ── scripted core (GraphicsCommandGatewayTests ports) ──────────────────

    @MainActor
    static func testGraphicsGatewayUnavailableSendsNothing() {
        var sends = 0
        let send: (JSONObject) async throws -> JSONObject = { body in sends += 1; return graphicsAck(body) }
        let usable = GraphicsState(node: gatewayObservedNode)
        let cases: [(String, Bool, GraphicsState?)] = [
            ("no running core / no capability", false, usable),
            ("no snapshot yet", true, nil),
            ("schema 0", true, GraphicsState(node: ["schemaVersion": 0, "coreEpoch": "epoch-1"])),
            ("empty epoch", true, GraphicsState(node: ["schemaVersion": 1, "coreEpoch": ""])),
        ]
        for (what, capable, state) in cases {
            let gateway = GraphicsGateway(capable: { capable }, observe: { state }, send: send)
            expect(!gateway.available, "\(what): unavailable")
            let result = awaitMain { await gateway.invoke("graphics.instance.cue", cueRequest()) }
            expectEqual(result?.reason, GraphicsCommandRequest.unavailable, "\(what): refused unavailable")
        }
        expectEqual(sends, 0, "an unavailable gateway never sends (StoppedOrOldCoreIsNotLaunchedByAnInvocation)")
        // Epoch conflict: observed epoch-2, request epoch-1.
        let moved = GraphicsGateway(capable: { true }, observe: { GraphicsState(node: ["schemaVersion": 1, "coreEpoch": "epoch-2"]) },
                                    send: send)
        expectEqual(awaitMain { await moved.invoke("graphics.instance.cue", cueRequest()) }?.reason, "core-epoch-conflict",
                    "a request for an old core epoch is refused before sending")
        expectEqual(sends, 0, "epoch conflict sends nothing")
    }

    @MainActor
    static func testGraphicsGatewayRefusalAndUncorrelated() {
        let observed = GraphicsState(node: gatewayObservedNode)
        var wire: [JSONObject] = []
        let refusing = GraphicsGateway(capable: { true }, observe: { observed }, send: { body in
            wire.append(body); return graphicsAck(body, accepted: false, reason: "revision-conflict")
        })
        let refused = awaitMain { await refusing.invoke("graphics.instance.cue", cueRequest()) }
        expectEqual(refused?.accepted, false, "a native refusal stays a refusal")
        expectEqual(refused?.reason, "revision-conflict", "with the core's reason verbatim")
        expectEqual(wire.first?["type"] as? String, "media-core-sync", "one media-core-sync")
        let commands = wire.first?["commands"] as? [JSONObject] ?? []
        expectEqual(commands.count, 1, "carrying exactly one command")
        expectEqual(commands.first?["type"] as? String, "graphics-instance-cue", "dots become dashes")
        expectEqual((commands.first?["graphics"] as? JSONObject)?["rideTake"] as? Bool, true, "body unchanged")

        let mismatched = GraphicsGateway(capable: { true }, observe: { observed },
                                         send: { body in graphicsAck(body, requestId: "another-request") })
        expectEqual(awaitMain { await mismatched.invoke("graphics.instance.cue", cueRequest()) }?.reason,
                    "graphics-result-not-correlated", "a result for another request is never success")
        let epochMoved = GraphicsGateway(capable: { true }, observe: { observed },
                                         send: { body in graphicsAck(body, epoch: "epoch-9") })
        expectEqual(awaitMain { await epochMoved.invoke("graphics.instance.cue", cueRequest()) }?.reason,
                    "graphics-result-not-correlated", "an accepted result from a NEW core epoch is not ours")
        let failing = GraphicsGateway(capable: { true }, observe: { observed },
                                      send: { _ in throw NSError(domain: "pipe", code: 32) })
        let transport = awaitMain { await failing.invoke("graphics.instance.cue", cueRequest()) }
        expect(transport?.accepted == false && transport?.reason.hasPrefix("graphics-transport-failed") == true,
               "a transport failure is a refusal, never success")
        expect(!failing.inFlight, "and releases the in-flight gate")
    }

    @MainActor
    static func testGraphicsGatewayBusyThenRecovers() {
        let observed = GraphicsState(node: gatewayObservedNode)
        var sends = 0
        var release: CheckedContinuation<Void, Never>?
        var hold = true
        let gateway = GraphicsGateway(capable: { true }, observe: { observed }, send: { body in
            sends += 1
            if hold { await withCheckedContinuation { release = $0 } }
            return graphicsAck(body)
        })
        var first: GraphicsOperationResult?
        Task { @MainActor in first = await gateway.invoke("graphics.instance.cue", cueRequest()) }
        spin(until: { release != nil }, limit: 2)
        expect(gateway.inFlight, "first command in flight")
        let second = awaitMain { await gateway.invoke("graphics.instance.cue", cueRequest(id: "request-two")) }
        expectEqual(second?.reason, "graphics-command-overloaded", "a second intent while one is in flight is refused")
        expectEqual(second?.requestId, "request-two", "naming its own request")
        expectEqual(sends, 1, "and never sent")
        hold = false
        release?.resume()
        spin(until: { first != nil }, limit: 2)
        expectEqual(first?.accepted, true, "the first completes")
        let third = awaitMain { await gateway.invoke("graphics.instance.cue", cueRequest(id: "request-three")) }
        expectEqual(third?.accepted, true, "the gateway recovers after the first admission")
        expectEqual(sends, 2, "two sends in all")
    }

    @MainActor
    static func testGraphicsGatewayCorrelatesInOperationsHistory() {
        let observed = GraphicsState(node: gatewayObservedNode)
        // Another client's result landed after ours: lastResult is theirs, ours is in operations[].
        let other: JSONObject = ["requestId": "someone-else", "clientId": "control-api", "accepted": false, "reason": "x"]
        let gateway = GraphicsGateway(capable: { true }, observe: { observed },
                                      send: { body in graphicsAck(body, extraOperations: [other]) })
        let result = awaitMain { await gateway.invoke("graphics.instance.cue", cueRequest()) }
        expectEqual(result?.accepted, true, "our result is found in operations[] despite an intervening lastResult")
        expectEqual(result?.requestId, "req-1", "and it is ours")
        // Same requestId from ANOTHER client in history must not be taken as ours.
        let impostor: JSONObject = ["requestId": "req-1", "clientId": "control-api", "accepted": true, "reason": ""]
        let confused = GraphicsGateway(capable: { true }, observe: { observed }, send: { _ in
            ["snapshot": ["graphics": ["schemaVersion": 1, "coreEpoch": "epoch-1", "operations": [impostor], "lastResult": impostor]]]
        })
        expectEqual(awaitMain { await confused.invoke("graphics.instance.cue", cueRequest()) }?.reason,
                    "graphics-result-not-correlated", "a same-id result from another client is not correlated")
    }

    @MainActor
    static func testGraphicsGatewaySocialClientRefused() {
        var sends = 0
        let gateway = GraphicsGateway(capable: { true }, observe: { GraphicsState(node: gatewayObservedNode) },
                                      send: { body in sends += 1; return graphicsAck(body) })
        let result = awaitMain { await gateway.invoke("graphics.instance.cue", cueRequest(client: GraphicsGateway.socialClientId)) }
        expectEqual(result?.reason, "social-graphics-require-operator-review", "the public gateway never speaks as social review")
        expectEqual(sends, 0, "and sends nothing")
    }
}
