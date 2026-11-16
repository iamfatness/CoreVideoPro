import Foundation

/// Identifiers and limits shared by the shell and the CoreMediaIO camera extension.
public enum CameraConstants {
    public static let deviceUUID = UUID(uuidString: "791E98ED-72E7-4716-9A26-1B4FBB9A1520")!
    public static let sourceStreamUUID = UUID(uuidString: "88409CE1-9E7E-4E22-817E-7E039C84C1EF")!
    public static let sinkStreamUUID = UUID(uuidString: "454F962A-782B-45DB-A9E6-BE09984FBBA7")!

    public static let legacyDeviceID = "us.iamfatness.corevideopro.camera.device"
    public static let defaultDeviceName = "CoreVideo Pro Camera"
    public static let sinkStreamName = "CoreVideo Pro Camera Sink"
    public static let machServiceName = "42394L3353.us.iamfatness.corevideopro.shell.camera"
    public static let extensionBundleID = "us.iamfatness.corevideopro.shell.camera"
    /// The camera extension's OWN version: its CFBundleVersion and
    /// CFBundleShortVersionString (scripts/make-macos-shell-bundle.sh reads it
    /// from this line). macOS replaces an installed extension whenever this
    /// differs, and a replacement can need a restart, so it moves ONLY when
    /// the extension changes, never with the app's build number.
    /// scripts/camera-extension-version.sh fails the release and CI when the
    /// extension's sources changed but this did not: bump it, then run
    /// `scripts/camera-extension-version.sh update`.
    public static let extensionVersion = 1
    /// Spec §4 law 6, tightened to Developer ID (2026-11-15 security review): an
    /// Apple-anchored chain whose intermediate is the Developer ID CA
    /// (1.2.840.113635.100.6.2.6) and whose leaf is a Developer ID Application
    /// certificate (1.2.840.113635.100.6.1.13) for our team, plus the identifier.
    public static let producerRequirement = #"anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists and certificate leaf[subject.OU] = "42394L3353" and (identifier "us.iamfatness.corevideopro.core" or identifier "us.iamfatness.corevideopro.vcam-probe")"#

    /// How long the last program frame is held after the producer stops delivering.
    public static let holdNanoseconds: UInt64 = 500_000_000
    public static let sinkWidth = 1920
    public static let sinkHeight = 1080
}

public struct CameraFormat: Equatable {
    public var width: Int
    public var height: Int
    public var fps: [Int]
    public init(width: Int, height: Int, fps: [Int]) {
        self.width = width; self.height = height; self.fps = fps
    }
}

public enum CameraFormats {
    /// Task 10 appends 720p.
    public static let source: [CameraFormat] = [CameraFormat(width: 1920, height: 1080, fps: [60, 30])]
}
