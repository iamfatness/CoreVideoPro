// The core's graphics fonts on a real install (Metal graphics backend G2 fix).
//
// On a pkg install the shell runs the core from
// /Library/Application Support/CoreVideo Pro/corevideo-native (CoreLocation),
// a root-owned copy that has NO GraphicsFonts/ beside it: make-macos-pkg.sh
// stages only the core there. The core's own lookup (GraphicsFonts/ beside the
// executable) therefore finds nothing, Segoe UI resolves unavailable and strict
// Program graphics fail. The shell must name the app's GraphicsFonts folder in
// COREVIDEO_GRAPHICS_FONTS_DIR on every core launch, exactly as it names the
// plugin host. Registered in ShellTests.runAndExit.

import Foundation

extension ShellTests {
    static let graphicsFontsCases: [(String, () -> Void)] = [
        ("graphics-fonts/launch-env-names-the-app-fonts-for-the-installed-core", testLaunchEnvNamesTheAppFonts),
        ("graphics-fonts/directory-resolution", testGraphicsFontsDirectoryResolution),
    ]

    static func testLaunchEnvNamesTheAppFonts() {
        var paths = ShellPaths.Paths()
        paths.corePath = "/Library/Application Support/CoreVideo Pro/corevideo-native"
        paths.coreCameraEligible = true
        paths.pluginHostPath = "/Applications/CoreVideo Pro.app/Contents/Resources/corevideo-plugin-host"
        paths.graphicsFontsDirectory = "/Applications/CoreVideo Pro.app/Contents/Resources/GraphicsFonts"
        let env = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil)
        expectEqual(env["COREVIDEO_GRAPHICS_FONTS_DIR"], "/Applications/CoreVideo Pro.app/Contents/Resources/GraphicsFonts",
                    "the installed core (outside the app) is told where the app's graphics fonts are")
        // Move-only guard for the keys this builder took over from AppModel.
        expectEqual(env["COREVIDEO_PLUGIN_HOST_PATH"], paths.pluginHostPath, "plugin host path still set")
        expectEqual(env["COREVIDEO_ZOOM_JOIN_WAIT_MS"], ZoomJoinStatus.coreJoinWaitMs, "join wait default still set")
        expect(env["COREVIDEO_VST3_SCAN_PATH"] != nil, "VST3 scan root still set")

        let override = CoreLaunchEnvironment.extras(
            paths: paths, processEnvironment: ["COREVIDEO_GRAPHICS_FONTS_DIR": "/tmp/fonts-override"], zoomPublicAppKey: nil)
        expectEqual(override["COREVIDEO_GRAPHICS_FONTS_DIR"], "/tmp/fonts-override", "an explicit override wins")

        paths.graphicsFontsDirectory = nil
        let none = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: "key-1")
        expect(none["COREVIDEO_GRAPHICS_FONTS_DIR"] == nil, "no folder found: the variable is not invented")
        expectEqual(none["COREVIDEO_ZOOM_PUBLIC_APP_KEY"], "key-1", "app key still forwarded")
    }

    static func testGraphicsFontsDirectoryResolution() {
        let root = NSTemporaryDirectory() + "cvp-graphics-fonts-\(UUID().uuidString)"
        let resources = root + "/App.app/Contents/Resources"
        let repo = root + "/repo"
        let fm = FileManager.default
        defer { try? fm.removeItem(atPath: root) }
        try? fm.createDirectory(atPath: resources, withIntermediateDirectories: true)
        try? fm.createDirectory(atPath: repo + "/native/build-metal/GraphicsFonts", withIntermediateDirectories: true)
        expectEqual(ShellPaths.graphicsFontsDirectory(bundleResources: resources, repoRoot: repo),
                    repo + "/native/build-metal/GraphicsFonts", "a dev run falls back to the build's fonts")
        try? fm.createDirectory(atPath: resources + "/GraphicsFonts", withIntermediateDirectories: true)
        expectEqual(ShellPaths.graphicsFontsDirectory(bundleResources: resources, repoRoot: repo),
                    resources + "/GraphicsFonts", "the app's own fonts win")
        expect(ShellPaths.graphicsFontsDirectory(bundleResources: root + "/none", repoRoot: root + "/none") == nil,
               "nothing found is nil")
    }
}
