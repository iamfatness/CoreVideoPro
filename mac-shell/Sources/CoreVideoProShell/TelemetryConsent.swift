// TelemetryConsent.swift: the opt-in consent file (Phase 4 R4, Task 7), a port of Windows
// TelemetryConsentStore (MC/Services/TelemetryConsentStore.cs). Same JSON
// ({"enabled": bool, "lastSentUtc": ISO-8601?}, nil omitted), temp-file + atomic rename,
// bounded retry (20 x 25 ms) whose sleep comes from TelemetryFileIO so tests never wait.
//
// The rules (Review Focus 3):
// - opt-in: an absent file, absent value, empty, corrupt, locked or unreadable file reads OFF;
// - revoking always lands: setEnabled(false) writes the disabled default when the file cannot
//   be read;
// - granting never guesses: setEnabled(true) throws and writes nothing when it cannot be read;
// - markSent never revokes by accident: it throws on an unreadable file rather than rewrite it.

import Foundation

struct TelemetryConsentState: Equatable {
    var enabled = false
    var lastSentUtc: Date?
}

/// A transient "someone else holds the file" error: retried, then surfaced.
struct TelemetryFileBusy: Error {}

/// The file seam. `read` returns nil when the file does not exist and throws when it cannot
/// be read (TelemetryFileBusy = retry); `write` replaces the file atomically.
struct TelemetryFileIO {
    var read: (String) throws -> Data?
    var write: (Data, String) throws -> Void
    var sleep: (TimeInterval) -> Void

    static let real = TelemetryFileIO(
        read: { path in
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory) else { return nil }
            if isDirectory.boolValue { throw CocoaError(.fileReadUnknown) }
            return try Data(contentsOf: URL(fileURLWithPath: path))
        },
        write: { data, path in
            let directory = (path as NSString).deletingLastPathComponent
            try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
            let temp = path + ".\(getpid()).tmp"
            try data.write(to: URL(fileURLWithPath: temp))
            defer { try? FileManager.default.removeItem(atPath: temp) }
            guard rename(temp, path) == 0 else { throw CocoaError(.fileWriteUnknown) }
        },
        sleep: { Thread.sleep(forTimeInterval: $0) })
}

final class TelemetryConsentStore {
    let path: String
    private let io: TelemetryFileIO
    private let attempts: Int
    private let backoff: TimeInterval
    private let lock = NSLock()

    init(path: String, fileIO: TelemetryFileIO = .real, retry: (attempts: Int, backoff: TimeInterval) = (20, 0.025)) {
        self.path = path
        io = fileIO
        attempts = max(1, retry.attempts)
        backoff = retry.backoff
    }

    /// ~/Library/Application Support/CoreVideoPro/telemetry-consent.json
    static func defaultPath() -> String {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?.path
            ?? NSHomeDirectory() + "/Library/Application Support"
        return base + "/CoreVideoPro/telemetry-consent.json"
    }

    // ── JSON (Windows camelCase, indented, nil omitted, DateTimeOffset "+00:00") ──

    struct CorruptFile: Error {}

    static func decode(_ data: Data) throws -> TelemetryConsentState {
        guard let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { throw CorruptFile() }
        var state = TelemetryConsentState()
        if let enabled = object["enabled"] {
            guard let number = enabled as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { throw CorruptFile() }
            state.enabled = number.boolValue
        }
        if let sent = object["lastSentUtc"], !(sent is NSNull) {
            guard let text = sent as? String, let date = parseDate(text) else { throw CorruptFile() }
            state.lastSentUtc = date
        }
        return state
    }

    static func encode(_ state: TelemetryConsentState) -> Data {
        var lines = ["  \"enabled\": \(state.enabled ? "true" : "false")"]
        if let sent = state.lastSentUtc { lines.append("  \"lastSentUtc\": \"\(formatDate(sent))\"") }
        return Data(("{\n" + lines.joined(separator: ",\n") + "\n}").utf8)
    }

    /// System.Text.Json's DateTimeOffset form in UTC: seconds, a trimmed fraction when present, "+00:00".
    static func formatDate(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyy-MM-dd'T'HH:mm:ss"
        var text = formatter.string(from: date)
        let fraction = date.timeIntervalSince1970 - floor(date.timeIntervalSince1970)
        let ticks = Int((fraction * 10_000_000).rounded())
        if ticks > 0 && ticks < 10_000_000 {
            var digits = String(format: "%07d", ticks)
            while digits.hasSuffix("0") { digits.removeLast() }
            text += "." + digits
        }
        return text + "+00:00"
    }

    static func parseDate(_ text: String) -> Date? {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = formatter.date(from: text) { return date }
        formatter.formatOptions = [.withInternetDateTime]
        return formatter.date(from: text)
    }

    // ── reads and writes ─────────────────────────────────────────────────────

    /// nil = no file. Retries a busy read; any other failure throws at once.
    private func readState() throws -> TelemetryConsentState? {
        var attempt = 1
        while true {
            do {
                guard let data = try io.read(path) else { return nil }
                return try Self.decode(data)
            } catch is TelemetryFileBusy where attempt < attempts {
                io.sleep(backoff)
                attempt += 1
            }
        }
    }

    private func writeState(_ state: TelemetryConsentState) throws {
        var attempt = 1
        while true {
            do { try io.write(Self.encode(state), path); return } catch is TelemetryFileBusy where attempt < attempts {
                io.sleep(backoff)
                attempt += 1
            }
        }
    }

    /// Windows Load: any failure is OFF. Consent is never inferred from a bad read.
    func load() -> TelemetryConsentState {
        lock.lock(); defer { lock.unlock() }
        return ((try? readState()) ?? nil) ?? TelemetryConsentState()
    }

    /// Windows LoadForUpdate: a corrupt file is the disabled default; an unreadable one throws.
    private func loadForUpdate() throws -> TelemetryConsentState {
        do { return try readState() ?? TelemetryConsentState() } catch is CorruptFile { return TelemetryConsentState() }
    }

    func setEnabled(_ enabled: Bool) throws {
        lock.lock(); defer { lock.unlock() }
        var state: TelemetryConsentState
        do { state = try loadForUpdate() } catch {
            // Revoking must always land: write the disabled default (losing only the watermark).
            // Granting never guesses: an unreadable file refuses and writes nothing.
            guard !enabled else { throw error }
            state = TelemetryConsentState()
        }
        state.enabled = enabled
        try writeState(state)
    }

    func markSent(_ sentUtc: Date) throws {
        lock.lock(); defer { lock.unlock() }
        var state = try loadForUpdate()
        state.lastSentUtc = sentUtc
        try writeState(state)
    }
}
