// CoreVideo Tiles S1 live proof (docs/superpowers/plans/2026-11-22-mac-tiles.md Task 9).
//
// Opt-in: COREVIDEO_SHELL_LIVE_CORE=<corevideo-native> and
// COREVIDEO_SHELL_LIVE_ENGINE=<corevideo-zoom-engine-fake>; otherwise it skips
// loudly. It launches a REAL Metal core with the fake Zoom engine (4 synthetic
// participants, primary colours) through MediaCoreBridge (never the owner's app),
// drives the real AppModel (join, the Tiles starter, the Mac Take), and judges
// the core's own take record, its `tiles` node and Program/Preview PIXELS.

import Foundation

extension ShellTests {
    static var tilesLiveCases: [(String, () -> Void)] {
        [("tiles-live/wall-cuts-and-owns-an-empty-scene", testTilesLiveWallCutsAndOwnsAnEmptyScene)]
    }

    /// Pixel (BGRA) at a normalized point of a surface's bytes.
    static func pixelAt(_ image: (width: Int, height: Int, bytes: [UInt8]), _ nx: Double, _ ny: Double) -> (b: Int, g: Int, r: Int) {
        let x = min(image.width - 1, max(0, Int(nx * Double(image.width))))
        let y = min(image.height - 1, max(0, Int(ny * Double(image.height))))
        let i = (y * image.width + x) * 4
        return (Int(image.bytes[i]), Int(image.bytes[i + 1]), Int(image.bytes[i + 2]))
    }

    static func isBlack(_ p: (b: Int, g: Int, r: Int), within tolerance: Int) -> Bool {
        p.b <= tolerance && p.g <= tolerance && p.r <= tolerance
    }

    @MainActor
    static func testTilesLiveWallCutsAndOwnsAnEmptyScene() {
        let env = ProcessInfo.processInfo.environment
        guard let corePath = env["COREVIDEO_SHELL_LIVE_CORE"], !corePath.isEmpty,
              let enginePath = env["COREVIDEO_SHELL_LIVE_ENGINE"], !enginePath.isEmpty else {
            FileHandle.standardError.write(Data(
                "  [skip] tiles live check: set COREVIDEO_SHELL_LIVE_CORE=<corevideo-native> and COREVIDEO_SHELL_LIVE_ENGINE=<corevideo-zoom-engine-fake>\n".utf8))
            return
        }
        func log(_ line: String) { FileHandle.standardError.write(Data("  [tiles-live] \(line)\n".utf8)) }
        let w = "tiles live"
        let paths = ShellPaths.Paths(corePath: corePath, engineBinaryPath: enginePath, pluginHostPath: nil, graphicsFontsDirectory: nil)
        var extras = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil)
        extras["COREVIDEO_FAKE_ENGINE_PARTICIPANTS"] = "4"
        extras["COREVIDEO_FAKE_PRIMARY_COLORS"] = "1"
        extras["COREVIDEO_FAKE_ENGINE_FPS"] = "30"
        extras["COREVIDEO_FAKE_NO_CHURN"] = "1"
        let bridge = MediaCoreBridge(corePath: corePath, environmentExtras: extras)
        let flag = LiveFlag()
        bridge.onStatus = { status in if case .connected = status { flag.connected = true } }
        bridge.start()
        defer { bridge.stop() }
        spin(until: { flag.connected }, limit: 30)
        guard flag.connected else { expect(false, "\(w): live core did not connect"); return }

        let app = AppModel()
        app.quitRequestForTesting = { body, timeout in try await bridge.request(body, timeout: timeout) }
        app.status = .connected(renderer: "metal")
        app.onConnected()

        var snapshot: JSONObject = [:]
        func poll() {
            let reply = awaitMain(8) { (try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": []])) ?? [:] } ?? [:]
            if let s = reply["snapshot"] as? JSONObject, !s.isEmpty { snapshot = s; app.applySnapshotForTesting(s) }
            if let z = awaitMain(8, { (try? await bridge.request(["type": "zoom-snapshot"])) ?? [:] }),
               let zs = z["snapshot"] as? JSONObject { app.applyZoomForTesting(zs) }
        }
        func waitFor(_ what: String, _ limit: TimeInterval, _ condition: () -> Bool) -> Bool {
            let end = Date().addingTimeInterval(limit)
            while Date() < end { poll(); if condition() { return true }; RunLoop.main.run(until: Date().addingTimeInterval(0.1)) }
            expect(false, "\(w): timed out waiting for \(what)")
            return false
        }
        func surface(_ key: String) -> UInt32 {
            ((snapshot[key] as? JSONObject)?["iosurfaceId"] as? NSNumber)?.uint32Value ?? 0
        }

        app.joinMeetingId = "1234567890"
        _ = awaitMain(15) { await app.performJoin() }
        app.setCapture(enabled: true)
        guard waitFor("4 camera-on participants", 30, { app.roster.filter(\.hasVideo).count >= 4 }) else { return }
        log("roster: \(app.roster.map { "\($0.id)\($0.hasVideo ? "" : "(off)")" })")

        // 1. The starter on Preview: the wall composites in Preview.
        app.newTilesScene()
        guard let wall = app.previewSceneId.isEmpty ? nil : app.previewSceneId else { expect(false, "\(w): starter on Preview"); return }
        guard waitFor("the Preview composite", 15, { programPixels(surface("previewSharedTexture")) != nil }) else { return }
        RunLoop.main.run(until: Date().addingTimeInterval(2.0)); poll()
        let previewImage = programPixels(surface("previewSharedTexture"))

        // 2. Take: the wall is cut to, never rebuilt.
        let before = ((snapshot["takeRecords"] as? JSONObject)?["count"] as? NSNumber)?.intValue ?? 0
        let take = awaitMain(10) { await app.take() }
        log("Take: \(String(describing: take))")
        var record: JSONObject = [:]
        _ = waitFor("the take record", 3) {
            let t = snapshot["takeRecords"] as? JSONObject
            guard ((t?["count"] as? NSNumber)?.intValue ?? 0) > before else { return false }
            record = (t?["records"] as? [JSONObject])?.last ?? [:]
            return true
        }
        log("take record: verdict=\(record["verdict"] ?? "-") wall=\(record["wall"] ?? "-") churn=\(record["subscriptionChurnDelta"] ?? "-") restarted=\(record["restartedSources"] ?? "-") missing=\(record["missingSources"] ?? "-")")
        expectEqual(record["verdict"] as? String, "cut", "\(w): verdict cut")
        expectEqual(record["wall"] as? String, "continuous", "\(w): the wall is continuous across the Take")
        expectEqual((record["restartedSources"] as? [String])?.count, 0, "\(w): no source restarted")
        expectEqual((record["missingSources"] as? [String])?.count, 0, "\(w): no source missing")
        expectEqual((record["subscriptionChurnDelta"] as? NSNumber)?.intValue, 0, "\(w): no subscription churn")
        guard waitFor("4 members on the Program wall", 5, {
            ((snapshot["tiles"] as? JSONObject)?["members"] as? [JSONObject])?.count == 4 }) else { return }
        let tilesNode = snapshot["tiles"] as? JSONObject ?? [:]
        expectEqual(tilesNode["layerId"] as? String, "tiles:" + wall, "\(w): the Program wall is the cued wall")
        let members = tilesNode["members"] as? [JSONObject] ?? []
        let centres: [(Double, Double)] = members.compactMap { m in
            guard let r = m["rect"] as? JSONObject, let x = r["x"] as? Double, let y = r["y"] as? Double,
                  let width = r["width"] as? Double, let height = r["height"] as? Double else { return nil }
            return (x + width / 2, y + height / 2)
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.5)); poll()
        if let program = programPixels(surface("programSharedTexture")) {
            let lit = centres.filter { !isBlack(pixelAt(program, $0.0, $0.1), within: 8) }.count
            expectEqual(lit, 4, "\(w): every Program tile centre shows a picture, not the #000000 background")
        } else { expect(false, "\(w): a Program surface") }
        // The Preview frame sampled before the Take showed the same wall (same layer id, same layout).
        if let previewImage {
            let lit = centres.filter { !isBlack(pixelAt(previewImage, $0.0, $0.1), within: 8) }.count
            expectEqual(lit, 4, "\(w): the Preview wall showed 4 non-background regions before the Take")
        } else { expect(false, "\(w): a Preview surface before the Take") }

        // 3. All frames stop. The fake engine keeps producing across
        //    zoom-stop-capture (only a leave destroys its targets), so the
        //    proof leaves the meeting. Capture off is sent too, as the
        //    operator would. After the hold window plus 1 s the wall is
        //    present and empty, and Program shows the wall background where
        //    the tiles were, never an improvised grid.
        app.setCapture(enabled: false)
        _ = awaitMain(20) { await app.leaveZoomForControl() }
        let holdMs = ((snapshot["tiles"] as? JSONObject)?["holdWindowMs"] as? NSNumber)?.doubleValue ?? 4000
        let quietUntil = Date().addingTimeInterval(holdMs / 1000 + 1)
        while Date() < quietUntil { poll(); RunLoop.main.run(until: Date().addingTimeInterval(0.2)) }
        _ = waitFor("an empty wall", 5) { ((snapshot["tiles"] as? JSONObject)?["members"] as? [JSONObject])?.isEmpty == true }
        expect(snapshot["tiles"] != nil, "\(w): the tiles node stays present with the wall empty")
        RunLoop.main.run(until: Date().addingTimeInterval(0.5)); poll()
        if let program = programPixels(surface("programSharedTexture")) {
            let samples = centres.map { pixelAt(program, $0.0, $0.1) } + [pixelAt(program, 0.5, 0.5)]
            log("empty-wall pixels at the old tile centres + canvas centre: \(samples)")
            expect(samples.allSatisfy { isBlack($0, within: 4) },
                   "\(w): an empty wall shows its #000000 background where the tiles were (empty-plan guard)")
        } else { expect(false, "\(w): a Program surface after the wall emptied") }

        // 4. No routes+wall collision anywhere in the snapshot.
        let text = (try? JSONSerialization.data(withJSONObject: snapshot)).flatMap { String(data: $0, encoding: .utf8) } ?? ""
        expect(!text.contains("tiles wall on the"), "\(w): no routes + wall collision warning")
    }
}
