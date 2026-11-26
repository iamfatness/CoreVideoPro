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
        [("tiles-live/wall-cuts-and-owns-an-empty-scene", testTilesLiveWallCutsAndOwnsAnEmptyScene),
         ("tiles-live/take-and-speaker-change-churn-nothing", testTilesLiveTakeAndSpeakerChangeChurnNothing),
         ("tiles-live/follow-speaker-evidence", testTilesLiveFollowSpeakerEvidence),
         ("tiles-live/presets-change-program-pixels", testTilesLivePresetsChangeProgramPixels),
         ("tiles-live/pinned-and-cropped-member-pixels", testTilesLivePinnedAndCroppedMemberPixels)]
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

    // ── Task 12: churn across a wall Take, and follow-speaker evidence (D6) ──

    /// A live core + fake engine + AppModel wired like the app (spine included).
    @MainActor
    final class TilesLiveRig {
        let bridge: MediaCoreBridge
        let app = AppModel()
        var snapshot: JSONObject = [:]
        var zoom: JSONObject = [:]
        init?(participants: Int, extra: [String: String]) {
            let env = ProcessInfo.processInfo.environment
            guard let core = env["COREVIDEO_SHELL_LIVE_CORE"], !core.isEmpty,
                  let engine = env["COREVIDEO_SHELL_LIVE_ENGINE"], !engine.isEmpty else { return nil }
            let paths = ShellPaths.Paths(corePath: core, engineBinaryPath: engine, pluginHostPath: nil, graphicsFontsDirectory: nil)
            var extras = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil)
            extras["COREVIDEO_FAKE_ENGINE_PARTICIPANTS"] = String(participants)
            extras["COREVIDEO_FAKE_ENGINE_FPS"] = "30"
            for (k, v) in extra { extras[k] = v }
            bridge = MediaCoreBridge(corePath: core, environmentExtras: extras)
            let flag = LiveFlag()
            bridge.onStatus = { status in if case .connected = status { flag.connected = true } }
            bridge.start()
            ShellTests.spin(until: { flag.connected }, limit: 30)
            guard flag.connected else { bridge.stop(); return nil }
            let bridge = self.bridge
            app.quitRequestForTesting = { body, timeout in try await bridge.request(body, timeout: timeout) }
            app.spineRequestForTesting = { body in try await bridge.request(body) }
            app.status = .connected(renderer: "metal")
            app.onConnected()
        }
        func stop() { bridge.stop() }
        func poll() {
            let bridge = self.bridge, app = self.app
            let reply = ShellTests.awaitMain(8) { (try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": []])) ?? [:] } ?? [:]
            if let s = reply["snapshot"] as? JSONObject, !s.isEmpty { snapshot = s; app.applySnapshotForTesting(s) }
            if let z = ShellTests.awaitMain(8, { (try? await bridge.request(["type": "zoom-snapshot"])) ?? [:] }),
               let zs = z["snapshot"] as? JSONObject { zoom = zs; app.applyZoomForTesting(zs) }
        }
        @discardableResult
        func wait(_ seconds: TimeInterval, until condition: () -> Bool = { false }) -> Bool {
            let end = Date().addingTimeInterval(seconds)
            while Date() < end { poll(); if condition() { return true }; RunLoop.main.run(until: Date().addingTimeInterval(0.1)) }
            return false
        }
        /// Per-source churn counts, keyed by source uuid.
        var churnBySource: [String: Int] {
            let sources = (snapshot["zoomSubscriptionChurn"] as? JSONObject)?["sources"] as? [JSONObject] ?? []
            var out: [String: Int] = [:]
            for s in sources {
                let key = s["sourceUuid"] as? String ?? s["sourceId"] as? String ?? "?"
                out[key] = (s["churn"] as? NSNumber)?.intValue ?? 0
            }
            return out
        }
    }

    @MainActor
    static func testTilesLiveTakeAndSpeakerChangeChurnNothing() {
        let w = "tiles live churn"
        // Explicit subscriptions only: a wall member gets frames ONLY because the
        // spine subscribed it (S1 slice review: the S1 live check could not see this).
        guard let rig = TilesLiveRig(participants: 6, extra: ["COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE": "0"]) else {
            FileHandle.standardError.write(Data("  [skip] tiles live churn: set COREVIDEO_SHELL_LIVE_CORE and COREVIDEO_SHELL_LIVE_ENGINE\n".utf8))
            return
        }
        defer { rig.stop() }
        func log(_ s: String) { FileHandle.standardError.write(Data("  [tiles-live] \(s)\n".utf8)) }
        let app = rig.app
        app.joinMeetingId = "1234567890"
        _ = awaitMain(15) { await app.performJoin() }
        guard rig.wait(30, until: { app.roster.filter(\.hasVideo).count >= 5 }) else { expect(false, "\(w): roster"); return }
        // No guest is on the multiview: the wall alone makes them sources.
        for i in app.slots.indices { app.slots[i].inShow = false }
        app.setCapture(enabled: true)
        app.newTilesScene()
        let wallId = app.previewSceneId
        rig.wait(3)
        let wallMembers = { ((rig.snapshot["tiles"] as? JSONObject)?["members"] as? [JSONObject])?.count ?? 0 }
        let beforeChurn = rig.churnBySource
        let beforeCount = ((rig.snapshot["takeRecords"] as? JSONObject)?["count"] as? NSNumber)?.intValue ?? 0
        let take = awaitMain(10) { await app.take() }
        log("Take: \(String(describing: take)); wall \(wallId)")
        rig.wait(3)
        let record = ((rig.snapshot["takeRecords"] as? JSONObject)?["records"] as? [JSONObject])?.last ?? [:]
        let afterCount = ((rig.snapshot["takeRecords"] as? JSONObject)?["count"] as? NSNumber)?.intValue ?? 0
        expect(afterCount > beforeCount, "\(w): a take record")
        log("take record: verdict=\(record["verdict"] ?? "-") wall=\(record["wall"] ?? "-") churnDelta=\(record["subscriptionChurnDelta"] ?? "-")")
        expectEqual(record["verdict"] as? String, "cut", "\(w): the wall Take is a cut")
        expectEqual((record["subscriptionChurnDelta"] as? NSNumber)?.intValue, 0, "\(w): no subscription churn on the Take")
        expect(wallMembers() >= 5, "\(w): the Program wall draws its members from spine subscriptions alone (\(wallMembers()))")
        // The fake engine rotates the speaker every 2 s, and every 10 s guest 104
        // leaves or rejoins (its own roster churn, not a speaker change).
        rig.wait(15)
        let afterChurn = rig.churnBySource
        let moved = afterChurn.filter { key, value in !key.contains("104") && value != (beforeChurn[key] ?? value) }
        log("churn before \(beforeChurn.sorted { $0.key < $1.key }) after \(afterChurn.sorted { $0.key < $1.key })")
        expect(moved.isEmpty, "\(w): speaker changes churn no subscription (moved: \(moved))")
    }

    @MainActor
    static func testTilesLiveFollowSpeakerEvidence() {
        let w = "tiles live follow-speaker (D6)"
        guard let rig = TilesLiveRig(participants: 4, extra: [:]) else {
            FileHandle.standardError.write(Data("  [skip] tiles live follow-speaker: set COREVIDEO_SHELL_LIVE_CORE and COREVIDEO_SHELL_LIVE_ENGINE\n".utf8))
            return
        }
        defer { rig.stop() }
        func log(_ s: String) { FileHandle.standardError.write(Data("  [tiles-live] \(s)\n".utf8)) }
        let app = rig.app
        app.joinMeetingId = "1234567890"
        _ = awaitMain(15) { await app.performJoin() }
        guard rig.wait(30, until: { app.roster.filter(\.hasVideo).count >= 4 }) else { expect(false, "\(w): roster"); return }
        // Slot 1 in show, every other slot out; a two-up scene whose second cell
        // follows the speaker.
        for i in app.slots.indices { app.slots[i].inShow = (app.slots[i].id == 1 && app.slots[i].kind == "zoom") }
        app.scenes.append(SceneDef(id: "follow", name: "Follow", layout: "two-up"))
        app.programSceneId = "follow"
        app.setCapture(enabled: true)
        app.syncScenes()
        func record(_ seconds: TimeInterval) -> [String] {
            var picks: [String] = []
            let end = Date().addingTimeInterval(seconds)
            while Date() < end {
                rig.poll()
                if let id = rig.zoom["activeSpeakerId"] as? String, !id.isEmpty, picks.last != id { picks.append(id) }
                RunLoop.main.run(until: Date().addingTimeInterval(0.25))
            }
            return picks
        }
        // BEFORE (the base commit's wire): the spine without sourceParticipantIds.
        let bridge = rig.bridge
        app.spineRequestForTesting = { body in
            var b = body
            if var payload = b["spinePayload"] as? JSONObject { payload.removeValue(forKey: "sourceParticipantIds"); b["spinePayload"] = payload }
            return try await bridge.request(b)
        }
        app.setCapture(enabled: true)
        let before = record(30)
        // AFTER (this branch): sourceParticipantIds rides the spine.
        app.spineRequestForTesting = { body in try await bridge.request(body) }
        app.setCapture(enabled: true)
        rig.wait(2)
        let sources = Set((app.spinePushRequests().last?["spinePayload"] as? JSONObject)?["sourceParticipantIds"] as? [String] ?? [])
        let after = record(30)
        log("follow-speaker BEFORE (no sourceParticipantIds): \(before)")
        log("follow-speaker AFTER  (sourceParticipantIds \(sources.sorted())): \(after)")
        expect(!sources.isEmpty, "\(w): the spine names its sources")
        expect(!after.isEmpty, "\(w): the director still directs someone (no speaker-following deadlock)")
        expect(!Set(before).isSubset(of: sources),
               "\(w): before the change the director followed a non-source (the evidence shows a difference)")
        expect(Set(after).isSubset(of: sources), "\(w): the director picks only among sources (\(after) ⊆ \(sources.sorted()))")
    }

    // ── Task 18: preset pixels on Program ──

    @MainActor
    static func testTilesLivePresetsChangeProgramPixels() {
        let w = "tiles live presets"
        guard let rig = TilesLiveRig(participants: 4, extra: ["COREVIDEO_FAKE_PRIMARY_COLORS": "1", "COREVIDEO_FAKE_NO_CHURN": "1"]) else {
            FileHandle.standardError.write(Data("  [skip] tiles live presets: set COREVIDEO_SHELL_LIVE_CORE and COREVIDEO_SHELL_LIVE_ENGINE\n".utf8))
            return
        }
        defer { rig.stop() }
        func log(_ s: String) { FileHandle.standardError.write(Data("  [tiles-live] \(s)\n".utf8)) }
        let app = rig.app
        app.joinMeetingId = "1234567890"
        _ = awaitMain(15) { await app.performJoin() }
        guard rig.wait(30, until: { app.roster.filter(\.hasVideo).count >= 4 }) else { expect(false, "\(w): roster"); return }
        app.setCapture(enabled: true)
        // Two walls: A on Program, B on Preview. Each preset is applied to the
        // Preview wall and taken; the next one lands on the other wall.
        app.scenes.append(SceneDef(id: "wall-a", name: "Wall A", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        app.scenes.append(SceneDef(id: "wall-b", name: "Wall B", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        app.programSceneId = "wall-a"; app.previewSceneId = "wall-b"
        app.syncScenes()
        rig.wait(3)
        func surface() -> (width: Int, height: Int, bytes: [UInt8])? {
            programPixels(((rig.snapshot["programSharedTexture"] as? JSONObject)?["iosurfaceId"] as? NSNumber)?.uint32Value ?? 0)
        }
        func px(_ img: (width: Int, height: Int, bytes: [UInt8]), _ x: Int, _ y: Int) -> (b: Int, g: Int, r: Int) {
            let cx = min(max(x, 0), img.width - 1), cy = min(max(y, 0), img.height - 1)
            let i = (cy * img.width + cx) * 4
            return (Int(img.bytes[i]), Int(img.bytes[i + 1]), Int(img.bytes[i + 2]))
        }
        for (preset, border) in [(TilesStylePreset.rounded, (r: 0x65, g: 0x7D, b: 0x87)), (.broadcast, (r: 0x8A, g: 0xE0, b: 0xCA))] {
            do { try app.applyTilesPreset(preset) } catch { expect(false, "\(w): \(preset) applies on Preview"); continue }
            rig.wait(1)
            _ = awaitMain(10) { await app.take() }
            rig.wait(1.5)
            guard let img = surface(),
                  let rect = (((rig.snapshot["tiles"] as? JSONObject)?["members"] as? [JSONObject])?.first?["rect"] as? JSONObject),
                  let x = rect["x"] as? Double, let y = rect["y"] as? Double, let width = rect["width"] as? Double,
                  let height = rect["height"] as? Double else { expect(false, "\(w): \(preset) Program wall and pixels"); continue }
            let left = Int((x * Double(img.width)).rounded()), top = Int((y * Double(img.height)).rounded())
            let midY = Int(((y + height / 2) * Double(img.height)).rounded())
            let corner = px(img, left + 2, top + 2), edge = px(img, left + 1, midY), outside = px(img, left - 6, midY)
            log("\(preset): tile0 at (\(left),\(top)) w=\(Int(width * Double(img.width))) corner=\(corner) edge=\(edge) outside=\(outside)")
            log("\(preset): exact left=\(x * Double(img.width)) scan=\((-3...6).map { px(img, left + $0, midY) })")
            // The rounded mask: the tile's colour never reaches its corner. Without glow
            // the corner is the #000000 wall background; Broadcast's 12 px white glow
            // fills the masked-off corner (glow is drawn outside the tile shape), so
            // there it is neutral grey, never the tile's primary colour.
            if preset == .broadcast {
                expect(abs(corner.r - corner.g) <= 8 && abs(corner.g - corner.b) <= 8 && corner.g > 8,
                       "\(w): broadcast the corner is masked off and filled by the white glow (\(corner))")
            } else {
                expect(corner.r <= 8 && corner.g <= 8 && corner.b <= 8, "\(w): \(preset) the rounded mask shows the #000000 background at the corner")
            }
            // The border: Broadcast's 5 px border has pure pixels (within 6). Rounded's
            // 2 px border sits at a fractional x with a ~1 px anti-aliased ramp on each
            // side, so no pixel is pure: the best of the first two is within 14.
            let tolerance = preset == .broadcast ? 6 : 14
            let candidates = preset == .broadcast ? [edge] : [px(img, left, midY), edge]
            let best = candidates.map { max(abs($0.r - border.r), abs($0.g - border.g), abs($0.b - border.b)) }.min() ?? 255
            expect(best <= tolerance, "\(w): \(preset) the border colour at the left edge (off by \(best), tolerance \(tolerance))")
            if preset == .broadcast {
                expect(outside.g > 8, "\(w): broadcast glow lifts the background beside the tile (\(outside))")
            }
        }
    }

    // ── Task 20: a pinned, cropped member on Program ──

    @MainActor
    static func testTilesLivePinnedAndCroppedMemberPixels() {
        let w = "tiles live pinned member"
        guard let rig = TilesLiveRig(participants: 4, extra: ["COREVIDEO_FAKE_PRIMARY_COLORS": "1", "COREVIDEO_FAKE_NO_CHURN": "1"]) else {
            FileHandle.standardError.write(Data("  [skip] tiles live pinned member: set COREVIDEO_SHELL_LIVE_CORE and COREVIDEO_SHELL_LIVE_ENGINE\n".utf8))
            return
        }
        defer { rig.stop() }
        func log(_ s: String) { FileHandle.standardError.write(Data("  [tiles-live] \(s)\n".utf8)) }
        let app = rig.app
        app.joinMeetingId = "1234567890"
        _ = awaitMain(15) { await app.performJoin() }
        guard rig.wait(30, until: { app.roster.filter(\.hasVideo).count >= 4 }) else { expect(false, "\(w): roster"); return }
        app.setCapture(enabled: true)
        app.scenes.append(SceneDef(id: "wall-a", name: "Wall A", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        app.scenes.append(SceneDef(id: "wall-b", name: "Wall B", layout: AppModel.tilesLayout, tiles: TilesSettings()))
        app.programSceneId = "wall-a"; app.previewSceneId = "wall-b"
        app.syncScenes()
        rig.wait(2)
        let pinned = "zoom:101"
        app.tilesSelectedMemberId = pinned
        app.setTilesPinned(true)
        app.setTilesPin(field: .width, pixels: 960)
        app.setTilesPin(field: .height, pixels: 1080)
        app.setTilesCrop(left: 25, right: 25)
        expectEqual(app.tilesSelectedOverride?.rect, TilesRect(x: 0, y: 0, width: 0.5, height: 1), "\(w): pinned at (0,0,960,1080)")
        rig.wait(1)
        _ = awaitMain(10) { await app.take() }
        rig.wait(2)
        let members = (rig.snapshot["tiles"] as? JSONObject)?["members"] as? [JSONObject] ?? []
        let others = members.filter { $0["sourceId"] as? String != pinned }
        let xs = others.compactMap { ($0["rect"] as? JSONObject)?["x"] as? Double }
        log("members: \(members.map { "\($0["sourceId"] ?? "?") \(($0["rect"] as? JSONObject).map { "\($0["x"] ?? 0),\($0["width"] ?? 0)" } ?? "")" })")
        expectEqual(others.count, 3, "\(w): three automatic tiles")
        expect(xs.count == 3 && xs.allSatisfy { $0 >= 0.5 - 1e-6 }, "\(w): the automatic tiles reflow into the right half (\(xs))")
        guard let img = programPixels(((rig.snapshot["programSharedTexture"] as? JSONObject)?["iosurfaceId"] as? NSNumber)?.uint32Value ?? 0) else {
            expect(false, "\(w): a Program surface"); return
        }
        var samples: [(Int, Int, Int)] = []
        for fx in [0.005, 0.25, 0.495] { for fy in [0.01, 0.5, 0.99] {
            let p = pixelAt(img, fx, fy); samples.append((p.r, p.g, p.b))
        } }
        log("left-half samples (r,g,b): \(samples)")
        let first = samples[0]
        let uniform = samples.allSatisfy { abs($0.0 - first.0) <= 24 && abs($0.1 - first.1) <= 24 && abs($0.2 - first.2) <= 24 }
        // Member 101's primary colour is BLUE (fake engine channel (pid-101)%3 == 0).
        let isBlue = first.2 > 200 && first.0 < 60 && first.1 < 60
        expect(uniform && isBlue, "\(w): the left half is member 101's blue edge to edge (\(first))")
        let pinnedRect = members.first { $0["sourceId"] as? String == pinned }?["rect"] as? JSONObject
        expect(abs((pinnedRect?["x"] as? Double ?? -1)) < 1e-6 && abs((pinnedRect?["width"] as? Double ?? 0) - 0.5) < 1e-6
               && abs((pinnedRect?["height"] as? Double ?? 0) - 1) < 1e-6, "\(w): the core places 101 at (0,0,0.5,1)")
        // The 25/25 crop cannot be seen in a solid frame; edge-to-edge fill of the 8:9
        // rect is what the crop makes possible for a 16:9 source (no letterbox bars).
    }
}
