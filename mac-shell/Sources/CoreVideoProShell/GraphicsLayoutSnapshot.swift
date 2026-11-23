// GraphicsLayoutSnapshot — COREVIDEO_SHELL_GRAPHICS_SNAPSHOT=<dir> renders the
// WHOLE main window (header, readiness strip, the Graphics tab, footer) offscreen
// at fixed window sizes with a staged graphics node and show, then exits. No core,
// no operator file: the staged show lives in a temp document (stageDocument). Layout proof for G4-G7
// Task 1 (the inspector must sit beside the playlist at every size).
//   COREVIDEO_SHELL_GRAPHICS_SELECT=<selection key>  (default "guest")
//   COREVIDEO_SHELL_GRAPHICS_SIZES=1360x860,1600x900  (default: the Mac minimum 1360x860,
//   1600x900 and 1920x1080)
//   COREVIDEO_SHELL_GRAPHICS_SURFACE=design  renders the Design surface, plus the Create data table
//   sheet over scores.csv as its own PNG

import AppKit
import SwiftUI

@MainActor
enum GraphicsLayoutSnapshot {
    /// The staged show: Windows' interview rows plus the names/clock rows, written to a TEMP document
    /// that COREVIDEO_GRAPHICS_DOCUMENT_PATH points the model at before it exists, so the operator's real
    /// show is never read, photographed or written.
    static func stageDocument() -> String {
        let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("cvp-graphics-snapshot-\(getpid())")
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let path = (dir as NSString).appendingPathComponent("default.json")
        var show = ShellTests.loadGraphicsNode("documents/windows-show.json")
        let names = ShellTests.loadGraphicsNode("documents/windows-names-clock.json")
        show["playlist"] = (show["playlist"] as? [JSONObject] ?? []) + (names["playlist"] as? [JSONObject] ?? [])
        // The Windows data show's designs, so the Design pane's YOUR DESIGNS bar has chips.
        show["designs"] = ShellTests.loadGraphicsNode("documents/windows-data.json")["designs"]
        if let data = try? JSONSerialization.data(withJSONObject: show) { FileManager.default.createFile(atPath: path, contents: data) }
        setenv("COREVIDEO_GRAPHICS_DOCUMENT_PATH", path, 1)
        return path
    }

    static func seed(_ model: AppModel) {
        let graphics = model.graphics   // loads the staged temp document
        let deadline = Date().addingTimeInterval(5)
        while !graphics.documentLoaded && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        graphics.coreRunning = { true }
        var node = ShellTests.loadGraphicsNode("nodes/program-and-cue.json")
        node["coreEpoch"] = "snapshot"
        node["instances"] = [
            ScriptedGraphicsCore.instance(GraphicsPlayoutIds.instanceId(rowId: "logo", sourceId: ""), program: 4, visibility: "visible",
                                          template: "corner-logo"),
            ScriptedGraphicsCore.instance(GraphicsPlayoutIds.instanceId(rowId: "title", sourceId: ""), cue: 2, template: "centered-title"),
        ] as [JSONObject]
        graphics.observe(snapshot: ["graphics": node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]])
        graphics.status = "Show title cued. Take carries it to Program."
        graphics.selectedKey = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_GRAPHICS_SELECT"] ?? "guest"
        model.selectedTab = .overlays
    }

    static func render(_ model: AppModel, size: CGSize, to url: URL) -> Bool {
        let host = NSHostingView(rootView: RootView().environmentObject(model)
            .frame(width: size.width, height: size.height)
            .environment(\.colorScheme, .dark))
        let window = NSWindow(contentRect: NSRect(x: -10_000, y: -10_000, width: size.width, height: size.height),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: .darkAqua)
        window.contentView = host
        host.frame = NSRect(origin: .zero, size: size)
        host.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        host.layoutSubtreeIfNeeded()
        host.display()
        guard let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return false }
        host.cacheDisplay(in: host.bounds, to: bitmap)
        guard let png = bitmap.representation(using: .png, properties: [:]) else { return false }
        return (try? png.write(to: url)) != nil
    }

    static func runAndExit(dir: String) -> Never {
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        _ = stageDocument()
        let design = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_GRAPHICS_SURFACE"] == "design"
        if design { GraphicsSurface.initial = .design }
        let model = AppModel()
        seed(model)
        let sizes = (ProcessInfo.processInfo.environment["COREVIDEO_SHELL_GRAPHICS_SIZES"] ?? "1360x860,1600x900,1920x1080")
            .split(separator: ",").compactMap { spec -> CGSize? in
                let parts = spec.split(separator: "x").compactMap { Double($0) }
                return parts.count == 2 ? CGSize(width: parts[0], height: parts[1]) : nil
            }
        if design, let data = try? GraphicsLocalData.read(url: ShellTests.repoRoot.appendingPathComponent("contracts/graphics-shell/data/scores.csv"), key: nil) {
            let picked = GraphicsPickedData(fileName: "scores", data: data, crawl: false)
            let view = GraphicsDataTableSheet(graphics: model.graphics, picked: picked, sheet: .constant(GraphicsDataSheetState(isPresented: true)))
            let host = NSHostingView(rootView: view.environment(\.colorScheme, .dark))
            host.frame = NSRect(origin: .zero, size: host.fittingSize)
            let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
            window.contentView = host
            host.layoutSubtreeIfNeeded(); host.display()
            if let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
                host.cacheDisplay(in: host.bounds, to: bitmap)
                let url = URL(fileURLWithPath: dir).appendingPathComponent("graphics-data-table-sheet.png")
                try? bitmap.representation(using: .png, properties: [:])?.write(to: url)
                print("graphics snapshot \(url.path) ok")
            }
        }
        let tag = (design ? "design-" : "") + (ProcessInfo.processInfo.environment["COREVIDEO_SHELL_GRAPHICS_SELECT"] ?? "guest")
            .replacingOccurrences(of: "|", with: "_").replacingOccurrences(of: ":", with: "-")
        for size in sizes {
            let url = URL(fileURLWithPath: dir).appendingPathComponent("graphics-\(tag)-\(Int(size.width))x\(Int(size.height)).png")
            print("graphics snapshot \(url.path) \(render(model, size: size, to: url) ? "ok" : "FAILED")")
        }
        exit(0)
    }
}
