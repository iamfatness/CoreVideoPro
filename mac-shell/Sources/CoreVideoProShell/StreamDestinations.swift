// StreamDestinations.swift — the Mac's stream destination list (mac streaming
// Phase 2, slice D1; plan docs/superpowers/plans/2026-11-21-mac-streaming-phase2.md).
//
// Three fixed rows, one per protocol id the core has a sender for (rtmp, srt,
// hls). The validation sentences and the settings payload are ports of
// Windows' StudioStreamOutputValidation (ValidateSrt / ValidateHls) and
// StudioViewModel.StreamDestinations.cs. The STATUS projection is new: it is
// decided from the core's truthful lifecycle and the output supervisor
// (docs/reference/destination-lifecycle.md, output-supervisor.md), never from
// the published `status`, which can still carry the adapter's launch word
// while nothing has been accepted (OutputLifecyclePolicy.h publishedSenderStatus).

import Foundation

struct StreamDestinationsConfig: Equatable, Codable {
    var rtmpEnabled = true
    var rtmpUrl = "rtmp://a.rtmp.youtube.com/live2"
    var srtEnabled = false
    var srtMode = "caller"
    var srtHost = ""
    var srtPort = "9000"
    var srtLatencyMs = "120"
    var srtStreamId = ""
    var srtKeyLength = "0"
    var hlsEnabled = false
    var hlsPlaylistUrl = ""
}

/// Secrets never live in the config (it is persisted in ShellPrefs); they come
/// from the Keychain (StreamKeychain).
struct StreamDestinationSecrets: Equatable {
    var rtmpKey = ""
    var srtPassphrase = ""
}

enum StreamDestinationPolicy {
    static let destinationIds = ["rtmp", "srt", "hls"]
    static let srtModes = ["caller", "listener", "rendezvous"]
    static let srtKeyLengths = ["0", "16", "24", "32"]

    private static func trimmed(_ s: String) -> String { s.trimmingCharacters(in: .whitespacesAndNewlines) }
    /// Windows ParsePositiveInt (int.TryParse: surrounding whitespace allowed, sign allowed).
    private static func parseInt(_ s: String) -> Int? { Int(trimmed(s)) }

    static func normalizeSrtMode(_ mode: String) -> String {
        let value = trimmed(mode).lowercased()
        return srtModes.contains(value) ? value : "caller"
    }

    static func parseSrtKeyLength(_ text: String) -> Int? {
        guard let value = Int(text), [0, 16, 24, 32].contains(value) else { return nil }
        return value
    }

    /// The RTMP rule the Mac already applies to remote configuration
    /// (RtmpDestinationPolicy.validate = Windows ValidateRtmp); the protocol is
    /// the URL's own scheme.
    static func validateRtmp(url: String, key: String) -> String? {
        let scheme = URLComponents(string: trimmed(url))?.scheme?.lowercased() ?? "rtmps"
        return RtmpDestinationPolicy.validate(protocol: scheme, serverUrl: url, streamKey: key)
    }

    /// Windows StudioStreamOutputValidation.ValidateSrt, in Windows' order and words.
    static func validateSrt(mode: String, host: String, port: String, latencyMs: String,
                            streamId: String, keyLength: String, passphrase: String) -> String? {
        let normalizedMode = trimmed(mode).isEmpty ? "caller" : trimmed(mode).lowercased()
        guard srtModes.contains(normalizedMode) else {
            return "Choose SRT mode caller, listener, or rendezvous before streaming."
        }
        guard !trimmed(host).isEmpty else { return "Configure SRT host before streaming." }
        guard let portValue = parseInt(port), (1...65535).contains(portValue) else {
            return "Configure SRT port between 1 and 65535 before streaming."
        }
        guard let latency = parseInt(latencyMs), latency >= 20 else {
            return "Configure SRT latency of at least 20 ms before streaming."
        }
        if trimmed(streamId).count > 512 { return "SRT stream ID must be 512 characters or less." }
        guard let key = parseSrtKeyLength(keyLength) else {
            return "Choose SRT encryption key length 0, 16, 24, or 32."
        }
        let secret = trimmed(passphrase)
        if key > 0 && secret.isEmpty { return "Configure an SRT passphrase when encryption is enabled." }
        if key > 0 && !(10...79).contains(secret.count) {
            return "SRT passphrase must be 10 to 79 characters when encryption is enabled."
        }
        if key == 0 && !secret.isEmpty { return "Choose an SRT key length before entering a passphrase." }
        return nil
    }

    /// Windows StudioStreamOutputValidation.ValidateHls, same two sentences.
    static func validateHls(_ playlistUrl: String) -> String? {
        let value = trimmed(playlistUrl)
        let unsafe = value.unicodeScalars.contains { CharacterSet.whitespacesAndNewlines.contains($0) }
            || value.contains("\\") || value.contains("\"")
        // .NET's Uri accepts (and escapes) these characters, so Windows reaches the
        // second sentence for them; URLComponents would refuse to parse. Parse a
        // neutralised copy so the Mac answers with the same sentence.
        let parsable = unsafe
            ? String(value.map { $0.isWhitespace || $0 == "\\" || $0 == "\"" ? "x" : $0 }) : value
        guard let parts = URLComponents(string: parsable),
              let scheme = parts.scheme?.lowercased(), scheme == "http" || scheme == "https",
              let host = parts.host, !trimmed(host).isEmpty else {
            return "Configure an http:// or https:// HLS playlist URL before streaming."
        }
        if parts.user != nil || parts.password != nil || parts.query != nil || parts.fragment != nil
            || unsafe || !parts.path.hasSuffix(".m3u8") {
            return "HLS playlist URL must end in .m3u8 and contain no credentials, query, fragment, or whitespace."
        }
        return nil
    }

    static func validateSrt(_ c: StreamDestinationsConfig, _ s: StreamDestinationSecrets) -> String? {
        validateSrt(mode: c.srtMode, host: c.srtHost, port: c.srtPort, latencyMs: c.srtLatencyMs,
                    streamId: c.srtStreamId, keyLength: c.srtKeyLength, passphrase: s.srtPassphrase)
    }

    /// The row's own problem, or nil (shown under each row; nil for a disabled row).
    static func rowError(_ id: String, _ c: StreamDestinationsConfig, _ s: StreamDestinationSecrets) -> String? {
        switch id {
        case "rtmp": return c.rtmpEnabled ? validateRtmp(url: c.rtmpUrl, key: s.rtmpKey) : nil
        case "srt": return c.srtEnabled ? validateSrt(c, s) : nil
        case "hls": return c.hlsEnabled ? validateHls(c.hlsPlaylistUrl) : nil
        default: return nil
        }
    }

    /// Windows ValidateSelectedDestinations: why streaming cannot start, or nil.
    static func validateSelected(_ c: StreamDestinationsConfig, _ s: StreamDestinationSecrets) -> String? {
        if !c.rtmpEnabled && !c.srtEnabled && !c.hlsEnabled { return "Select at least one stream destination." }
        for id in destinationIds { if let error = rowError(id, c, s) { return error } }
        return nil
    }

    /// Enabled AND valid rows, in rtmp/srt/hls order (Windows BuildSelectedStreamDestinations(validatedOnly)).
    static func enabledDestinations(_ c: StreamDestinationsConfig, _ s: StreamDestinationSecrets) -> [String] {
        destinationIds.filter { id in
            let enabled = id == "rtmp" ? c.rtmpEnabled : id == "srt" ? c.srtEnabled : c.hlsEnabled
            return enabled && rowError(id, c, s) == nil
        }
    }

    /// `destinationSettings` for start-program-output: one entry per valid
    /// enabled row. An explicit destination bitrate wins over the stream
    /// profile in the core (#606), so each row carries the operator's value.
    static func destinationSettings(_ c: StreamDestinationsConfig, _ s: StreamDestinationSecrets,
                                    bitrateMbps: Double) -> [JSONObject] {
        let enabled = Set(enabledDestinations(c, s))
        var rows: [JSONObject] = []
        if enabled.contains("rtmp") {
            rows.append([
                "id": "rtmp", "label": "Stream", "protocol": "rtmp",
                "url": trimmed(c.rtmpUrl), "streamKey": trimmed(s.rtmpKey),
                "fps": 30, "targetBitrateMbps": bitrateMbps, "audioBitrateKbps": 160,
                "videoCodec": "h264", "encoderMode": "auto",
                "keyframeIntervalSeconds": 2.0, "rateControl": "cbr",
                "h264Profile": "high", "bFrames": 2, "allowEnhancedRtmp": false,
            ])
        }
        if enabled.contains("srt") {
            let latency = parseInt(c.srtLatencyMs) ?? 120
            var row: JSONObject = [
                "id": "srt", "label": "SRT",
                "mode": normalizeSrtMode(c.srtMode), "host": trimmed(c.srtHost),
                "port": parseInt(c.srtPort) ?? 0,
                "latencyMs": latency, "latencyUs": latency * 1000,
                "passphrase": trimmed(s.srtPassphrase),
                "keyLength": parseSrtKeyLength(c.srtKeyLength) ?? 0,
                "fps": 30, "targetBitrateMbps": bitrateMbps,
                "videoCodec": "h264", "encoderMode": "auto",
            ]
            if !trimmed(c.srtStreamId).isEmpty { row["streamId"] = trimmed(c.srtStreamId) }
            rows.append(row)
        }
        if enabled.contains("hls") {
            rows.append([
                "id": "hls", "label": "HLS", "protocol": "hls", "url": trimmed(c.hlsPlaylistUrl),
                "fps": 30, "targetBitrateMbps": bitrateMbps, "audioBitrateKbps": 160,
                "videoCodec": "h264", "encoderMode": "auto",
                "keyframeIntervalSeconds": 2.0, "rateControl": "cbr",
                "h264Profile": "high", "bFrames": 2,
            ])
        }
        return rows
    }

    static func displayName(_ id: String) -> String {
        switch id { case "rtmp": return "RTMP"; case "srt": return "SRT"; case "hls": return "HLS"; default: return id.uppercased() }
    }
}

// ── status projection ─────────────────────────────────────────────────────

enum DestinationTone: Equatable { case neutral, good, warn, bad }

struct DestinationStatus: Equatable {
    var label: String
    var tone: DestinationTone
    var detail: String = ""
    var showsRetry = false
    /// True ONLY when the lifecycle says producing (fresh progress) AND the
    /// supervisor has seen accepted units advance. Never from a launch.
    var isLive = false
}

enum DestinationStatusProjection {
    private static func number(_ value: Any?) -> Double? { (value as? NSNumber)?.doubleValue }

    private static func seconds(_ ms: Double) -> Int { max(1, Int((ms / 1000).rounded(.up))) }

    static func project(_ sender: JSONObject?, desired: Bool) -> DestinationStatus {
        guard let sender else {
            return desired ? DestinationStatus(label: "Starting", tone: .neutral)
                           : DestinationStatus(label: "Off", tone: .neutral)
        }
        let lifecycle = sender["lifecycle"] as? JSONObject
        let state = lifecycle?["state"] as? String ?? ""
        let supervisor = sender["supervisor"] as? JSONObject
        let failures = Int(number(supervisor?["consecutiveFailures"]) ?? 0)
        let nextMs = number(supervisor?["nextAttemptInMs"]) ?? 0
        let progressAgeMs = number(supervisor?["lastProgressAgeMs"]) ?? -1
        let reason = (supervisor?["reason"] as? String).flatMap { $0.isEmpty ? nil : $0 }
            ?? (sender["warning"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? ""

        // Give-up first: the supervisor's terminal and five-strike paths both set
        // it, and nothing will retry on its own until the operator does.
        if (supervisor?["gaveUp"] as? Bool) == true {
            return DestinationStatus(label: "Stopped retrying", tone: .bad,
                                     detail: reason.isEmpty ? "The destination failed and is no longer retried." : reason,
                                     showsRetry: true)
        }
        let retrying = failures > 0
            ? (nextMs > 0 ? "retrying in \(seconds(nextMs)) s" : "retrying") : ""
        switch state {
        case "producing", "live":
            guard let supervisor else {
                return DestinationStatus(label: "Unsupervised", tone: .warn,
                                         detail: "No supervisor confirms that media is reaching this destination.")
            }
            if (supervisor["healthy"] as? Bool) == true {
                return DestinationStatus(label: "Live", tone: .good, isLive: true)
            }
            return DestinationStatus(label: "Not confirmed", tone: .warn,
                                     detail: progressAgeMs >= 0
                                        ? "No media accepted for \(seconds(progressAgeMs)) s."
                                        : "Waiting for the destination to accept media.")
        case "requested", "preparing", "starting":
            if failures > 0 {
                return DestinationStatus(label: nextMs > 0 ? "Reconnecting in \(seconds(nextMs)) s" : "Reconnecting",
                                         tone: .warn, detail: reason)
            }
            return DestinationStatus(label: "Connecting", tone: .neutral)
        case "interrupted":
            var detail = progressAgeMs >= 0 ? "No media accepted for \(seconds(progressAgeMs)) s" : "Media stopped moving"
            if !retrying.isEmpty { detail += "; \(retrying)" }
            return DestinationStatus(label: "Interrupted", tone: .warn, detail: detail + ".")
        case "failed":
            let error = (lifecycle?["error"] as? String).flatMap { $0.isEmpty ? nil : $0 }
                ?? (sender["lastError"] as? String) ?? reason
            return DestinationStatus(label: "Failed", tone: .bad, detail: error)
        case "stopping", "finalizing":
            return DestinationStatus(label: "Stopping", tone: .neutral)
        case "completed":
            return DestinationStatus(label: "Stopped", tone: .neutral)
        case "idle":
            return desired ? DestinationStatus(label: "Starting", tone: .neutral)
                           : DestinationStatus(label: "Off", tone: .neutral)
        default:
            // Absent lifecycle means UNKNOWN, never healthy (contracts/README.md).
            return DestinationStatus(label: "Unknown", tone: .warn,
                                     detail: "The media core did not report this destination's lifecycle.")
        }
    }

    /// The sender row for one destination id in an `outputSenderSession` node.
    static func sender(_ id: String, in session: JSONObject?) -> JSONObject? {
        (session?["senders"] as? [JSONObject])?.first { $0["destination"] as? String == id }
    }
}

/// Health/Diagnostics wording for the stream helpers.
enum StreamHealthLine {
    static func childGuard(_ guarded: Bool?) -> String {
        switch guarded {
        case true?: return "on (ffmpeg ends with the media core)"
        case false?: return "OFF: ffmpeg can outlive a crashed media core"
        case nil: return "not reported yet"
        }
    }
}

// ── remote configuration (transport.stream.srt.configure) ─────────────────

/// A pure port of Windows `StreamDestinationControl.Configure` (SRT branch,
/// W/Services/StreamDestinationControl.cs:9-61) for the control API. Order is
/// Windows': busy, schema, mode, host shape, then ValidateSrt only when
/// enabled. No refusal text echoes an argument (the args carry a passphrase).
/// Replayed against Windows-exported vectors (contracts/control.action-vectors.json,
/// section srt-configure).
enum SrtDestinationControlPolicy {
    static let busyRefusal = "Stop streaming and wait for the transition to finish before configuring destinations."
    static let schemaRefusal = "Destination arguments do not match the action schema; see GET /manifest."
    static let modeRefusal = "SRT mode must be caller, listener, or rendezvous."
    static let hostRefusal = "SRT host must be a hostname or IP address without a URL scheme or query."

    struct Decision: Equatable {
        var config: StreamDestinationsConfig
        var passphrase: String
    }

    /// `args` are the raw control args; `current` is the saved config, which
    /// keeps every non-SRT field.
    static func configure(args: [Any?], busy: Bool, current: StreamDestinationsConfig,
                          currentPassphrase: String) -> Result<Decision, ControlBindError> {
        func refuse(_ text: String) -> Result<Decision, ControlBindError> { .failure(ControlBindError(message: text, status: 422)) }
        if busy { return refuse(busyRefusal) }
        guard case .success(let values) = ControlCatalog.bindSchema("transport.stream.srt.configure", args) else {
            return refuse(schemaRefusal)
        }
        func text(_ index: Int) -> String {
            ((index < values.count ? values[index] : nil) as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        }
        let enabled = values.first as? Bool ?? false
        var next = current
        next.srtEnabled = enabled
        next.srtMode = text(1).lowercased()
        next.srtHost = text(2)
        next.srtPort = text(3)
        next.srtLatencyMs = text(4)
        next.srtStreamId = text(5)
        next.srtKeyLength = text(6)
        // Omitted keeps the stored passphrase; an explicit "" clears it.
        let passphrase = (values.count > 7 && values[7] != nil) ? text(7) : currentPassphrase
        guard StreamDestinationPolicy.srtModes.contains(next.srtMode) else { return refuse(modeRefusal) }
        if !next.srtHost.isEmpty && !isHostName(next.srtHost.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))) {
            return refuse(hostRefusal)
        }
        if enabled, let error = StreamDestinationPolicy.validateSrt(
            mode: next.srtMode, host: next.srtHost, port: next.srtPort, latencyMs: next.srtLatencyMs,
            streamId: next.srtStreamId, keyLength: next.srtKeyLength, passphrase: passphrase) {
            return refuse(error)
        }
        return .success(Decision(config: next, passphrase: passphrase))
    }

    /// .NET `Uri.CheckHostName(name) != Unknown`: an IPv4 or IPv6 literal, or a
    /// DNS name (dot-separated labels of letters, digits, '-' and '_', each 1-63
    /// characters, optional trailing dot, at most 255 characters). Anything
    /// carrying a scheme, path, query, port or space is Unknown.
    static func isHostName(_ name: String) -> Bool {
        guard !name.isEmpty else { return false }
        var v4 = in_addr(), v6 = in6_addr()
        if inet_pton(AF_INET, name, &v4) == 1 { return true }
        // An IPv6 literal may carry a zone id ("fe80::1%en0"), which .NET accepts.
        let address = name.contains(":") ? String(name.split(separator: "%", maxSplits: 1).first ?? "") : name
        if inet_pton(AF_INET6, address, &v6) == 1 && (address == name || !name.hasSuffix("%")) { return true }
        guard name.count <= 255 else { return false }
        var labels = name.split(separator: ".", omittingEmptySubsequences: false)
        if labels.count > 1, labels.last?.isEmpty == true { labels.removeLast() }
        return labels.allSatisfy { label in
            // .NET's IRI host path accepts Unicode letters and numbers ("cam².example" is a
            // valid host on Windows: srt-configure vector host-superscript-digit).
            (1...63).contains(label.count) && label.allSatisfy { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" }
        }
    }
}
