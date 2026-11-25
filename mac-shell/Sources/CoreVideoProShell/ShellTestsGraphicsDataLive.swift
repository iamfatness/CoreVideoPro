// Graphics G4-G7 Tasks 10 and 12 — opt-in live proofs (plan D11) against a REAL Metal core
// (COREVIDEO_SHELL_LIVE_CORE=<corevideo-native>; skipped loudly otherwise). Program pixels come
// from the core's exported Program IOSurface, as in the G1-G3 live check.
//   Task 10: a Mac-created table design (scores.csv) is shown, scores-v2.csv is staged and
//            published: Program changes inside the table's bounds only, the instance id is
//            unchanged and no In motion replays (visibility never leaves on-air).
//   Task 12: a loop crawl (speed 1200, gap 0, three items) is shown; a staged revision is
//            published while it plays: crawlPendingRevision > 0, then Program adopts it at a loop
//            boundary (programFields crawl-revision) with the same instance and no exit; Pause
//            freezes Program pixels, Resume moves them, Hide returns Program to baseline.

import Foundation

extension ShellTests {
    static var graphicsDataLiveCases: [(String, () -> Void)] {
        [("graphics-live/data-publish-and-crawl-against-a-real-core", testGraphicsDataLiveCore)]
    }

    /// The bounding box (in surface pixels) of every pixel that changed by more than 24 in any channel.
    static func changedBox(_ a: [UInt8], _ b: [UInt8], width: Int) -> (minX: Int, minY: Int, maxX: Int, maxY: Int)? {
        guard a.count == b.count, width > 0 else { return nil }
        var box: (Int, Int, Int, Int)?
        var i = 0
        while i < a.count {
            if abs(Int(a[i]) - Int(b[i])) > 24 || abs(Int(a[i + 1]) - Int(b[i + 1])) > 24 || abs(Int(a[i + 2]) - Int(b[i + 2])) > 24 {
                let p = i / 4, x = p % width, y = p / width
                box = box.map { (min($0.0, x), min($0.1, y), max($0.2, x), max($0.3, y)) } ?? (x, y, x, y)
            }
            i += 4
        }
        return box.map { (minX: $0.0, minY: $0.1, maxX: $0.2, maxY: $0.3) }
    }

    @MainActor
    static func testGraphicsDataLiveCore() {
        guard let corePath = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_LIVE_CORE"], !corePath.isEmpty else {
            FileHandle.standardError.write(Data("  [skip] graphics data live check: set COREVIDEO_SHELL_LIVE_CORE=<corevideo-native>\n".utf8))
            return
        }
        func log(_ line: String) { FileHandle.standardError.write(Data("  [live-data] \(line)\n".utf8)) }
        let fonts = ProcessInfo.processInfo.environment["COREVIDEO_GRAPHICS_FONTS_DIR"]
            ?? repoRoot.appendingPathComponent("native/resources/graphics-fonts").path
        let paths = ShellPaths.Paths(corePath: corePath, engineBinaryPath: nil, pluginHostPath: nil, graphicsFontsDirectory: fonts)
        let bridge = MediaCoreBridge(corePath: corePath, environmentExtras: CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil))
        let flag = LiveFlag()
        bridge.onStatus = { status in if case .connected = status { flag.connected = true } }
        bridge.start()
        defer { bridge.stop() }
        spin(until: { flag.connected }, limit: 30)
        guard flag.connected else { expect(false, "live core did not connect"); return }

        let app = AppModel()
        app.quitRequestForTesting = { body, timeout in try await bridge.request(body, timeout: timeout) }
        app.status = .connected(renderer: "metal")
        app.onConnected()
        spin(until: { app.graphics.documentLoaded }, limit: 3)
        app.graphics.saveDelayNs = 0
        var surfaceId: UInt32 = 0
        @discardableResult
        func absorb(_ reply: JSONObject) -> JSONObject {
            let snapshot = reply["snapshot"] as? JSONObject ?? [:]
            if !snapshot.isEmpty { app.graphics.observe(snapshot: snapshot) }
            if let id = (snapshot["programSharedTexture"] as? JSONObject)?["iosurfaceId"] as? NSNumber { surfaceId = id.uint32Value }
            return snapshot
        }
        @discardableResult
        func poll() -> JSONObject {
            absorb(awaitMain(8) { (try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": []])) ?? [:] } ?? [:])
        }
        func waitFor(_ what: String, _ limit: TimeInterval = 10, _ condition: () -> Bool) -> Bool {
            let end = Date().addingTimeInterval(limit)
            while Date() < end { poll(); if condition() { return true }; RunLoop.main.run(until: Date().addingTimeInterval(0.1)) }
            expect(false, "live: timed out waiting for \(what)")
            return false
        }
        app.graphics.playout.sleep = { ns in
            try? await Task.sleep(nanoseconds: ns)
            _ = absorb((try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": []])) ?? [:])
        }
        func settle(_ seconds: Double = 0.8) { RunLoop.main.run(until: Date().addingTimeInterval(seconds)); poll() }
        func data(_ file: String, key: String?) -> GraphicsLocalData? {
            try? GraphicsLocalData.read(url: repoRoot.appendingPathComponent("contracts/graphics-shell/data/" + file), key: key)
        }

        guard waitFor("graphics available", 15, { app.graphics.available }) else { return }
        _ = awaitMain(10) { await app.take() }
        guard waitFor("a Program surface", 10, { programPixels(surfaceId) != nil }) else { return }
        settle(0.5)
        guard let baseline = programPixels(surfaceId) else { return }
        log("Program surface \(baseline.width)x\(baseline.height)")

        // ── Task 10: table publish ─────────────────────────────────────────
        guard let scores = data("scores.csv", key: "team") else { expect(false, "scores.csv"); return }
        let tableBinding = GraphicsDataBinding(sourceId: GraphicsDataTableForm.newSourceId(), keyColumn: "team", columns: ["team", "points"],
                                               sortColumn: "points", limit: 5, revision: 1)
        let created = awaitMain(20) { await app.graphics.createDataDesign(name: "Live scores", data: scores, binding: tableBinding) }
        log("create table: \(created?.message ?? "nil")")
        guard created?.ok == true, let tableRowId = created?.rowId, let tableRow = app.graphics.document.row(tableRowId) else {
            expect(false, "live: the table design is created"); return
        }
        let tableEntry = GraphicsPlayout.entry(for: tableRow)
        let shown = awaitMain(15) { await app.graphics.playout.show(tableEntry) }
        expectEqual(shown?.accepted, true, "live: the table shows (\(shown?.reason ?? ""))")
        _ = waitFor("the table drawn on Program", 10) {
            let i = app.graphics.state?.instance(tableEntry.instanceId)
            return (i?.firstDrawSubmittedFrame ?? 0) > 0 && i?.isOnProgram == true
        }
        settle()
        let beforePublish = programPixels(surfaceId)?.bytes ?? []
        let lineage = app.graphics.state?.instance(tableEntry.instanceId)
        guard let v2 = data("scores-v2.csv", key: "team") else { expect(false, "scores-v2.csv"); return }
        let staged = awaitMain(10) { await app.graphics.stageData(rowId: tableRowId, binding: tableBinding, data: v2).value }
        log("stage: \(staged?.reason ?? "nil")")
        var visibilities: [String] = []
        let published = awaitMain(15) { await app.graphics.publishData(rowId: tableRowId).value }
        log("publish: \(published?.reason ?? "nil")")
        expectEqual(published?.reason, "Data revision 2 is on Program.", "live: Program adopts the published revision")
        for _ in 0..<6 { settle(0.15); visibilities.append(app.graphics.state?.instance(tableEntry.instanceId)?.visibility ?? "gone") }
        let afterPublish = programPixels(surfaceId)?.bytes ?? []
        let delta = changedFraction(beforePublish, afterPublish)
        let box = changedBox(beforePublish, afterPublish, width: baseline.width)
        log(String(format: "publish changed %.3f%% of Program; changed box %@", delta * 100, String(describing: box)))
        expect(delta > 0, "live: the published score changes Program pixels")
        // The table is 1440x(5+1)*56 = 1440x336 design pixels, centred on a 1920x1080 canvas.
        let sx = Double(baseline.width) / 1920, sy = Double(baseline.height) / 1080
        let table = (minX: Int(240 * sx) - 2, minY: Int(372 * sy) - 2, maxX: Int(1680 * sx) + 2, maxY: Int(708 * sy) + 2)
        if let box {
            expect(box.minX >= table.minX && box.maxX <= table.maxX && box.minY >= table.minY && box.maxY <= table.maxY,
                   "live: the change stays inside the table bounds \(table): \(box)")
        }
        let after = app.graphics.state?.instance(tableEntry.instanceId)
        expectEqual(after?.instanceId, lineage?.instanceId, "live: the same instance")
        expect(visibilities.allSatisfy { $0 != "hidden" && $0 != "building-in" && $0 != "gone" }, "live: no In motion replays (\(visibilities))")
        _ = awaitMain(10) { await app.graphics.playout.hide(tableEntry.instanceId) }
        _ = waitFor("the table off Program", 10) { app.graphics.state?.instance(tableEntry.instanceId)?.isOnProgram == false }

        // ── Task 12: crawl across a loop boundary ──────────────────────────
        guard let headlines = data("crawl.csv", key: nil) else { expect(false, "crawl.csv"); return }
        let crawlBinding = GraphicsDataBinding(sourceId: GraphicsDataTableForm.newSourceId(), keyColumn: nil, columns: ["item"], sortColumn: nil,
                                               limit: 3, revision: 1, crawl: GraphicsCrawlOptions(speed: 1200, gap: 0, direction: "left", mode: "loop"))
        let crawlCreated = awaitMain(20) { await app.graphics.createDataDesign(name: "Live crawl", data: headlines, binding: crawlBinding) }
        log("create crawl: \(crawlCreated?.message ?? "nil")")
        guard crawlCreated?.ok == true, let crawlRowId = crawlCreated?.rowId, let crawlRow = app.graphics.document.row(crawlRowId) else {
            expect(false, "live: the crawl design is created"); return
        }
        let crawlEntry = GraphicsPlayout.entry(for: crawlRow)
        let crawlShown = awaitMain(15) { await app.graphics.playout.show(crawlEntry) }
        expectEqual(crawlShown?.accepted, true, "live: the crawl shows (\(crawlShown?.reason ?? ""))")
        guard waitFor("the crawl playing", 10, { app.graphics.state?.instance(crawlEntry.instanceId)?.crawlPlayback == "playing" }) else { return }
        let crawlLineage = app.graphics.state?.instance(crawlEntry.instanceId)
        guard let v2Crawl = data("crawl-v2.csv", key: nil) else { expect(false, "crawl-v2.csv"); return }
        _ = awaitMain(10) { await app.graphics.stageData(rowId: crawlRowId, binding: crawlBinding, data: v2Crawl).value }
        let queued = awaitMain(15) { await app.graphics.publishData(rowId: crawlRowId).value }
        log("crawl publish: \(queued?.reason ?? "nil")")
        expectEqual(queued?.reason, "Data revision 2 queued for a loop boundary.", "live: a playing loop crawl queues the revision")
        poll()
        let pending = app.graphics.state?.instance(crawlEntry.instanceId)?.crawlPendingRevision ?? 0
        log("crawlPendingRevision right after publish: \(pending)")
        expect(pending > 0, "live: the revision is pending until a loop boundary")
        var crawlVisibilities: [String] = []
        let adopted = waitFor("Program adopting crawl revision 2 at a loop boundary", 20) {
            let i = app.graphics.state?.instance(crawlEntry.instanceId)
            crawlVisibilities.append(i?.visibility ?? "gone")
            return (i?.programFields?[GraphicsCrawlData.revisionField] as? String) == "2"
        }
        let adoptedInstance = app.graphics.state?.instance(crawlEntry.instanceId)
        log("adopted=\(adopted) programFields=\(String(describing: adoptedInstance?.programFields)) pending=\(adoptedInstance?.crawlPendingRevision ?? -1)")
        expectEqual(adoptedInstance?.instanceId, crawlLineage?.instanceId, "live: the same crawl instance")
        expect(crawlVisibilities.allSatisfy { $0 != "hidden" && $0 != "gone" }, "live: the crawl never left Program (\(Set(crawlVisibilities)))")
        let paused = awaitMain(10) { await app.graphics.controlCrawl(rowId: crawlRowId, action: "pause").value }
        expectEqual(paused?.accepted, true, "live: Pause accepted")
        _ = waitFor("the crawl paused", 5) { app.graphics.state?.instance(crawlEntry.instanceId)?.crawlPlayback == "paused" }
        settle(0.3)
        let p1 = programPixels(surfaceId)?.bytes ?? []
        settle(0.5)
        let p2 = programPixels(surfaceId)?.bytes ?? []
        let pausedDelta = changedFraction(p1, p2)
        log(String(format: "paused: %.4f%% differ across 0.5 s", pausedDelta * 100))
        expect(pausedDelta == 0, "live: a paused crawl holds Program still")
        _ = awaitMain(10) { await app.graphics.controlCrawl(rowId: crawlRowId, action: "resume").value }
        _ = waitFor("the crawl playing again", 5) { app.graphics.state?.instance(crawlEntry.instanceId)?.crawlPlayback == "playing" }
        settle(0.3)
        let r1 = programPixels(surfaceId)?.bytes ?? []
        settle(0.5)
        let r2 = programPixels(surfaceId)?.bytes ?? []
        let resumedDelta = changedFraction(r1, r2)
        log(String(format: "resumed: %.3f%% differ across 0.5 s", resumedDelta * 100))
        expect(resumedDelta > 0, "live: a resumed crawl moves")
        _ = awaitMain(10) { await app.graphics.playout.hide(crawlEntry.instanceId) }
        _ = waitFor("the crawl off Program", 10) { app.graphics.state?.instance(crawlEntry.instanceId)?.isOnProgram == false }
        settle(1.0)
        let afterHide = changedFraction(baseline.bytes, programPixels(surfaceId)?.bytes ?? [])
        log(String(format: "after Hide %.3f%% differ from baseline", afterHide * 100))
        expect(afterHide < 0.001, "live: Hide returns Program to the baseline")
    }
}
