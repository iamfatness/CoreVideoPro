// Graphics G1-G3 Task 12 — the opt-in live check (plan D11).
//
// COREVIDEO_SHELL_LIVE_CORE=<path to corevideo-native> runs it; otherwise it
// skips loudly. It launches a REAL Metal core through MediaCoreBridge with
// CoreLaunchEnvironment.extras (never the owner's app), drives the real
// GraphicsModel / GraphicsPlayout and the real AppModel Take, and judges
// Program PIXELS from the core's exported Program IOSurface (BGRA8):
//   1. Show a Title row -> firstDrawSubmittedFrame > 0 and Program pixels change;
//   2. Hide -> Program returns to the baseline;
//   3. Cue the row, then an ordinary Take -> the graphic is on Program.

import Foundation
import IOSurface

extension ShellTests {
    static var graphicsLiveCases: [(String, () -> Void)] {
        [("graphics-live/show-hide-cue-take-against-a-real-core", testGraphicsLiveCore)]
    }

    final class LiveFlag: @unchecked Sendable { var connected = false }

    /// Program BGRA pixels from an IOSurface id, or nil.
    static func programPixels(_ surfaceId: UInt32) -> (width: Int, height: Int, bytes: [UInt8])? {
        guard surfaceId != 0, let surface = IOSurfaceLookup(surfaceId) else { return nil }
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface), stride = IOSurfaceGetBytesPerRow(surface)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        var bytes = [UInt8](repeating: 0, count: width * height * 4)
        for y in 0..<height { for x in 0..<(width * 4) { bytes[y * width * 4 + x] = base[y * stride + x] } }
        return (width, height, bytes)
    }

    /// Fraction of pixels whose any channel differs by more than 24.
    static func changedFraction(_ a: [UInt8], _ b: [UInt8]) -> Double {
        guard a.count == b.count, !a.isEmpty else { return 1 }
        var changed = 0
        var i = 0
        while i < a.count {
            if abs(Int(a[i]) - Int(b[i])) > 24 || abs(Int(a[i + 1]) - Int(b[i + 1])) > 24 || abs(Int(a[i + 2]) - Int(b[i + 2])) > 24 {
                changed += 1
            }
            i += 4
        }
        return Double(changed) / Double(a.count / 4)
    }

    @MainActor
    static func testGraphicsLiveCore() {
        guard let corePath = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_LIVE_CORE"], !corePath.isEmpty else {
            FileHandle.standardError.write(Data("  [skip] graphics live check: set COREVIDEO_SHELL_LIVE_CORE=<corevideo-native>\n".utf8))
            return
        }
        func log(_ line: String) { FileHandle.standardError.write(Data("  [live] \(line)\n".utf8)) }
        let fonts = ProcessInfo.processInfo.environment["COREVIDEO_GRAPHICS_FONTS_DIR"]
            ?? repoRoot.appendingPathComponent("native/resources/graphics-fonts").path
        let paths = ShellPaths.Paths(corePath: corePath, engineBinaryPath: nil, pluginHostPath: nil, graphicsFontsDirectory: fonts)
        let extras = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil)
        expectEqual(extras["COREVIDEO_GRAPHICS_FONTS_DIR"], fonts, "the launch names the fonts folder (installed-layout rule)")
        let bridge = MediaCoreBridge(corePath: corePath, environmentExtras: extras)
        let flag = LiveFlag()
        bridge.onStatus = { status in if case .connected = status { flag.connected = true } }
        bridge.start()
        defer { bridge.stop() }
        spin(until: { flag.connected }, limit: 30)
        guard flag.connected else { expect(false, "live core did not connect"); return }

        let app = AppModel()
        app.quitRequestForTesting = { body, timeout in try await bridge.request(body, timeout: timeout) }
        app.status = .connected(renderer: "metal")
        app.onConnected()   // as the app does on every core generation: scenes, Preview != Program, brand kit
        spin(until: { app.graphics.documentLoaded }, limit: 3)
        app.graphics.saveDelayNs = 0
        var surfaceId: UInt32 = 0

        /// One empty sync: the core answers with its snapshot; graphics observes it.
        @discardableResult
        func poll() -> JSONObject {
            let reply = awaitMain(8) { (try? await bridge.request(["type": "media-core-sync", "elapsedMs": app.elapsedMs(), "commands": []])) ?? [:] } ?? [:]
            let snapshot = reply["snapshot"] as? JSONObject ?? [:]
            if !snapshot.isEmpty { app.graphics.observe(snapshot: snapshot) }
            if let id = (snapshot["programSharedTexture"] as? JSONObject)?["iosurfaceId"] as? NSNumber {   // as AppModel.applySharedTexture
                surfaceId = id.uint32Value
            }
            return snapshot
        }
        func waitFor(_ what: String, _ limit: TimeInterval = 10, _ condition: () -> Bool) -> Bool {
            let end = Date().addingTimeInterval(limit)
            while Date() < end { poll(); if condition() { return true }; RunLoop.main.run(until: Date().addingTimeInterval(0.1)) }
            expect(false, "live: timed out waiting for \(what)")
            return false
        }
        // The cue wait polls the REAL core (the app's 2 Hz sync does this in production).
        app.graphics.playout.sleep = { ns in try? await Task.sleep(nanoseconds: ns); _ = poll() }

        guard waitFor("graphics available", 15, { app.graphics.available }) else { return }
        log("graphics available, epoch \(app.graphics.state?.coreEpoch ?? "?")")
        // Put a real scene on Program through the real Mac Take.
        let first = awaitMain(10) { await app.take() }
        log("first Take: \(String(describing: first))")
        guard waitFor("a Program surface", 10, { programPixels(surfaceId) != nil }) else { return }
        RunLoop.main.run(until: Date().addingTimeInterval(0.5)); poll()
        guard let baseline = programPixels(surfaceId) else { return }
        log("Program surface \(surfaceId) \(baseline.width)x\(baseline.height)")

        guard let defaults = app.graphics.state.flatMap(GraphicsDefaultRows.interview) else {
            expect(false, "live: the core's catalog has no starter templates"); return
        }
        var rows = defaults
        if let index = rows.firstIndex(where: { $0.id == "title" }) { rows[index].playOutAfterMs = 0 }
        app.graphics.document.playlist = rows
        let title = GraphicsPlayout.entry(for: rows.first { $0.id == "title" }!)

        // 0. Measure the first preparation of this template on this core (diagnostic).
        let started = Date()
        let firstCue = awaitMain(15) { await app.graphics.playout.cue(title) }
        let i0 = app.graphics.state?.instance(title.instanceId)
        log(String(format: "first Cue after %.2fs: %@ (cuePending=%@ cuePreparation=%@ cueWarning=%@ preparation=%@)",
                   Date().timeIntervalSince(started), firstCue?.reason ?? "nil", "\(i0?.cuePending ?? false)",
                   i0?.cuePreparation ?? "-", i0?.cueWarning ?? "-", i0?.preparation ?? "-"))
        if firstCue?.accepted != true {
            _ = waitFor("the first cue to finish preparing", 20) { app.graphics.state?.instance(title.instanceId)?.cuePending == false }
            log(String(format: "first preparation finished after %.2fs", Date().timeIntervalSince(started)))
        }
        _ = awaitMain(10) { await app.graphics.playout.clearCued() }
        poll()

        // 1. Show
        let shown = awaitMain(15) { await app.graphics.playout.show(title) }
        log("Show: \(shown?.accepted == true) \(shown?.reason ?? "nil")")
        expectEqual(shown?.accepted, true, "live: Show accepted by the real core")
        _ = waitFor("first draw of the title", 10) {
            (app.graphics.state?.instance(title.instanceId)?.firstDrawSubmittedFrame ?? 0) > 0
                && app.graphics.state?.instance(title.instanceId)?.isOnProgram == true
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.6)); poll()
        let onAir = programPixels(surfaceId)?.bytes ?? []
        let showDelta = changedFraction(baseline.bytes, onAir)
        log(String(format: "Show changed %.2f%% of Program pixels", showDelta * 100))
        expect(showDelta > 0.005, "live: the shown title changes Program pixels")
        expectEqual(app.graphics.strip.onProgram.map(\.instanceId), [title.instanceId], "live: one ON AIR chip, from core truth")

        // 2. Hide
        let hidden = awaitMain(10) { await app.graphics.playout.hide(title.instanceId) }
        expectEqual(hidden?.accepted, true, "live: Hide accepted")
        _ = waitFor("the title off Program", 10) { app.graphics.state?.instance(title.instanceId)?.isOnProgram == false }
        RunLoop.main.run(until: Date().addingTimeInterval(0.8)); poll()
        let afterHide = changedFraction(baseline.bytes, programPixels(surfaceId)?.bytes ?? [])
        log(String(format: "after Hide %.3f%% differ from baseline", afterHide * 100))
        expect(afterHide < 0.001, "live: Hide returns Program to the baseline")

        // 3. Cue, then the ordinary Mac Take
        let cued = awaitMain(15) { await app.graphics.playout.cue(title) }
        log("Cue: \(cued?.accepted == true) \(cued?.reason ?? "nil")")
        expectEqual(cued?.accepted, true, "live: Cue accepted")
        RunLoop.main.run(until: Date().addingTimeInterval(0.4)); poll()
        let cuedDelta = changedFraction(baseline.bytes, programPixels(surfaceId)?.bytes ?? [])
        expect(cuedDelta < 0.001, "live: a cue alone leaves Program untouched (\(cuedDelta))")
        let take = awaitMain(10) { await app.take() }
        log("Take: \(String(describing: take))")
        _ = waitFor("the cued title on Program after the Take", 10) {
            app.graphics.state?.instance(title.instanceId)?.isOnProgram == true
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.8)); poll()
        let takeDelta = changedFraction(baseline.bytes, programPixels(surfaceId)?.bytes ?? [])
        log(String(format: "after Take %.2f%% of Program pixels changed", takeDelta * 100))
        expect(takeDelta > 0.005, "live: the cued title rode the Take onto Program pixels")
        _ = awaitMain(10) { await app.graphics.playout.clearAll() }
    }
}
