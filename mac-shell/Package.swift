// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CoreVideoProShell",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "CoreVideoProShell",
            path: "Sources/CoreVideoProShell"
        )
    ]
)
