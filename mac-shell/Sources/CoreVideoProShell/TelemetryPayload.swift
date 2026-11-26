// TelemetryPayload.swift: the opt-in telemetry config, machine class and event payload
// (Phase 4 R4, docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md
// Task 7). Ports of Windows CrashReportingConfig (MC/Services/CrashReportUploader.cs),
// MachineClassProbe.cs, TelemetryEvent.cs (TelemetryPayloadBuilder) and
// TelemetryConsentStore.cs (TelemetryCrashCount), held to contracts/telemetry.fixtures.json,
// which the Windows MediaCore tests write.
//
// KEY HANDLING (owner decision D-T1): the API key comes ONLY from the process environment
// (COREVIDEO_TELEMETRY_API_KEY). Nothing is baked into the binary, and the key is never logged:
// TelemetryConfig's description says only whether a key is present.

import Darwin
import Foundation

struct TelemetryConfig: Equatable, CustomStringConvertible {
    static let defaultEndpoint = "https://telemetry.corevideo.io"
    static let endpointEnvVar = "COREVIDEO_TELEMETRY_ENDPOINT"
    static let apiKeyEnvVar = "COREVIDEO_TELEMETRY_API_KEY"

    let endpoint: String
    /// Trimmed; nil when the variable is unset. Never logged.
    let apiKey: String?

    /// A non-blank key AND an absolute http/https endpoint (Windows CrashReportingConfig.Enabled).
    var enabled: Bool {
        guard let apiKey, !apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              let scheme = Self.absoluteScheme(endpoint) else { return false }
        return scheme == "https" || scheme == "http"
    }

    /// Windows EndpointHost: the host of an absolute URI, else "(invalid)".
    var endpointHost: String {
        guard Self.absoluteScheme(endpoint) != nil, let host = URLComponents(string: endpoint)?.host else { return "(invalid)" }
        return host
    }

    var description: String {
        "TelemetryConfig(endpointHost: \(endpointHost), apiKey: \(apiKey.map { $0.isEmpty ? "empty" : "set" } ?? "missing"), enabled: \(enabled))"
    }

    /// An absolute URI's lower-cased scheme (with an authority), else nil.
    private static func absoluteScheme(_ text: String) -> String? {
        guard let parts = URLComponents(string: text), let scheme = parts.scheme?.lowercased(), !scheme.isEmpty,
              let host = parts.host, !host.isEmpty else { return nil }
        return scheme
    }

    /// Windows Resolve: a non-blank override wins (trimmed); the key is trimmed.
    static func resolve(endpointOverride: String?, apiKey: String?) -> TelemetryConfig {
        let override = endpointOverride?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        // A blank or whitespace key is NO key (Windows Enabled tests IsNullOrWhiteSpace).
        let key = apiKey?.trimmingCharacters(in: .whitespacesAndNewlines)
        return TelemetryConfig(endpoint: override.isEmpty ? defaultEndpoint : override,
                               apiKey: (key?.isEmpty ?? true) ? nil : key)
    }

    static func fromEnvironment(_ env: [String: String] = ProcessInfo.processInfo.environment) -> TelemetryConfig {
        resolve(endpointOverride: env[endpointEnvVar], apiKey: env[apiKeyEnvVar])
    }
}

struct TelemetryMachineClass: Codable, Equatable {
    var label: String
    var cpuCores: Int
    var ramBand: String
    var gpuTier: String?
}

/// Windows MachineClassProbe, Mac shape (D-T2): "mac-arm64-cpu<N>-ram<N>gb", no model code,
/// no serial, no stable identifier.
enum MachineClassProbe {
    static func describe(cpu: Int, physicalMemoryBytes: UInt64) -> String {
        guard physicalMemoryBytes > 0, cpu > 0 else { return "mac-arm64" }
        return "mac-arm64-cpu\(cpu)-ram\(ramGigabytes(physicalMemoryBytes))gb"
    }

    /// C# Math.Round(bytes / 2^30): half to EVEN.
    static func ramGigabytes(_ bytes: UInt64) -> Int {
        Int((Double(bytes) / 1_073_741_824).rounded(.toNearestOrEven))
    }

    /// Windows RamBand, band for band.
    static func ramBand(_ gb: Int) -> String {
        switch gb {
        case ...0: return "unknown"
        case ..<8: return "<8GB"
        case ..<16: return "8-16GB"
        case ..<32: return "16-32GB"
        case ..<64: return "32-64GB"
        case ..<128: return "64-128GB"
        default: return "128GB+"
        }
    }

    /// "Apple M3 Pro" -> "apple-m3-pro"; anything that is not an Apple M-series brand -> nil.
    static func gpuTier(brandString: String?) -> String? {
        guard let brand = brandString?.trimmingCharacters(in: .whitespacesAndNewlines) else { return nil }
        let words = brand.split(separator: " ").map(String.init)
        guard words.count >= 2, words[0] == "Apple", words[1].count >= 2, words[1].first == "M",
              words[1].dropFirst().allSatisfy(\.isNumber) else { return nil }
        var tier = "apple-" + words[1].lowercased()
        if words.count >= 3 {
            guard ["Pro", "Max", "Ultra"].contains(words[2]), words.count == 3 else { return nil }
            tier += "-" + words[2].lowercased()
        }
        return tier
    }

    static func probe() -> TelemetryMachineClass {
        let cpu = ProcessInfo.processInfo.processorCount
        let memory = ProcessInfo.processInfo.physicalMemory
        return TelemetryMachineClass(label: describe(cpu: cpu, physicalMemoryBytes: memory), cpuCores: cpu,
                                     ramBand: ramBand(memory > 0 ? ramGigabytes(memory) : 0),
                                     gpuTier: gpuTier(brandString: sysctlString("machdep.cpu.brand_string")))
    }

    private static func sysctlString(_ name: String) -> String? {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var buffer = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &buffer, &size, nil, 0) == 0 else { return nil }
        return String(cString: buffer)
    }
}

/// One crash report the server accepted (Windows CrashReportRecord; Task 10 reuses it).
struct CrashReportRecord: Codable, Equatable {
    var reportId: String
    var sentAtUtc: Date
    var dumpFiles: [String]
}

enum TelemetryCrashCount {
    /// Windows TelemetryCrashCount.CountSince: reports sent after the last telemetry send.
    static func countSince(reports: [CrashReportRecord], lastSentUtc: Date?) -> Int {
        guard let since = lastSentUtc else { return reports.count }
        return reports.filter { $0.sentAtUtc > since }.count
    }
}

struct TelemetryOutputConfigShape: Equatable {
    var recordingEnabled = false
    var streamingEnabled = false
    var vcamEnabled = false
    var isoSourceCount = 0
    var captureSourceCount = 0
    var zoomParticipantCount = 0
}

/// The event payload, in Windows' declaration order (the wire order).
struct TelemetryPayload: Equatable {
    var name: String
    var version: String
    var sessionLengthSeconds: Int64
    var outputConfigShape: TelemetryOutputConfigShape
    var crashCountSinceLastSend: Int
    var machine: TelemetryMachineClass
    var machineClass: String { machine.label }
}

enum TelemetryEventNames {
    static let sessionEnd = "session-end"
    static let heartbeat = "heartbeat"
}

enum TelemetryPayloadBuilder {
    /// Windows BuildOutputConfigShape over the core's own snapshot (the sessionState JSON):
    /// counts and on/off only. Nothing here reads a name, id, URL, key or path into the payload.
    static func outputConfigShape(snapshot: JSONObject?) -> TelemetryOutputConfigShape {
        guard let snapshot else { return TelemetryOutputConfigShape() }
        let devices = snapshot["captureDevices"] as? [JSONObject] ?? []
        let connected = Set(devices.filter { $0["connectionState"] as? String == "connected" }
            .compactMap { $0["id"] as? String })
        let senders = (snapshot["outputSenderSession"] as? JSONObject)?["senders"] as? [JSONObject] ?? []
        let streaming = senders.contains { sender in
            let destination = sender["destination"] as? String ?? ""
            // OutputDestinationKind.IsStream: not NDI (its own output), not the recording.
            let isStream = !destination.isEmpty && destination != "ndi" && destination != "ndi-graphics"
                && destination != "recording"
            return isStream && ["live", "starting", "warning"].contains(sender["status"] as? String ?? "")
        }
        return TelemetryOutputConfigShape(
            recordingEnabled: (snapshot["recording"] as? JSONObject)?["active"] as? Bool ?? false,
            streamingEnabled: streaming,
            vcamEnabled: (snapshot["virtualCamera"] as? JSONObject)?["enabled"] as? Bool ?? false,
            isoSourceCount: (snapshot["isoParticipantIds"] as? [Any])?.count ?? 0,
            captureSourceCount: connected.count,
            zoomParticipantCount: (snapshot["participants"] as? [Any])?.count ?? 0)
    }

    static func build(name: String, version: String, sessionLengthSeconds: Int64, snapshot: JSONObject?,
                      crashCountSinceLastSend: Int, machine: TelemetryMachineClass) -> TelemetryPayload {
        TelemetryPayload(name: name, version: version, sessionLengthSeconds: max(0, sessionLengthSeconds),
                         outputConfigShape: outputConfigShape(snapshot: snapshot),
                         crashCountSinceLastSend: max(0, crashCountSinceLastSend), machine: machine)
    }

    /// The ordered members (Windows camelCase declaration order; a nil gpuTier is omitted).
    private static func members(_ p: TelemetryPayload) -> [(String, Any)] {
        let shape = p.outputConfigShape
        var machine: [(String, Any)] = [("label", p.machine.label), ("cpuCores", p.machine.cpuCores),
                                        ("ramBand", p.machine.ramBand)]
        if let gpu = p.machine.gpuTier { machine.append(("gpuTier", gpu)) }
        return [
            ("name", p.name), ("version", p.version), ("sessionLengthSeconds", p.sessionLengthSeconds),
            ("outputConfigShape", [
                ("recordingEnabled", shape.recordingEnabled), ("streamingEnabled", shape.streamingEnabled),
                ("vcamEnabled", shape.vcamEnabled), ("isoSourceCount", shape.isoSourceCount),
                ("captureSourceCount", shape.captureSourceCount), ("zoomParticipantCount", shape.zoomParticipantCount),
            ] as [(String, Any)]),
            ("crashCountSinceLastSend", p.crashCountSinceLastSend), ("machineClass", p.machineClass),
            ("machine", machine),
        ]
    }

    /// Compact JSON, keys in Windows order (what is POSTed and logged).
    static func serializeWire(_ payload: TelemetryPayload) -> String { encode(members(payload), indent: nil) }

    /// Indented JSON for the "Preview what's sent" block.
    static func serializePreview(_ payload: TelemetryPayload) -> String { encode(members(payload), indent: "") }

    private static func encode(_ object: [(String, Any)], indent: String?) -> String {
        let inner = indent.map { $0 + "  " }
        let parts = object.map { key, value -> String in
            let rendered: String
            switch value {
            case let nested as [(String, Any)]: rendered = encode(nested, indent: inner)
            case let flag as Bool: rendered = flag ? "true" : "false"
            case let number as Int: rendered = String(number)
            case let number as Int64: rendered = String(number)
            default: rendered = ControlJSON.string(value as? String ?? "")
            }
            return ControlJSON.string(key) + (indent == nil ? ":" : ": ") + rendered
        }
        if let indent { return "{" + parts.map { "\n\(indent)  " + $0 }.joined(separator: ",") + "\n\(indent)}" }
        return "{" + parts.joined(separator: ",") + "}"
    }
}
