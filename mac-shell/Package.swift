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
        )
    ]
)
