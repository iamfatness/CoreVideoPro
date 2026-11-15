// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CoreVideoProShell",
    platforms: [.macOS(.v13)],
    targets: [
        .target(
            name: "CameraExtensionKit",
            path: "Sources/CameraExtensionKit"
        ),
        .executableTarget(
            name: "CoreVideoProShell",
            dependencies: ["CameraExtensionKit"],
            path: "Sources/CoreVideoProShell"
        ),
        // The CoreMediaIO camera extension (roadmap M9.2). Assembled into
        // Contents/Library/SystemExtensions/…camera.systemextension by
        // scripts/make-macos-shell-bundle.sh; never run directly.
        .executableTarget(
            name: "CoreVideoProCamera",
            dependencies: ["CameraExtensionKit"],
            path: "Sources/CoreVideoProCamera",
            linkerSettings: [
                .linkedFramework("CoreMediaIO"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("IOSurface"),
                .linkedFramework("Security"),
                .linkedFramework("VideoToolbox")
            ]
        ),
        // Capture probe for the virtual camera's local e2e check
        // (scripts/macos-vcam-e2e.sh, roadmap M9.2 Task 7). A developer tool:
        // never bundled into the app.
        .executableTarget(
            name: "VcamProbe",
            dependencies: ["CameraExtensionKit"],
            path: "Sources/VcamProbe",
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo")
            ]
        )
    ]
)
