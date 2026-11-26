// Social review against a REAL core (plan 2026-11-22 Task 14 Step 5).
// COREVIDEO_SHELL_LIVE_CORE=<native/build-metal/corevideo-native> runs it;
// otherwise it skips loudly. Same harness as the graphics live check: its own
// core through MediaCoreBridge, never the owner's app, Program judged from the
// core's exported IOSurface pixels.
//   1. a social instance under any other client id is refused by the CORE and
//      Program is pixel-identical before and after;
//   2. an approved YouTube card lands on Program (inside its lower-centre rect),
//      not on Preview, and is gone again after the core's 15 s on-air limit;
//   3. the Twitch card draws text (the Segoe UI substitution works on Metal).

import Foundation

extension ShellTests {
    static var socialLiveCases: [(String, () -> Void)] {
        [("social-live/fail-closed-and-card-pixels-against-a-real-core", testSocialLiveCore)]
    }

    final class LiveSurfaces: @unchecked Sendable { var connected = false; var preview: UInt32 = 0 }

    /// Fraction of changed pixels inside a canvas-space rect (1920x1080 basis).
    static func changedFraction(_ a: [UInt8], _ b: [UInt8], width: Int, height: Int, rect: CGRect) -> Double {
        guard a.count == b.count, a.count == width * height * 4 else { return 1 }
        let sx = Double(width) / 1920, sy = Double(height) / 1080
        let x0 = max(0, Int(rect.minX * sx)), x1 = min(width, Int(rect.maxX * sx))
        let y0 = max(0, Int(rect.minY * sy)), y1 = min(height, Int(rect.maxY * sy))
        var changed = 0, total = 0
        for y in y0..<y1 {
            for x in x0..<x1 {
                let i = (y * width + x) * 4
                total += 1
                if abs(Int(a[i]) - Int(b[i])) > 24 || abs(Int(a[i + 1]) - Int(b[i + 1])) > 24 || abs(Int(a[i + 2]) - Int(b[i + 2])) > 24 {
                    changed += 1
                }
            }
        }
        return total == 0 ? 0 : Double(changed) / Double(total)
    }

    /// Fraction of bright (text) pixels inside a canvas-space rect.
    static func brightFraction(_ a: [UInt8], width: Int, height: Int, rect: CGRect) -> Double {
        let sx = Double(width) / 1920, sy = Double(height) / 1080
        let x0 = max(0, Int(rect.minX * sx)), x1 = min(width, Int(rect.maxX * sx))
        let y0 = max(0, Int(rect.minY * sy)), y1 = min(height, Int(rect.maxY * sy))
        var bright = 0, total = 0
        for y in y0..<y1 { for x in x0..<x1 {
            let i = (y * width + x) * 4
            total += 1
            if Int(a[i]) + Int(a[i + 1]) + Int(a[i + 2]) > 3 * 160 { bright += 1 }
        } }
        return total == 0 ? 0 : Double(bright) / Double(total)
    }

    // The card: 1600x232, anchor lower-centre, y -54 (contracts/graphics-social).
    static let socialCardRect = CGRect(x: 160, y: 1080 - 54 - 232, width: 1600, height: 232)
    static let socialTextRect = CGRect(x: 160 + 38, y: 1080 - 54 - 232 + 57, width: 1524, height: 155)

    @MainActor
    static func testSocialLiveCore() {
        guard let corePath = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_LIVE_CORE"], !corePath.isEmpty else {
            FileHandle.standardError.write(Data("  [skip] social live check: set COREVIDEO_SHELL_LIVE_CORE=<corevideo-native>\n".utf8))
            return
        }
        func log(_ line: String) { FileHandle.standardError.write(Data("  [social-live] \(line)\n".utf8)) }
        let fonts = ProcessInfo.processInfo.environment["COREVIDEO_GRAPHICS_FONTS_DIR"]
            ?? repoRoot.appendingPathComponent("native/resources/graphics-fonts").path
        let paths = ShellPaths.Paths(corePath: corePath, engineBinaryPath: nil, pluginHostPath: nil, graphicsFontsDirectory: fonts)
        let bridge = MediaCoreBridge(corePath: corePath,
                                     environmentExtras: CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil))
        let live = LiveSurfaces()
        bridge.onStatus = { if case .connected = $0 { live.connected = true } }
        bridge.onEvent = { event in
            if event["type"] as? String == "preview-shared-texture", let id = event["iosurfaceId"] as? NSNumber {
                live.preview = id.uint32Value
            }
        }
        bridge.start()
        defer { bridge.stop() }
        spin(until: { live.connected }, limit: 30)
        guard live.connected else { expect(false, "social live: core did not connect"); return }

        let app = AppModel()
        app.quitRequestForTesting = { body, timeout in try await bridge.request(body, timeout: timeout) }
        app.status = .connected(renderer: "metal")
        app.onConnected()
        app.stopTimersForTesting()
        var program: UInt32 = 0
        @discardableResult func absorb(_ reply: JSONObject) -> JSONObject {
            let snapshot = reply["snapshot"] as? JSONObject ?? [:]
            if !snapshot.isEmpty { app.graphics.observe(snapshot: snapshot) }
            if let id = (snapshot["programSharedTexture"] as? JSONObject)?["iosurfaceId"] as? NSNumber { program = id.uint32Value }
            return snapshot
        }
        func poll() {
            absorb(awaitMain(8) { (try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": []])) ?? [:] } ?? [:])
        }
        func settle(_ seconds: TimeInterval) { RunLoop.main.run(until: Date().addingTimeInterval(seconds)); poll() }
        func waitFor(_ what: String, _ limit: TimeInterval = 10, _ condition: () -> Bool) -> Bool {
            let end = Date().addingTimeInterval(limit)
            while Date() < end { poll(); if condition() { return true }; RunLoop.main.run(until: Date().addingTimeInterval(0.1)) }
            expect(false, "social live: timed out waiting for \(what)")
            return false
        }
        guard waitFor("graphics available", 15, { app.graphics.available }) else { return }
        _ = awaitMain(10) { await app.take() }
        guard waitFor("a Program surface", 10, { programPixels(program) != nil }) else { return }
        settle(0.6)
        guard let baseline = programPixels(program) else { return }
        log("Program \(baseline.width)x\(baseline.height), preview surface \(live.preview)")

        // 1. The core refuses a social instance under any other client, and Program does not move.
        let epoch = app.graphics.state?.coreEpoch ?? ""
        let wrong: JSONObject = ["type": "graphics-instance-create", "graphics": [
            "schemaVersion": 1, "clientId": "graphics-playout", "requestId": "social-live-wrong", "coreEpoch": epoch,
            "instanceId": "social-review-x", "scope": "global", "templateId": "youtube-chat-card", "templateVersion": 1,
            "fields": ["author": "A", "message": "M"]] as JSONObject]
        let reply = awaitMain(8) { (try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": [wrong]])) ?? [:] } ?? [:]
        let state = GraphicsState(node: (absorb(reply)["graphics"] as? JSONObject) ?? [:])
        let refusal = state.operations.last { $0.requestId == "social-live-wrong" } ?? state.lastResult
        log("wrong client: accepted=\(refusal?.accepted == true) reason=\(refusal?.reason ?? "nil")")
        expect(refusal?.accepted == false, "social live: the core refuses a social instance from another client")
        expect(refusal?.reason.contains("social") == true, "social live: with a social refusal code (\(refusal?.reason ?? "nil"))")
        settle(0.5)
        let afterWrong = changedFraction(baseline.bytes, programPixels(program)?.bytes ?? [])
        expect(afterWrong < 0.001, "social live: Program is unchanged by the refused command (\(afterWrong))")

        // 2/3. Approve and Show a card per provider through the REAL gated path.
        let playout = SocialPlayout(invoke: { action, body in
                                        let r = await app.graphics.gateway.invokeSocial(action, body)
                                        if !r.accepted || ProcessInfo.processInfo.environment["COREVIDEO_SOCIAL_LIVE_TRACE"] == "1" {
                                            log("\(action) \((body["action"] as? String) ?? "") -> accepted=\(r.accepted) \(r.reason) rev=\(r.currentRevision)")
                                        }
                                        return r
                                    },
                                    observe: { app.graphics.state },
                                    sleep: { ns in
                                        try? await Task.sleep(nanoseconds: ns)
                                        _ = absorb((try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": []])) ?? [:])
                                    })
        for provider in ["YouTube", "Twitch"] {
            let source = awaitMain { await playout.connect(provider: provider) } ?? 0
            let item = awaitMain { await playout.receive(source: source, id: "live-\(provider)", author: "Social Live Author",
                                                         message: "A reviewed message drawn by the real core") } ?? nil
            guard let item else { expect(false, "social live: \(provider) item"); continue }
            expect(playout.approve(item.key), "approve \(provider)")
            settle(0.3)
            let previewBefore = programPixels(live.preview)?.bytes
            let shownAt = Date()
            let shown = awaitMain(15) { await playout.show(item.key) }
            log("\(provider) Show: \(shown?.accepted == true) \(shown?.reason ?? "nil")")
            expectEqual(shown?.accepted, true, "social live: \(provider) Show accepted")
            _ = waitFor("\(provider) card on Program", 5) { playout.onProgram }
            settle(0.4)
            let onAir = programPixels(program)?.bytes ?? []
            // The plate (#141822) is close to a dark scene, so the judge is the
            // text: bright pixels that appear inside the card's text area.
            let cardDelta = changedFraction(baseline.bytes, onAir, width: baseline.width, height: baseline.height, rect: socialTextRect)
            let text = brightFraction(onAir, width: baseline.width, height: baseline.height, rect: socialTextRect)
                - brightFraction(baseline.bytes, width: baseline.width, height: baseline.height, rect: socialTextRect)
            log(String(format: "%@ text area %.1f%% changed, %.2f%% newly bright, %.2fs after Show",
                       provider, cardDelta * 100, text * 100, Date().timeIntervalSince(shownAt)))
            expect(Date().timeIntervalSince(shownAt) < 5, "social live: \(provider) on Program promptly")
            expect(cardDelta > 0.01, "social live: \(provider) card changes its lower-centre text area (\(cardDelta))")
            expect(text > 0.01, "social live: \(provider) card draws text (Segoe UI substitution) (\(text))")
            if let before = previewBefore, let after = programPixels(live.preview)?.bytes {
                let previewDelta = changedFraction(before, after, width: baseline.width, height: baseline.height, rect: socialCardRect)
                expect(previewDelta < 0.01, "social live: \(provider) card is not on Preview (\(previewDelta))")
            } else {
                log("no Preview surface exported; Preview leg not judged")
            }
            if provider == "YouTube" {
                // The core's 15 s on-air limit retires the card on its own.
                let gone = waitFor("the card leaving Program by itself", 20) {
                    changedFraction(baseline.bytes, programPixels(program)?.bytes ?? []) < 0.001
                }
                log(String(format: "card gone by itself after %.1fs", Date().timeIntervalSince(shownAt)))
                expect(gone && Date().timeIntervalSince(shownAt) <= 16.5, "social live: gone within the 15 s limit")
            }
            _ = awaitMain { await playout.disconnect(); return true }
            settle(0.6)
        }
    }
}
