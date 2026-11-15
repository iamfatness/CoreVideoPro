// Virtual camera — shell side (roadmap M9.2, docs/virtual-camera-macos-spec.md).
//
// The shell never touches pixels. It holds the operator's INTENT (enable,
// mirror, OH Framer, device name — persisted), sends the same
// `sync-virtual-camera` command Windows sends, reads the same `virtualCamera`
// snapshot node, and layers the Mac-only camera-extension ACTIVATION state
// (CameraExtensionActivation.swift) on top for the approval UI.
//
// Everything decidable without a window or a core is pure and lives here so
// the harness can pin it: the read model, the status label precedence, the
// framer label (a verbatim port of Windows VirtualCameraFramerStatusLabel) and
// the wire command.

import CameraExtensionKit
import Foundation

/// Operator intent, persisted in ShellPrefs (optional key: an older prefs file
/// decodes to nil and keeps every other field). An empty `deviceName` means
/// "use the default name" (CameraConstants.defaultDeviceName).
struct PersistedVirtualCamera: Codable, Equatable {
    var enabled = false
    var mirror = false
    var framerEnabled = false
    var deviceName = ""
}

extension PersistedVirtualCamera {
    /// Field-by-field, like ShellPrefs: a nested object written by an older
    /// build (or missing a field added later) keeps what it has instead of
    /// throwing and dropping the operator's whole virtual camera setup.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func v<T: Decodable>(_ key: CodingKeys, _ fallback: T) -> T {
            ((try? c.decodeIfPresent(T.self, forKey: key)) ?? nil) ?? fallback
        }
        self.init(enabled: v(.enabled, false), mirror: v(.mirror, false),
                  framerEnabled: v(.framerEnabled, false), deviceName: v(.deviceName, ""))
    }
}

/// The core's `virtualCamera` snapshot node (CoreStateCapture::virtualCameraState).
struct VirtualCameraReadModel: Equatable {
    var enabled = false
    var status = "off"
    var warning = ""
    var width = 0
    var height = 0
    var framerState = "off"
    var framerWarning = ""
    var framesPublished = 0

    /// nil when the snapshot carries no `virtualCamera` node — "not observed",
    /// never a default "off".
    static func parse(_ snapshot: JSONObject) -> VirtualCameraReadModel? {
        guard let node = snapshot["virtualCamera"] as? JSONObject else { return nil }
        func int(_ value: Any?) -> Int { (value as? NSNumber)?.intValue ?? 0 }
        let resolution = node["resolution"] as? JSONObject ?? [:]
        var m = VirtualCameraReadModel()
        m.enabled = node["enabled"] as? Bool ?? false
        m.status = node["status"] as? String ?? "off"
        m.warning = node["warning"] as? String ?? ""
        m.width = int(resolution["width"])
        m.height = int(resolution["height"])
        m.framerState = node["framerState"] as? String ?? "off"
        m.framerWarning = node["framerWarning"] as? String ?? ""
        m.framesPublished = int(node["framesPublished"])
        return m
    }
}

enum VirtualCameraPresentation {
    /// Core warning codes that mean "the device isn't reachable yet" (not
    /// installed, awaiting approval, restarting, updated) — the core retries
    /// every 1 s, so the operator sees progress, not an error (spec §5).
    static let connectingPrefixes = ["CAMERA_EXTENSION_NOT_FOUND:", "CAMERA_EXTENSION_LOST:"]

    /// Status line. Precedence: operator intent, then the extension's
    /// activation state, then the core's own status (Windows wording where it
    /// exists — StudioViewModel.VirtualCameraStatusLabel).
    static func label(intent: Bool, ext: CameraExtensionState, core: VirtualCameraReadModel?) -> String {
        if !intent { return "Virtual camera off" }
        switch ext {
        case .activating: return "Installing the virtual camera\u{2026}"
        case .needsApproval: return "Allow CoreVideo Pro Camera in System Settings to finish setup."
        case .notInApplications: return "Move CoreVideo Pro to the Applications folder to use the virtual camera."
        case .needsReboot: return "Restart your Mac to finish installing the virtual camera."
        case .unavailable(let reason), .failed(let reason): return reason
        case .unknown, .notInstalled, .active: break
        }
        guard let core else { return "Virtual camera on" }
        switch core.status {
        case "live":
            return "Virtual camera live (\(core.width)x\(core.height))"
        case "failed":
            if connectingPrefixes.contains(where: { core.warning.hasPrefix($0) }) {
                return "Connecting to CoreVideo Pro Camera\u{2026}"
            }
            // Windows: an empty warning reads "Virtual camera failed".
            if core.warning.isEmpty { return "Virtual camera failed" }
            if let range = core.warning.range(of: ": ") {
                let rest = String(core.warning[range.upperBound...])
                return rest.isEmpty ? core.warning : rest
            }
            return core.warning
        default:
            return "Virtual camera on"
        }
    }

    /// The "Open System Settings" affordance exists only while approval is pending.
    static func showsSettingsButton(_ ext: CameraExtensionState) -> Bool {
        ext == .needsApproval
    }

    /// Verbatim port of Windows StudioViewModel.VirtualCameraFramerStatusLabel.
    static func framerLabel(framerEnabled: Bool, intent: Bool, core: VirtualCameraReadModel?) -> String {
        if !framerEnabled { return "Framer off" }
        if !intent { return "Ready when webcam output is enabled" }
        if core?.framerState == "unavailable" { return core?.framerWarning ?? "" }
        return core?.framerState == "active" ? "OH Framer active on webcam output" : "Waiting for webcam frame"
    }
}

extension CoreCommands {
    /// Root-level command (rides inside a media-core-sync `commands` array).
    /// `on` is ALWAYS sent: the core treats a missing `on` as TRUE. The core
    /// ignores an empty `deviceName`, so a blank field sends the default name
    /// (DeviceNamePolicy also trims, drops controls and caps the length).
    static func syncVirtualCamera(_ v: PersistedVirtualCamera) -> JSONObject {
        ["type": "sync-virtual-camera",
         "on": v.enabled,
         "mirror": v.mirror,
         "framerEnabled": v.framerEnabled,
         "deviceName": DeviceNamePolicy.sanitized(v.deviceName)]
    }

    /// The keys MediaCore::syncVirtualCamera actually reads, re-derived from
    /// the core's source (`command.get("<key>")` inside that function), so a
    /// renamed key on either side fails a test instead of being silently ignored.
    /// nil in a packaged app (no repo alongside), which the test reports as SKIPPED.
    static func syncVirtualCameraKeysFromCoreSource() -> Set<String>? {
        locateCoreSource("native/src/core/MediaCore.cpp").flatMap { path -> Set<String>? in
            guard let source = try? String(contentsOfFile: path, encoding: .utf8),
                  let start = source.range(of: "void MediaCore::syncVirtualCamera(") else { return nil }
            // The function body ends at the first line that is exactly "}".
            let tail = source[start.upperBound...]
            let end = tail.range(of: "\n}\n")?.lowerBound ?? tail.endIndex
            let body = tail[..<end]
            var keys: Set<String> = []
            var rest = body[...]
            while let hit = rest.range(of: "command.get(\"") {
                let after = rest[hit.upperBound...]
                guard let close = after.firstIndex(of: "\"") else { break }
                keys.insert(String(after[..<close]))
                rest = after[close...]
            }
            return keys.isEmpty ? nil : keys
        }
    }

    /// Walks up from the running binary looking for a repo-relative core source.
    static func locateCoreSource(_ relative: String) -> String? {
        var dir = URL(fileURLWithPath: CommandLine.arguments.first ?? ".").deletingLastPathComponent()
        for _ in 0..<8 {
            let candidate = dir.appendingPathComponent(relative)
            if FileManager.default.fileExists(atPath: candidate.path) { return candidate.path }
            dir = dir.deletingLastPathComponent()
        }
        return nil
    }
}
