// macOS control parity, Phase 2 (docs/superpowers/plans/2026-11-21-mac-control-audio-phase2.md):
// RC6 Volt rundown and RC5 Companion. ContractParity skips *Tests.swift files.

import Foundation

extension ShellTests {

    @MainActor
    static var controlPhase2Cases: [(String, () -> Void)] {
        [
            ("control/volt-classify", testVoltRundownClassify),
            ("control/volt-load", testVoltRundownLoad),
            ("control/volt-preview", testVoltRundownPreview),
            ("control/volt-take", testVoltRundownTake),
            ("control/companion-request-shapes", testCompanionRequestShapesAgainstTheMacServer),
            // Review fixes.
            ("control/route-set-refusal-changes-nothing", testRemoteRouteRefusalChangesNothingLocally),
        ]
    }

    /// Stages `<bin>/Volt Rundown/<name>` in the harness-isolated bin.
    static func stageVoltFile(_ name: String) {
        let path = MediaBin.root + "/" + MediaBin.voltRundownFolder + "/" + name
        try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                                 withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: path, contents: Data("staged".utf8))
    }

    /// Windows MediaBinClassifier + MediaBinServiceTests.LoadGroups_SeparatesVoltRundownCuts...
    static func testVoltRundownClassify() {
        expectEqual(MediaBin.classify(fileName: "a.mp4", relativePath: "Volt Rundown/a.mp4"), "volt-rundown", "staged mp4")
        expectEqual(MediaBin.classify(fileName: "slate-a.mp4", relativePath: "Volt Rundown/slate-a.mp4"), "volt-rundown",
                    "the Volt check runs before the slate check")
        expectEqual(MediaBin.classify(fileName: "a.mov", relativePath: "Volt Rundown/a.mov"), "stinger", ".mov is not a Volt clip")
        expectEqual(MediaBin.kindLabel("volt-rundown"), "Volt Rundown", "label")
        stageVoltFile("package-a123.mp4")
        stageVoltFile(".volt-working/partial.mp4")
        let volt = MediaBin.loadAssets().filter { $0.kind == "volt-rundown" }
        expectEqual(volt.map(\.name), ["package-a123"], "the staged clip is listed; the working folder is not")
        expectEqual(volt.first?.id, MediaBin.assetId(relativePath: "Volt Rundown/package-a123.mp4"), "Windows asset id")
    }

    @MainActor
    static func testVoltRundownLoad() {
        let model = AppModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        for bad in ["Pkg", "pkg_a", "", String(repeating: "a", count: 73), "../x"] {
            expectEqual(invoke(s, "media.rundown.load", [bad]), .fail("Invalid staged clip key."), "bad key \(bad)")
        }
        expectEqual(invoke(s, "media.rundown.load", ["never-staged"]),
                    .fail("The staged clip is missing from the local Volt Rundown bin."), "missing clip")
        stageVoltFile("show-open.mp4")
        let id = MediaBin.assetId(relativePath: "Volt Rundown/show-open.mp4")
        expectEqual(invoke(s, "media.rundown.load", ["show-open"]), .withData(#"{"sourceId":"media:\#(id)"}"#),
                    "load refreshes the bin and answers the source id")
        expect(String(repeating: "a", count: 72).count == 72 && AppModel.isVoltClipKey(String(repeating: "a", count: 72)),
               "72 characters is a valid key")
    }

    @MainActor
    static func voltModel() -> (AppModel, FakeTakeCore) {
        let core = FakeTakeCore()
        let model = takeModel(core)   // Program s2, Preview s1
        stageVoltFile("clip-1.mp4")
        model.refreshMediaBin()
        if let index = model.scenes.firstIndex(where: { $0.id == "s1" }) {
            model.scenes[index].layers = [
                SceneLayer(id: "L1", slotId: 1), SceneLayer(id: "L2", slotId: 2),
                SceneLayer(id: "L3", slotId: 3, opacity: 0),
            ]
        }
        return (model, core)
    }

    @MainActor
    static func testVoltRundownPreview() {
        let (model, _) = voltModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let clipId = MediaBin.assetId(relativePath: "Volt Rundown/clip-1.mp4")
        expectEqual(invoke(s, "media.rundown.preview", ["gone", "s1", "L2"]),
                    .fail("The staged clip is missing. Send it from Volt again."), "missing")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s2", "L2"]),
                    .fail("The clip scene must be off Program before cueing another clip."), "scene on Program")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "nope", ""]),
                    .fail("The clip scene or its editable video route does not exist."), "unknown scene")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", ""]),
                    .fail("The clip scene or its editable video route does not exist."), "two eligible layers: ambiguous")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", "L3"]),
                    .fail("The clip scene or its editable video route does not exist."), "an invisible layer is not eligible")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", "L2"]),
                    .fail("Clip playback is unavailable in this build"), "Mac: no clip playback in the core")
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]] as JSONObject)
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", "L2"]), .appliedPreview("s1"), "cued")
        let layers = model.scenes.first { $0.id == "s1" }?.layers ?? []
        expectEqual(layers.first { $0.id == "L2" }?.mediaAssetId, clipId, "the named layer carries the clip")
        expect(layers.first { $0.id == "L2" }?.slotId == nil, "and no slot")
        expectEqual(layers.first { $0.id == "L1" }?.slotId, 1, "the other layers are untouched")
        expect(layers.first { $0.id == "L1" }?.mediaAssetId == nil, "no clip on L1")
        expectEqual(model.previewSceneId, "s1", "the scene is on Preview")
    }

    @MainActor
    static func testVoltRundownTake() {
        let (model, core) = voltModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let clipId = MediaBin.assetId(relativePath: "Volt Rundown/clip-1.mp4")
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]] as JSONObject)
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("The staged Volt clip must be cued on Preview before Take."), "not cued yet")
        _ = invoke(s, "media.rundown.preview", ["clip-1", "s1", "L2"])
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("Wait for the staged clip to decode on Preview before Take."), "no decoded frame yet")
        func row(onProgram: Bool, onPreview: Bool, frames: Int) -> JSONObject {
            ["mediaSources": [["sourceId": "media:\(clipId)", "mediaAssetId": clipId, "state": "cued", "loop": false,
                               "onProgram": onProgram, "onPreview": onPreview, "positionMs": 0, "durationMs": 1000,
                               "decodedVideoFrames": frames]]] as JSONObject
        }
        model.applySnapshotForTesting(row(onProgram: false, onPreview: true, frames: 0))
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("Wait for the staged clip to decode on Preview before Take."), "0 decoded frames")
        model.applySnapshotForTesting(row(onProgram: true, onPreview: true, frames: 5))
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("The staged clip is already on Program. Take it off Program before replaying it."), "already on Program")
        // syncScenes is debounced, so a scene sync may still land later: the
        // check is that no refusal started a Take, which is never deferred.
        expect(!core.bodies.indices.contains { core.types($0).contains("begin-take-transition") },
               "no refusal started a Take")
        model.applySnapshotForTesting(row(onProgram: false, onPreview: true, frames: 5))
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]), .success, "ready: the ordinary Take runs")
        expect(core.bodies.indices.contains { core.types($0).first == "begin-take-transition" }, "the button's Take batch")
        expectEqual(model.programSceneId, "s1", "the clip scene is on Program")
    }

    // ── RC5: the Companion module's requests against the Mac server ──────────

    /// The headers Node's global fetch (undici) sends, plus the module's own
    /// (companion-module-corevideopro/src/main.ts headers(): Content-Type on EVERY
    /// request, Bearer when a token is configured).
    @MainActor
    static func companionHttp(_ method: String, _ path: String, port: UInt16, host: String = "127.0.0.1",
                              body: String? = nil, token: String? = nil, contentType: Bool = true) -> String {
        var head = "\(method) \(path) HTTP/1.1\r\nhost: \(host):\(port)\r\nconnection: keep-alive\r\n"
        if contentType { head += "content-type: application/json\r\n" }
        if let token { head += "authorization: Bearer \(token)\r\n" }
        head += "accept: */*\r\naccept-language: *\r\nsec-fetch-mode: cors\r\nuser-agent: node\r\naccept-encoding: gzip, deflate\r\n"
        let bytes = Data((body ?? "").utf8)
        if body != nil { head += "content-length: \(bytes.count)\r\n" }
        return raw(port, Data((head + "\r\n").utf8) + bytes)
    }

    /// The `ws` package's upgrade (main.ts connectWebSocket): no Origin, a
    /// permessage-deflate offer, `?token=` only when a token is configured.
    @MainActor
    static func companionWebSocketFirstFrame(port: UInt16, query: String = "") -> (status: String, frame: [String: Any]?) {
        let upgrade = "GET /ws\(query) HTTP/1.1\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            + "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"
            + "Host: 127.0.0.1:\(port)\r\n\r\n"
        let client = RawClient(port: port)
        client.run(Data(upgrade.utf8))
        func parse() -> (String, [String: Any]?) {
            let all = client.bytesSoFar
            guard let end = all.range(of: Data("\r\n\r\n".utf8)) else { return ("", nil) }
            let status = String(decoding: all[..<end.lowerBound], as: UTF8.self).components(separatedBy: "\r\n").first ?? ""
            let frame = [UInt8](all[end.upperBound...])
            guard frame.count >= 2, frame[0] == 0x81 else { return (status, nil) }
            var length = Int(frame[1] & 0x7f), offset = 2
            if length == 126, frame.count >= 4 { length = Int(frame[2]) << 8 | Int(frame[3]); offset = 4 }
            else if length == 127, frame.count >= 10 { length = frame[2..<10].reduce(0) { $0 << 8 | Int($1) }; offset = 10 }
            guard frame.count >= offset + length else { return (status, nil) }
            let object = try? JSONSerialization.jsonObject(with: Data(frame[offset..<(offset + length)])) as? [String: Any]
            return (status, object)
        }
        spin(until: { parse().1 != nil }, limit: 3)
        client.connection.cancel()
        let result = parse()
        return (result.0, result.1)
    }

    static func body(_ response: String) -> [String: Any] {
        guard let range = response.range(of: "\r\n\r\n") else { return [:] }
        return (try? JSONSerialization.jsonObject(with: Data(response[range.upperBound...].utf8)) as? [String: Any]) ?? [:]
    }

    /// The seven presets (companion-module-corevideopro/src/presets.ts) and the
    /// feedback flags they light. The module's own vitest (src/mac.test.ts) checks
    /// this list against buildPresets() over the recorded fixture this case writes.
    static let companionPresetActions = ["transport.ftb.toggle", "transport.take", "transport.record.toggle",
                                         "transport.stream.toggle", "transport.engine.toggle",
                                         "automation.autoAssignInputs.set", "graphics.lowerThird.toggle"]
    static let companionPresetFeedbacks = ["programBlackout", "canTake", "recording", "streaming", "engineOn",
                                           "autoAssignInputs", "lowerThirdOnAir"]

    @MainActor
    static func testCompanionRequestShapesAgainstTheMacServer() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let (server, started) = startServer(ControlRouter(surface: surface),
                                            ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil))
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }

        // fetchManifest: GET /manifest WITH Content-Type (the module sends it on GETs too).
        let manifestResponse = companionHttp("GET", "/manifest", port: port)
        expect(manifestResponse.hasPrefix("HTTP/1.1 200"), "GET /manifest with the module's headers is 200")
        let manifest = body(manifestResponse)
        let ids = Set((manifest["Actions"] as? [[String: Any]] ?? []).compactMap { $0["Id"] as? String })
        expectEqual(ids.count, 43, "the Mac manifest lists its 43 supported actions (PascalCase; quick wins +audio.zoomMode.set, +autoassign; RC7 +srt.configure)")
        for action in companionPresetActions {
            expect(ids.contains(action), "preset action \(action) is in the Mac manifest (all seven, since the quick wins)")
        }

        // invokeAction: POST /invoke, positional args, Content-Type.
        let take = companionHttp("POST", "/invoke", port: port, body: #"{"action":"transport.take","args":[]}"#)
        expect(take.hasPrefix("HTTP/1.1 200"), "the take preset invokes: \(take.prefix(20))")
        expectEqual(core.bodies.count, 1, "and reached the model's Take")
        let autoassign = companionHttp("POST", "/invoke", port: port,
                                       body: #"{"action":"automation.autoAssignInputs.set","args":[true]}"#)
        expect(autoassign.hasPrefix("HTTP/1.1 200"), "the autoassign preset invokes over the module's HTTP shape")
        expect(model.autoAssignInputsEnabled, "and the model's toggle is on")
        let autoassignOff = companionHttp("POST", "/invoke", port: port,
                                          body: #"{"action":"automation.autoAssignInputs.set","args":[false]}"#)
        expect(autoassignOff.hasPrefix("HTTP/1.1 200"), "turning it off invokes too")
        expect(!model.autoAssignInputsEnabled, "the toggle follows the Companion button")
        expectEqual(model.currentPrefs().autoAssignInputs, false, "and is saved as the operator's choice")
        // The module's Content-Type is load-bearing: the Mac refuses an invoke without it.
        expect(companionHttp("POST", "/invoke", port: port, body: #"{"action":"transport.take","args":[]}"#,
                             contentType: false).hasPrefix("HTTP/1.1 415"), "no Content-Type is 415 (Mac hardening)")
        // A LAN address typed into the module while the Mac binds loopback: refused (owner doc note).
        expect(companionHttp("GET", "/manifest", port: port, host: "192.168.1.20").hasPrefix("HTTP/1.1 403"),
               "Host naming a LAN address on a loopback bind is 403 (DNS-rebinding guard)")

        // connectWebSocket: the first frame is the whole ControlState.
        let ws = companionWebSocketFirstFrame(port: port)
        expectEqual(ws.status, "HTTP/1.1 101 Switching Protocols", "the ws package's upgrade is accepted")
        let state = ws.frame ?? [:]
        for field in companionPresetFeedbacks {
            expect(state[field] is Bool, "preset feedback \(field) is a Bool in the first frame")
        }
        expectEqual(state["autoAssignInputs"] as? Bool, false, "autoAssignInputs reports the toggle the preset just set")

        // With a token: Bearer on HTTP, ?token= on /ws (main.ts).
        let (tokenServer, tokenStarted) = startServer(ControlRouter(surface: surface),
                                                      ControlServerOptions(port: 0, host: "127.0.0.1", authToken: "s3cret"))
        defer { tokenServer.stop() }
        if case .success(let tokenPort)? = tokenStarted {
            expect(companionHttp("GET", "/manifest", port: tokenPort, token: "s3cret").hasPrefix("HTTP/1.1 200"), "Bearer accepted")
            expect(companionHttp("GET", "/manifest", port: tokenPort).hasPrefix("HTTP/1.1 401"), "no token is 401")
            expectEqual(companionWebSocketFirstFrame(port: tokenPort, query: "?token=s3cret").status,
                        "HTTP/1.1 101 Switching Protocols", "?token= on the upgrade")
        } else { expect(false, "token server starts") }

        // The recorded fixture the module's vitest reads (drift-checked here).
        let fixture: [String: Any] = [
            "note": "Written by the Mac shell harness case control/companion-request-shapes "
                + "(COREVIDEO_WRITE_COMPANION_FIXTURE=1 rewrites it). Do not edit by hand.",
            "manifest": manifest,
            "stateKeys": state.keys.sorted(),
        ]
        let encoded = (try? JSONSerialization.data(withJSONObject: fixture, options: [.prettyPrinted, .sortedKeys])) ?? Data()
        let relative = "companion-module-corevideopro/src/fixtures/mac-control.json"
        if ProcessInfo.processInfo.environment["COREVIDEO_WRITE_COMPANION_FIXTURE"] == "1" {
            let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent().deletingLastPathComponent()
            let url = root.appendingPathComponent(relative)
            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? (encoded + Data("\n".utf8)).write(to: url)
        }
        let onDisk = repoFile(relative).flatMap { try? JSONSerialization.jsonObject(with: $0) as? NSDictionary }
        expect(onDisk?.isEqual(to: fixture) == true,
               "\(relative) matches what the Mac serves (rerun with COREVIDEO_WRITE_COMPANION_FIXTURE=1)")

        // Optional live leg: the module's OWN compiled code (fetchManifest, invokeAction,
        // the ws client) against this server. Needs Node and a built module copy.
        let env = ProcessInfo.processInfo.environment
        if let script = env["COREVIDEO_COMPANION_LIVE_SCRIPT"], let dist = env["COREVIDEO_COMPANION_MODULE_DIR"] {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
            process.arguments = ["node", script, "--module", dist, "--base", "http://127.0.0.1:\(port)", "--allow-take"]
            let pipe = Pipe(); process.standardOutput = pipe; process.standardError = pipe
            do { try process.run() } catch { expect(false, "live leg: node runs (\(error))"); return }
            spin(until: { !process.isRunning }, limit: 20)
            let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            FileHandle.standardError.write(Data(output.utf8))
            expectEqual(process.terminationStatus, 0, "live leg: the module's own code passes against the Mac server")
        }
    }

    // ── Review fix 1: a refused remote route edit changes nothing ────────────

    /// Windows refuses a stale audio.route.set and changes nothing. The Mac used
    /// to write the grid, the operator intent and the UNREVISIONED base matrix
    /// before the core judged the revision, so a refused edit landed ~100 ms later
    /// anyway and was re-sent on every core respawn.
    @MainActor
    static func testRemoteRouteRefusalChangesNothingLocally() {
        let fake = FakeRouteCore()
        let model = AppModel()
        model.controlSettleSeconds = 0.4
        model.quitRequestForTesting = { body, _ in await MainActor.run { fake.handle(body) } }
        model.status = .connected(renderer: "test")
        var basePushes: [JSONObject] = []
        model.routingBaseSendObserver = { basePushes.append($0) }
        model.applySnapshotForTesting(fake.handle([:])["snapshot"] as? JSONObject ?? [:])   // core-1 / revision 2
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "audio.route.set", ["zoom:43", "iso-1", true, 0, "core-1", 2, "op-a"]), .success, "iso-1 applied")
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", true, -6, "core-1", 3, "op-b"]), .success, "mon applied")
        spin(until: { false }, limit: 0.3)
        let pushesBefore = basePushes.count
        func baseGain(_ source: String, _ bus: String) -> Double? {
            (model.routingBaseCommand()?["sends"] as? [JSONObject] ?? [])
                .first { $0["sourceId"] as? String == source && $0["busId"] as? String == bus }?["gainDb"] as? Double
        }
        // Stale revision 2 (the core is at 4): conflict.
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", true, -3, "core-1", 2, "op-c"]),
                    .fail("Audio route conflict at revision 4; read state and retry."), "stale gain edit conflicts")
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "iso-1", true, 0, "core-1", 2, "op-d"]),
                    .fail("Audio route conflict at revision 4; read state and retry."), "stale ISO edit conflicts")
        spin(until: { false }, limit: 0.3)
        expectEqual(baseGain("zoom:42", "mon"), -6, "the base matrix keeps the applied gain, not the refused one")
        expect(baseGain("zoom:42", "iso-1") == nil, "the refused ISO route is not in the base matrix")
        expectEqual(baseGain("zoom:43", "iso-1"), 0, "the ISO-exclusive rule did not unroute the other source")
        expectEqual(model.audioSends["zoom:42"]?["mon"], -6, "the grid shows the applied gain")
        expect(model.audioSends["zoom:42"]?["iso-1"] == nil, "the grid does not show the refused route")
        expectEqual(model.audioSends["zoom:43"]?["iso-1"], 0, "the other ISO source stays routed")
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", false, 0, "core-1", 2, "op-e"]),
                    .fail("Audio route conflict at revision 4; read state and retry."), "stale un-route conflicts")
        spin(until: { false }, limit: 0.3)
        expectEqual(model.audioSends["zoom:42"]?["mon"], -6, "a refused un-route leaves the route in the grid")
        expectEqual(baseGain("zoom:42", "mon"), -6, "and in the base matrix")
        expectEqual(basePushes.count, pushesBefore, "a refusal pushes no base matrix")
        model.applySnapshotForTesting(fake.handle([:])["snapshot"] as? JSONObject ?? [:])
        expect(model.audioSends["zoom:42"]?["iso-1"] == nil, "no retained draft resurfaces on the next snapshot")
    }
}
