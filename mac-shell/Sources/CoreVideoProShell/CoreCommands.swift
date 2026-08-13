// Pure builders for the wire commands the shell sends to corevideo-native.
//
// These exist to be TESTED. The shell↔core contract is a hand-written JSON
// protocol with one rule that is invisible at the call site and fatal to get
// wrong: some commands are dispatched from a `payload` sub-object and some are
// read from the request root. `JsonRpcServer::handle` returns
// "<type> requires a payload." and discards the command when the wrapper is
// missing — which is exactly how connect-capture-device silently did nothing
// while the UI showed the device as connected.
//
// Keeping the shapes here (rather than inline in a Task closure) means a
// regression shows up as a failing assertion instead of a dead button.

import Foundation

enum CoreCommands {
    // ── payload-wrapped (JsonRpcServer reads request["payload"]) ─────────────

    /// `JsonRpcServer.cpp`: connect-capture-device requires a payload.
    /// `outputSourceId` must be carried too — without it the core keys frames by
    /// its own device id and the multiview lookup misses, which presented as
    /// permanently pink/placeholder tiles.
    static func connectCaptureDevice(deviceId: String,
                                     outputSourceId: String) -> JSONObject {
        ["type": "connect-capture-device",
         "payload": ["deviceId": deviceId, "outputSourceId": outputSourceId]]
    }

    static func disconnectCaptureDevice(deviceId: String) -> JSONObject {
        ["type": "disconnect-capture-device", "payload": ["deviceId": deviceId]]
    }

    /// zoom-join is payload-wrapped. The duplicated keys are deliberate and
    /// match what the shell has always sent: the core and the engine have read
    /// different spellings over time, so both are supplied. `userZak` is added
    /// ONLY when OAuth returned one — a fresh ZAK per join, never cached, never
    /// logged (it is what grants host privileges and 1080p).
    static func zoomJoin(meetingId: String, displayName: String,
                         passcode: String, webinar: Bool,
                         userZak: String? = nil) -> JSONObject {
        var payload: JSONObject = [
            "meetingId": meetingId,
            "meetingNumber": meetingId,
            "passcode": passcode,
            "password": passcode,
            "displayName": displayName,
            "webinar": webinar,
        ]
        if let userZak, !userZak.isEmpty { payload["userZak"] = userZak }
        return ["type": "zoom-join", "payload": payload]
    }

    // ── root-level (read straight off the command object) ────────────────────

    /// Rides inside a media-core-sync `commands` array; fields sit at the root
    /// of the command, NOT under a payload. Values are clamped core-side to
    /// [-10, 10] (clampColorGradeAxis) and scaled by 0.1 in the shader.
    static func setColorGrade(exposure: Double, contrast: Double,
                              saturation: Double, temperature: Double) -> JSONObject {
        ["type": "set-color-grade",
         "exposure": exposure, "contrast": contrast,
         "saturation": saturation, "temperature": temperature]
    }

    static func configureMultiviewer(layoutMode: String, tileCount: Int) -> JSONObject {
        ["type": "configure-multiviewer",
         "layoutMode": layoutMode, "tileCount": tileCount]
    }

    /// The envelope every batched command travels in.
    static func mediaCoreSync(elapsedMs: Double, commands: [JSONObject]) -> JSONObject {
        ["type": "media-core-sync", "elapsedMs": elapsedMs, "commands": commands]
    }

    // ── contract metadata, used by the tests ─────────────────────────────────

    /// Command types that MUST carry a `payload` wrapper, per the
    /// "<type> requires a payload." guards in JsonRpcServer::handle.
    ///
    /// Hand-maintained lists of a contract owned by another language drift, so
    /// the test does not trust this one: it re-derives the set from
    /// native/src/rpc/JsonRpcServer.cpp and fails if the two disagree. That way
    /// a payload guard added on the C++ side surfaces here instead of as a
    /// silently discarded command.
    static let payloadWrappedTypes: Set<String> = [
        "browser-add",
        "browser-reload",
        "browser-remove",
        "connect-capture-device",
        "disconnect-capture-device",
        "register-capture-shm",
        "select-capture-input",
        "set-capture-audio-sync-offset",
        "unregister-capture-shm",
        "zoom-join",
    ]

    /// Walks up from the running binary looking for the core's RPC server
    /// source. Returns nil in a packaged app (no repo alongside it), which the
    /// test reports as SKIPPED rather than passing silently.
    static func locateJsonRpcServerSource() -> String? {
        var dir = URL(fileURLWithPath: CommandLine.arguments.first ?? ".")
            .deletingLastPathComponent()
        for _ in 0..<8 {
            let candidate = dir.appendingPathComponent("native/src/rpc/JsonRpcServer.cpp")
            if FileManager.default.fileExists(atPath: candidate.path) {
                return candidate.path
            }
            dir = dir.deletingLastPathComponent()
        }
        return nil
    }

    /// The payload-requiring command types as the CORE actually declares them.
    static func payloadWrappedTypesFromCoreSource() -> Set<String>? {
        guard let path = locateJsonRpcServerSource(),
              let source = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        var found: Set<String> = []
        // Matches: "<type> requires a payload."
        for line in source.split(separator: "\n") {
            guard let range = line.range(of: " requires a payload") else { continue }
            let head = line[line.startIndex..<range.lowerBound]
            guard let quote = head.lastIndex(of: "\"") else { continue }
            let name = head[head.index(after: quote)...]
            if !name.isEmpty { found.insert(String(name)) }
        }
        return found.isEmpty ? nil : found
    }
}
