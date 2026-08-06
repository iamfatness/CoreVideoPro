// Diagnostics.swift — the macOS DIAGNOSE surface plus the redacted SUPPORT
// BUNDLE export (the mac counterpart of the WinUI DiagnosticsWindow and
// SupportBundleBuilder / SupportBundleArchiveBuilder / SupportBundleLogRedactor).
//
// TWO HOUSE LAWS ARE ENCODED HERE AND MUST NOT BE RELAXED:
//
//  1. NEVER CLAIM HEALTH YOU DID NOT MEASURE. Every row renders a value that is
//     actually present in the core snapshot; an absent field renders "—" in the
//     dim token, never a green tick. `DiagRow` enforces this structurally — a
//     nil value ignores the caller's tone, so no future edit can accidentally
//     paint an unmeasured field healthy.
//
//  2. REDACTION IS THE POINT. A support bundle must never carry a stream key, a
//     Zoom OAuth token, a `userZak`, an `sdkJwt` or a passphrase. Assume it gets
//     emailed to a stranger. Filtering runs on THREE independent layers (key
//     name, value pattern, known-literal identity) and the writer then re-reads
//     every byte it produced and re-checks it. File PATHS are not secrets and
//     are emitted verbatim — they carry most of the diagnostic value.
//
// Local export only. There is deliberately no uploader and no telemetry here:
// nothing in this file sends a byte off the machine.

import AppKit
import Foundation
import SwiftUI

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Redaction
// ─────────────────────────────────────────────────────────────────────────────

enum Redactor {
    static let placeholder = "[redacted]"

    // Layer 1 — KEY NAMES. Mirrors the Windows SupportBundleLogRedactor list
    // (key/token/passphrase/password/secret/pwd/zak/jwt) and reuses its
    // ENDS-WITH rule, which is exactly what keeps the diagnostic fields alive:
    // `keyPhase`, `keyPosition`, `keyer`, `keyframeIntervalSeconds`,
    // `programPixelSignature` and `renderPlanSignature` all survive, while
    // `streamKey`, `userZak`, `sdkJwt`, `refresh_token` and a bare `key` do not.
    private static let deniedKeySuffixes = [
        "key", "token", "passphrase", "password", "passcode", "secret",
        "pwd", "zak", "jwt", "credential", "credentials",
    ]

    // Substrings for names the ends-with rule cannot reach.
    private static let deniedKeySubstrings = [
        "streamkey", "apikey", "authorization", "bearer", "clientsecret",
        "cookie", "privatekey",
    ]

    static func isSecretKey(_ key: String) -> Bool {
        let normalized = key.lowercased()
            .replacingOccurrences(of: "_", with: "")
            .replacingOccurrences(of: "-", with: "")
        if deniedKeySubstrings.contains(where: normalized.contains) { return true }
        return deniedKeySuffixes.contains { normalized.hasSuffix($0) }
    }

    // Layer 2 — VALUE PATTERNS. Applied to every string that leaves this
    // process, including raw log lines where there is no key structure at all.
    private struct Rule {
        let regex: NSRegularExpression
        let template: String

        init(_ pattern: String, _ template: String,
             _ options: NSRegularExpression.Options = [.caseInsensitive]) {
            // A malformed literal here is a programming error; crashing at first
            // use beats silently shipping an unfiltered bundle.
            // swiftlint:disable:next force_try
            regex = try! NSRegularExpression(pattern: pattern, options: options)
            self.template = template
        }
    }

    private static let rules: [Rule] = [
        // URL userinfo — rtmp://user:password@host/…
        Rule("(://)[^/\\s:@\"']+:[^/\\s@\"']+@", "$1" + placeholder + "@"),
        // Any rtmp/rtmps/srt URL: scheme + host survive (they name the service,
        // which IS diagnostic); everything after the authority is treated as a
        // stream key. Same rule as the Windows RtmpUrlPath.
        Rule("((?:rtmps?|srt)://[^/\\s\"']+/)[^\\s\"',}\\]]+", "$1" + placeholder),
        // Secret query parameters on any URL.
        Rule("([?&][A-Za-z0-9_-]*(?:key|token|passphrase|password|passcode|secret|pwd|zak|jwt|sig|auth)=)[^&\\s\"'<>]+",
             "$1" + placeholder),
        // "someKey": "value" in JSON-ish text.
        Rule("\"([A-Za-z0-9_-]*(?:key|token|passphrase|password|passcode|secret|pwd|zak|jwt))\"\\s*:\\s*\"[^\"]*\"",
             "\"$1\":\"" + placeholder + "\""),
        // someKey=value / someKey: value in log lines.
        Rule("([A-Za-z0-9_-]*(?:key|token|passphrase|password|passcode|secret|pwd|zak|jwt)\\s*[:=]\\s*)[^&\\s\"',;}\\]]+",
             "$1" + placeholder),
        // Authorization headers.
        Rule("\\bBearer\\s+[A-Za-z0-9\\-_.=+/]+", "Bearer " + placeholder, []),
        // JWT-shaped blobs (Zoom ZAKs and sdkJwts arrive in this shape).
        Rule("\\beyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}(?:\\.[A-Za-z0-9_-]*)?",
             placeholder, []),
        // Catch-all for opaque high-entropy tokens: a >=28 char run of
        // [A-Za-z0-9_-] carrying lower AND upper AND digits. The lookbehind
        // excludes runs preceded by / or . so FILE PATHS are never touched, and
        // the mixed-case requirement spares UUIDs, lowercase ids and
        // SCREAMING_SNAKE env-var names.
        Rule("(?<![A-Za-z0-9_\\-/.])(?=[A-Za-z0-9_-]*[a-z])(?=[A-Za-z0-9_-]*[A-Z])(?=[A-Za-z0-9_-]*[0-9])[A-Za-z0-9_-]{28,}",
             placeholder, []),
    ]

    /// Scrubs one string: known literals first (layer 3), then value patterns.
    static func scrub(_ text: String, literals: [String] = []) -> String {
        var result = text
        // Layer 3 — KNOWN LITERALS. The shell holds the operator's live stream
        // key and meeting passcode in memory; an identity match cannot be fooled
        // by a shape the pattern rules did not anticipate.
        for literal in literals where literal.count >= 4 {
            result = result.replacingOccurrences(of: literal, with: placeholder)
        }
        guard !result.isEmpty else { return result }
        for rule in rules {
            let range = NSRange(result.startIndex..., in: result)
            result = rule.regex.stringByReplacingMatches(
                in: result, options: [], range: range, withTemplate: rule.template)
        }
        return result
    }

    /// Walks a decoded JSON tree, blanking secret-named keys and scrubbing every
    /// surviving string. The KEY is kept (so a reader can see the field existed)
    /// — only the value is replaced.
    static func redact(_ value: Any, literals: [String] = []) -> Any {
        if let object = value as? [String: Any] {
            var result: [String: Any] = [:]
            for (key, child) in object {
                if isSecretKey(key) {
                    // Preserve presence/absence: "was a key configured at all"
                    // is the question the operator actually needs answered.
                    if let text = child as? String {
                        result[key] = text.isEmpty ? "absent" : "present-" + placeholder
                    } else {
                        result[key] = placeholder
                    }
                    continue
                }
                result[key] = redact(child, literals: literals)
            }
            return result
        }
        if let array = value as? [Any] {
            return array.map { redact($0, literals: literals) }
        }
        if let text = value as? String {
            return scrub(text, literals: literals)
        }
        return value
    }

    static func redactLines(_ text: String, literals: [String] = []) -> String {
        text.split(separator: "\n", omittingEmptySubsequences: false)
            .map { scrub(String($0), literals: literals) }
            .joined(separator: "\n")
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Log tails
// ─────────────────────────────────────────────────────────────────────────────

enum LogTail {
    struct Tail {
        var text: String
        var totalBytes: Int
        var truncated: Bool
    }

    /// Reads the last `maxBytes` of a file. A mid-file cut drops the partial
    /// first line so a tail always starts on a whole record.
    static func read(path: String, maxBytes: Int) -> Tail? {
        guard let handle = FileHandle(forReadingAtPath: path) else { return nil }
        defer { try? handle.close() }
        guard let end = try? handle.seekToEnd() else { return nil }
        let total = Int(end)
        let truncated = total > maxBytes
        let offset = truncated ? UInt64(total - maxBytes) : 0
        try? handle.seek(toOffset: offset)
        let data = (try? handle.readToEnd()) ?? Data()
        var text = String(decoding: data, as: UTF8.self)
        if truncated, let newline = text.firstIndex(of: "\n") {
            text = String(text[text.index(after: newline)...])
        }
        return Tail(text: text, totalBytes: total, truncated: truncated)
    }

    static func byteCount(path: String) -> Int? {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return (attributes?[.size] as? NSNumber)?.intValue
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Snapshot accessor
// ─────────────────────────────────────────────────────────────────────────────

/// Dotted-path reader over the decoded core snapshot. Every accessor returns an
/// Optional so an ABSENT field stays distinguishable from a zero/false one —
/// that distinction is the whole "never claim health you did not measure" rule.
struct Snap {
    let root: JSONObject

    init(_ root: JSONObject) { self.root = root }

    private func value(_ path: String) -> Any? {
        var current: Any = root
        for part in path.split(separator: ".") {
            guard let dictionary = current as? JSONObject,
                  let next = dictionary[String(part)] else { return nil }
            current = next
        }
        return current is NSNull ? nil : current
    }

    var isEmpty: Bool { root.isEmpty }

    func object(_ path: String) -> JSONObject? { value(path) as? JSONObject }

    func objects(_ path: String) -> [JSONObject] {
        (value(path) as? [Any])?.compactMap { $0 as? JSONObject } ?? []
    }

    func strings(_ path: String) -> [String] {
        (value(path) as? [Any])?.compactMap { $0 as? String }.filter { !$0.isEmpty } ?? []
    }

    func string(_ path: String) -> String? {
        guard let raw = value(path) else { return nil }
        if let text = raw as? String { return text.isEmpty ? nil : text }
        if let number = raw as? NSNumber { return number.stringValue }
        return nil
    }

    func bool(_ path: String) -> Bool? { (value(path) as? NSNumber)?.boolValue }
    func number(_ path: String) -> Double? { (value(path) as? NSNumber)?.doubleValue }
    func int(_ path: String) -> Int? { (value(path) as? NSNumber)?.intValue }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Formatting
// ─────────────────────────────────────────────────────────────────────────────

enum DiagFormat {
    static func count(_ value: Int?) -> String? {
        guard let value else { return nil }
        return NumberFormatter.localizedString(from: NSNumber(value: value), number: .decimal)
    }

    static func count(_ value: Double?) -> String? {
        guard let value else { return nil }
        return count(Int(value))
    }

    static func bytes(_ value: Double?) -> String? {
        guard let value else { return nil }
        return ByteCountFormatter.string(fromByteCount: Int64(value), countStyle: .file)
    }

    static func millis(_ value: Double?) -> String? {
        guard let value else { return nil }
        if value < 1000 { return String(format: "%.0f ms", value) }
        let seconds = value / 1000
        if seconds < 60 { return String(format: "%.1f s", seconds) }
        return String(format: "%d:%02d", Int(seconds) / 60, Int(seconds) % 60)
    }

    static func decimal(_ value: Double?, _ places: Int, suffix: String = "") -> String? {
        guard let value else { return nil }
        return String(format: "%.\(places)f%@", value, suffix)
    }

    static func yesNo(_ value: Bool?) -> String? {
        guard let value else { return nil }
        return value ? "yes" : "no"
    }

    static func fileSize(_ path: String) -> String {
        guard let bytes = LogTail.byteCount(path: path) else { return "missing" }
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }

    static let stamp: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter
    }()

    static let iso: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss'Z'"
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter
    }()
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Support bundle
// ─────────────────────────────────────────────────────────────────────────────

/// Everything the writer needs, pre-serialized so the export can run off the
/// main actor without carrying live model dictionaries across the hop.
struct SupportBundleInput {
    var createdAt = Date()
    var coreSnapshotJson = Data()
    var zoomSnapshotJson = Data()
    var shellStateJson = Data()
    var warnings: [String] = []
    /// Live secret VALUES held by the shell. These are never written; they are
    /// used to scrub by identity and then to VERIFY no copy survived.
    var knownSecretLiterals: [String] = []
    var shellLogPath = ShellLog.path
    var coreLogPath = CoreLog.path
    var crashReportDirectory = NSHomeDirectory() + "/Library/Logs/DiagnosticReports"
    var destinationRoot = SupportBundle.defaultRoot
    var tailByteLimit = 2 * 1024 * 1024
}

struct SupportBundleResult {
    var folder: URL
    var archive: URL?
    var includedFiles: [String] = []
    var notes: [String] = []
    /// Verification counter. MUST be zero on a healthy export; a non-zero value
    /// means a known secret slipped past layers 1 and 2 and was only caught by
    /// the final identity pass — a bug worth reporting, not a normal state.
    var literalHits = 0

    var revealTarget: URL { archive ?? folder }
}

enum SupportBundleError: LocalizedError {
    case cannotCreateFolder(String)

    var errorDescription: String? {
        switch self {
        case .cannotCreateFolder(let why):
            return "could not create the bundle folder: \(why)"
        }
    }
}

enum SupportBundle {
    static let defaultRoot = URL(fileURLWithPath: NSHomeDirectory())
        .appendingPathComponent("Library/Application Support/CoreVideoPro/support-bundles",
                                isDirectory: true)

    /// Writes a redacted bundle folder (and, when `ditto` succeeds, a zip beside
    /// it). A missing log is never fatal — it becomes a manifest skip note,
    /// exactly like the Windows builder.
    static func write(_ input: SupportBundleInput) throws -> SupportBundleResult {
        let literals = input.knownSecretLiterals.filter { $0.count >= 4 }
        let name = "corevideo-support-" + DiagFormat.stamp.string(from: input.createdAt)
        let folder = input.destinationRoot.appendingPathComponent(name, isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        } catch {
            throw SupportBundleError.cannotCreateFolder(error.localizedDescription)
        }

        var result = SupportBundleResult(folder: folder)
        var notes: [String] = []

        func writeText(_ text: String, to file: String) {
            do {
                try Data(text.utf8).write(to: folder.appendingPathComponent(file))
                result.includedFiles.append(file)
            } catch {
                notes.append("[skipped] \(file): \(error.localizedDescription)")
            }
        }

        func writeRedactedJson(_ data: Data, to file: String, label: String) {
            guard !data.isEmpty,
                  let decoded = try? JSONSerialization.jsonObject(with: data) else {
                notes.append("[skipped] \(file): no \(label) captured this session")
                return
            }
            let redacted = Redactor.redact(decoded, literals: literals)
            guard let encoded = try? JSONSerialization.data(
                withJSONObject: redacted,
                options: [.prettyPrinted, .sortedKeys, .fragmentsAllowed]) else {
                notes.append("[skipped] \(file): could not re-encode the redacted \(label)")
                return
            }
            do {
                try encoded.write(to: folder.appendingPathComponent(file))
                result.includedFiles.append(file)
            } catch {
                notes.append("[skipped] \(file): \(error.localizedDescription)")
            }
        }

        writeRedactedJson(input.coreSnapshotJson, to: "core-snapshot.json", label: "core snapshot")
        writeRedactedJson(input.zoomSnapshotJson, to: "zoom-snapshot.json", label: "zoom snapshot")
        writeRedactedJson(input.shellStateJson, to: "shell-state.json", label: "shell state")

        if input.warnings.isEmpty {
            writeText("No warnings recorded this session.\n", to: "warnings.txt")
        } else {
            writeText(Redactor.redactLines(input.warnings.joined(separator: "\n"),
                                           literals: literals) + "\n",
                      to: "warnings.txt")
        }

        func writeLogTail(path: String, to file: String) {
            guard let tail = LogTail.read(path: path, maxBytes: input.tailByteLimit) else {
                notes.append("[skipped] \(file): not found at \(path)")
                return
            }
            let header = "# \(path)\n# \(tail.totalBytes) bytes on disk"
                + (tail.truncated ? " (tail of the last \(input.tailByteLimit) bytes)" : "")
                + "\n\n"
            writeText(header + Redactor.redactLines(tail.text, literals: literals), to: file)
        }

        writeLogTail(path: input.shellLogPath, to: "shell-log-tail.txt")
        writeLogTail(path: input.coreLogPath, to: "core-log-tail.txt")

        // Crash-report LISTING only (the Windows dumps.txt shape): names, sizes
        // and dates are triage; the reports themselves are never copied.
        writeText(crashReportListing(directory: input.crashReportDirectory),
                  to: "crash-reports.txt")

        // ── verification pass ────────────────────────────────────────────────
        // Re-read every byte just written and prove no known secret survived.
        // Belt and braces over layers 1 and 2: an identity match cannot be
        // fooled by a value shape the pattern rules did not anticipate.
        var verifiedCount = 0
        for file in result.includedFiles {
            let url = folder.appendingPathComponent(file)
            guard var text = try? String(contentsOf: url, encoding: .utf8) else { continue }
            verifiedCount += 1
            var hits = 0
            for literal in literals where text.contains(literal) {
                hits += text.components(separatedBy: literal).count - 1
                text = text.replacingOccurrences(of: literal, with: Redactor.placeholder)
            }
            if hits > 0 {
                result.literalHits += hits
                try? Data(text.utf8).write(to: url)
                notes.append("[verify] \(file): \(hits) known-secret occurrence(s) survived "
                             + "the filters and were scrubbed by the final pass")
            }
        }
        result.notes = notes

        // Manifest LAST so it can report the verification outcome.
        writeText(manifest(input: input, result: result, verified: verifiedCount),
                  to: "manifest.txt")

        result.archive = makeArchive(folder: folder)
        return result
    }

    private static func crashReportListing(directory: String) -> String {
        let manager = FileManager.default
        guard let names = try? manager.contentsOfDirectory(atPath: directory) else {
            return "No crash-report directory at \(directory).\n"
        }
        let matches = names.filter { $0.lowercased().contains("corevideo") }
        guard !matches.isEmpty else {
            return "\(directory)\nNo CoreVideo Pro crash reports found.\n"
        }
        var lines = [
            directory,
            "Listing only — crash-report CONTENTS are never copied into a bundle.",
            "",
        ]
        for name in matches.sorted() {
            let attributes = try? manager.attributesOfItem(atPath: directory + "/" + name)
            let size = (attributes?[.size] as? NSNumber)?.intValue ?? 0
            let modified = (attributes?[.modificationDate] as? Date)
                ?? Date(timeIntervalSince1970: 0)
            lines.append("\(DiagFormat.iso.string(from: modified))  \(size)  \(name)")
        }
        return lines.joined(separator: "\n") + "\n"
    }

    private static func manifest(input: SupportBundleInput,
                                 result: SupportBundleResult,
                                 verified: Int) -> String {
        let paths = ShellPaths.resolve()
        var lines: [String] = []
        lines.append("CoreVideo Pro support bundle (macOS shell)")
        lines.append("created: \(DiagFormat.iso.string(from: input.createdAt))")
        lines.append("app version: "
                     + ((Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String)
                        ?? "dev"))
        lines.append("macOS: \(ProcessInfo.processInfo.operatingSystemVersionString)")
#if arch(arm64)
        lines.append("architecture: arm64")
#else
        lines.append("architecture: not-arm64")
#endif
        lines.append("cpu cores: \(ProcessInfo.processInfo.processorCount)")
        lines.append("physical memory: "
                     + ByteCountFormatter.string(
                        fromByteCount: Int64(ProcessInfo.processInfo.physicalMemory),
                        countStyle: .memory))
        lines.append("core binary: \(paths.corePath ?? "not found")")
        lines.append("engine binary: \(paths.engineBinaryPath ?? "not found")")
        lines.append("log tail limit: \(input.tailByteLimit) bytes per log")
        lines.append("")
        lines.append("REDACTION")
        lines.append("  This bundle is filtered on three independent layers before it is")
        lines.append("  written. (1) KEY NAME: any field whose name ends in key / token /")
        lines.append("  passphrase / password / passcode / secret / pwd / zak / jwt (or")
        lines.append("  contains streamkey / apikey / authorization / bearer / cookie) has")
        lines.append("  its VALUE replaced. (2) VALUE PATTERN: URL userinfo, rtmp+srt URL")
        lines.append("  paths, secret query parameters, Bearer headers, JWT-shaped blobs and")
        lines.append("  opaque >=28 character mixed-case tokens are replaced everywhere,")
        lines.append("  including raw log lines. (3) KNOWN LITERAL: the shell's live stream")
        lines.append("  key and meeting passcode are matched by identity. Every written file")
        lines.append("  is then RE-READ and re-checked against layer 3.")
        lines.append("  Verification: \(verified) file(s) re-read, "
                     + "\(result.literalHits) known-secret occurrence(s) found.")
        lines.append("  FILE PATHS ARE NOT SECRETS and are included verbatim — they carry")
        lines.append("  most of the diagnostic value. Crash reports are listed, never copied.")
        lines.append("  Nothing in this export is transmitted anywhere; it is a local write.")
        lines.append("")
        lines.append("CONTENTS")
        lines.append("  [included] manifest.txt (this file)")
        for file in result.includedFiles.sorted() {
            let size = LogTail.byteCount(path: result.folder.appendingPathComponent(file).path)
            lines.append("  [included] \(file) (\(size ?? 0) bytes)")
        }
        for note in result.notes {
            lines.append("  " + note)
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// `ditto -c -k --keepParent` is the macOS zip that keeps the folder as the
    /// archive root. A failure is NOT fatal — the folder is already a complete
    /// bundle, and that is what gets reported to the operator.
    private static func makeArchive(folder: URL) -> URL? {
        let archive = folder.appendingPathExtension("zip")
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        process.arguments = ["-c", "-k", "--sequesterRsrc", "--keepParent",
                             folder.path, archive.path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0 ? archive : nil
        } catch {
            return nil
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Self-check
// ─────────────────────────────────────────────────────────────────────────────

/// A test-style self-check, runnable without a test target:
///
///     COREVIDEO_SHELL_SELFCHECK=1 .build/release/CoreVideoProShell
///
/// It seeds a snapshot, a shell state and two log files with every secret shape
/// the product actually handles, exports a REAL bundle into a temp directory,
/// then greps every produced file for those secrets. It also asserts the benign
/// load-bearing values (file paths, keyPhase, keyframeIntervalSeconds, render
/// signatures) survive untouched — an over-eager filter is a bug too.
enum DiagnosticsSelfCheck {
    private static let streamKey = "abcd-1234-SECRET-stream-key"
    private static let zak = "eyJhbGciOiJIUzI1NiJ9.eyJ1c2VySWQiOiJTRUNSRVQtWkFLIn0.k9dQsecretSIG"
    private static let sdkJwt = "eyJ0eXAiOiJKV1QifQ.eyJhcHBLZXkiOiJTRUNSRVQifQ.abcSECRETsig"
    private static let passphrase = "srt-passphrase-SECRET"
    private static let passcode = "Zoom-Passcode-SECRET-9911"
    private static let accessToken = "AccessTokenSECRET0123456789abcdefGHIJ"
    private static let benignPath =
        "/Users/operator/Movies/CoreVideoPro/show-20260806/Program.mp4"

    // swiftlint:disable:next function_body_length
    static func run() -> [String] {
        var failures: [String] = []

        let snapshot: JSONObject = [
            "health": ["status": "live", "renderer": "metal", "programFrameHealth": "good"],
            "recording": [
                "programPath": benignPath,
                "streams": [["kind": "program", "path": benignPath]],
            ],
            "outputSenderSession": [
                "senders": [[
                    "destination": "rtmp://a.rtmp.youtube.com/live2/\(streamKey)",
                    "status": "live",
                    "keyframeIntervalSeconds": 2,
                ]],
            ],
            "programFrame": [
                "programPixelSignature": 4_051_200,
                "renderPlanSignature": 991_817,
                "keyPhase": "on-air",
            ],
            "destinationSettings": ["streamKey": streamKey, "passphrase": passphrase],
            "zoomJoin": [
                "userZak": zak, "sdkJwt": sdkJwt, "passcode": passcode,
                "meetingId": "88812345678",
            ],
            "oauth": ["access_token": accessToken, "refresh_token": accessToken],
        ]
        let shellState: JSONObject = [
            "streamUrl": "rtmp://a.rtmp.youtube.com/live2",
            "note": "Authorization: Bearer \(accessToken)",
            "logPath": ShellLog.path,
        ]

        let temp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("corevideo-selfcheck-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: temp, withIntermediateDirectories: true)
        let shellLog = temp.appendingPathComponent("seed-shell.log")
        let coreLog = temp.appendingPathComponent("seed-core.log")
        let seededShellLog = """
            2026-08-06 joining meeting 88812345678 passcode=\(passcode)
            2026-08-06 stream target rtmp://a.rtmp.youtube.com/live2/\(streamKey)
            2026-08-06 payload {"userZak":"\(zak)","sdkJwt":"\(sdkJwt)"}
            2026-08-06 Authorization: Bearer \(accessToken)
            2026-08-06 artifact \(benignPath)
            """
        let seededCoreLog = """
            [recording] wrote \(benignPath) frames=1801 keyframeIntervalSeconds=2
            [sender] srt://ingest.example:9000?passphrase=\(passphrase) status=live
            [lock] audio.gather over budget 2100us site=audio.gather
            [oauth] refresh_token=\(accessToken) rotated
            """
        try? seededShellLog.write(to: shellLog, atomically: true, encoding: .utf8)
        try? seededCoreLog.write(to: coreLog, atomically: true, encoding: .utf8)

        var input = SupportBundleInput()
        input.coreSnapshotJson = (try? JSONSerialization.data(withJSONObject: snapshot)) ?? Data()
        input.shellStateJson = (try? JSONSerialization.data(withJSONObject: shellState)) ?? Data()
        input.warnings = ["stream: failed with key \(streamKey)"]
        input.knownSecretLiterals = [streamKey, passcode]
        input.shellLogPath = shellLog.path
        input.coreLogPath = coreLog.path
        input.crashReportDirectory = temp.path
        input.destinationRoot = temp

        let secrets = [
            ("streamKey", streamKey), ("userZak", zak), ("sdkJwt", sdkJwt),
            ("passphrase", passphrase), ("passcode", passcode), ("oauth token", accessToken),
        ]
        do {
            let bundle = try SupportBundle.write(input)
            let files = (try? FileManager.default.contentsOfDirectory(
                atPath: bundle.folder.path)) ?? []
            if files.isEmpty { failures.append("bundle folder is empty") }
            var sawBenignPath = false
            for file in files {
                let url = bundle.folder.appendingPathComponent(file)
                guard let text = try? String(contentsOf: url, encoding: .utf8) else { continue }
                for (name, secret) in secrets where text.contains(secret) {
                    failures.append("LEAK: \(name) found verbatim in \(file)")
                }
                if text.contains(benignPath) { sawBenignPath = true }
            }
            if !sawBenignPath {
                failures.append("over-redaction: the benign artifact PATH did not survive "
                                + "(paths are not secrets)")
            }
            print("self-check: bundle folder \(bundle.folder.path)")
            print("self-check: files \(files.sorted().joined(separator: ", "))")
            print("self-check: archive \(bundle.archive?.lastPathComponent ?? "none")")
        } catch {
            failures.append("bundle export threw: \(error.localizedDescription)")
        }

        // Load-bearing benign values must survive the scrubber untouched.
        let benignCases = [
            "keyframeIntervalSeconds=2",
            "keyPhase: on-air",
            "renderPlanSignature=991817",
            "artifact /Users/operator/Movies/CoreVideoPro/show/Program.mp4",
            "renderer=metal fps=60 dropped=0 layerCount=3",
        ]
        for benign in benignCases where Redactor.scrub(benign) != benign {
            failures.append("over-redaction: \"\(benign)\" became \"\(Redactor.scrub(benign))\"")
        }

        // Key-name classifier, both directions.
        for key in ["streamKey", "userZak", "sdkJwt", "passphrase", "passcode",
                    "refresh_token", "access_token", "apiKey", "key", "Authorization"]
        where !Redactor.isSecretKey(key) {
            failures.append("key classifier missed \(key)")
        }
        for key in ["keyPhase", "keyPosition", "keyer", "keyframeIntervalSeconds",
                    "programPixelSignature", "renderPlanSignature", "monitorStatus"]
        where Redactor.isSecretKey(key) {
            failures.append("key classifier over-matched \(key)")
        }

        try? FileManager.default.removeItem(at: temp)
        return failures
    }

    /// Headless export of the CURRENT on-disk logs, for the case the UI itself
    /// is the thing that is broken (a Finder launch has no console, so an
    /// operator who cannot reach the Diagnose tab has no other way to produce a
    /// bundle). There is no live core here, so the snapshot files are skip
    /// notes — the log tails are the payload.
    ///     COREVIDEO_SHELL_EXPORT_BUNDLE=1 .build/release/CoreVideoProShell
    ///     COREVIDEO_SHELL_EXPORT_BUNDLE=/some/dir  (destination override)
    static func exportHeadlessAndExit(destination: String) -> Never {
        var input = SupportBundleInput()
        if destination != "1", !destination.isEmpty {
            input.destinationRoot = URL(fileURLWithPath: destination, isDirectory: true)
        }
        do {
            let bundle = try SupportBundle.write(input)
            print("support bundle: \(bundle.revealTarget.path)")
            for file in bundle.includedFiles.sorted() { print("  included \(file)") }
            for note in bundle.notes { print("  \(note)") }
            exit(0)
        } catch {
            print("support bundle FAILED: \(error.localizedDescription)")
            exit(1)
        }
    }

    /// Entry point for the env-var harness run.
    static func runAndExit() -> Never {
        let failures = run()
        if failures.isEmpty {
            print("self-check: PASS — no seeded secret survived the bundle export")
            exit(0)
        }
        for failure in failures {
            print("self-check: FAIL — \(failure)")
        }
        exit(1)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - View helpers
// ─────────────────────────────────────────────────────────────────────────────

enum DiagTone {
    case neutral, good, warn, bad

    var color: Color {
        switch self {
        case .neutral: return Studio.textPrimary
        case .good: return Studio.accent
        case .warn: return Studio.amber
        case .bad: return Studio.red
        }
    }

    /// Maps the core's status vocabulary. An UNRECOGNISED status stays neutral:
    /// it is reported verbatim, never coloured green on a guess.
    static func forStatus(_ status: String?) -> DiagTone {
        switch status?.lowercased() {
        case "live", "encoding", "recording", "connected", "ready", "good", "healthy",
             "ok", "attached", "in_meeting", "in-meeting", "streaming", "active":
            return .good
        case "warning", "degraded", "recovering", "low", "launching", "connecting",
             "starting", "detected", "stubbed", "synthetic":
            return .warn
        case "error", "failed", "blocked", "missing", "stalled", "disconnected":
            return .bad
        default:
            return .neutral
        }
    }
}

struct DiagItem {
    var label: String
    var value: String?
    var tone: DiagTone = .neutral
    var wrap = false
}

struct DiagRow: View {
    let label: String
    let value: String?
    var tone: DiagTone = .neutral
    var wrap = false

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(label)
                .font(.plexMono(9))
                .foregroundStyle(Studio.textDim)
                .frame(width: 132, alignment: .leading)
            // An ABSENT value is dim "—" whatever tone the caller asked for, so
            // the pane can never render a green tick for something unmeasured.
            if wrap {
                Text(value ?? "—")
                    .font(.plexMono(10))
                    .foregroundStyle(value == nil ? Studio.textDim : tone.color)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            } else {
                Text(value ?? "—")
                    .font(.plexMono(10))
                    .foregroundStyle(value == nil ? Studio.textDim : tone.color)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .textSelection(.enabled)
            }
            Spacer(minLength: 0)
        }
    }
}

struct DiagRows: View {
    let items: [DiagItem]

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                DiagRow(label: item.label, value: item.value,
                        tone: item.tone, wrap: item.wrap)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct DiagSection<Content: View>: View {
    let title: String
    var subtitle: String?
    @ViewBuilder var content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            MonoLabel(title)
            if let subtitle {
                Text(subtitle)
                    .font(.grotesk(10))
                    .foregroundStyle(Studio.textDim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(StudioPanel())
    }
}

private struct DiagNote: View {
    let text: String
    var tone: DiagTone = .warn

    var body: some View {
        Text(text)
            .font(.plexMono(10))
            .foregroundStyle(tone.color)
            .textSelection(.enabled)
            .fixedSize(horizontal: false, vertical: true)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - The pane
// ─────────────────────────────────────────────────────────────────────────────

struct DiagnosePane: View {
    @EnvironmentObject var model: AppModel

    @State private var exportStatus = ""
    @State private var exportTone: DiagTone = .neutral
    @State private var exportedURL: URL?
    @State private var exporting = false
    @State private var perfLines: [String] = []
    @State private var perfScanned = false

    private var snap: Snap { Snap(model.lastSnapshot) }
    private var zoom: Snap { Snap(model.lastZoomSnapshot) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Diagnose").font(.grotesk(14, .semibold))
            if snap.isEmpty {
                DiagNote(text: "No core snapshot received yet — every reading below stays "
                         + "blank until the media core answers a sync.")
            }
            DiagSection(title: "Core") { DiagRows(items: coreItems) }
            DiagSection(title: "Program frame") { DiagRows(items: programItems) }
            DiagSection(title: "Encoder") { DiagRows(items: encoderItems) }
            recordingSection
            DiagSection(title: "Streaming") { DiagRows(items: streamingItems) }
            DiagSection(title: "Zoom / engine") { DiagRows(items: zoomItems) }
            DiagSection(title: "Capture devices") { DiagRows(items: captureItems) }
            DiagSection(title: "Audio") { DiagRows(items: audioItems) }
            perfSection
            logsSection
            warningsSection
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // ── core ─────────────────────────────────────────────────────────────────

    private var bridgeStateText: String {
        switch model.status {
        case .connected(let renderer): return "connected (\(renderer))"
        case .launching: return "launching…"
        case .exited(let code): return "exited (code \(code)) — relaunching"
        case .failed(let why): return "failed — \(why)"
        }
    }

    private var bridgeTone: DiagTone {
        switch model.status {
        case .connected: return .good
        case .launching: return .warn
        case .exited, .failed: return .bad
        }
    }

    private var coreItems: [DiagItem] {
        let paths = ShellPaths.resolve()
        var items: [DiagItem] = [
            DiagItem(label: "bridge", value: bridgeStateText, tone: bridgeTone),
            DiagItem(label: "last sync error",
                     value: model.statusDetail.isEmpty ? nil : model.statusDetail,
                     tone: .bad, wrap: true),
            DiagItem(label: "core status", value: snap.string("health.status"),
                     tone: DiagTone.forStatus(snap.string("health.status"))),
            DiagItem(label: "renderer", value: snap.string("health.renderer")),
            DiagItem(label: "profile", value: snap.string("profile.name")),
            DiagItem(label: "max program", value: snap.string("profile.maxProgramResolution")
                .map { $0 + " @ " + (snap.string("profile.maxProgramFps") ?? "?") + "fps" }),
            DiagItem(label: "capabilities",
                     value: DiagFormat.count(snap.strings("profile.capabilities").count)
                        .map { $0 + " advertised" }),
            DiagItem(label: "core binary", value: paths.corePath ?? "NOT FOUND",
                     tone: paths.corePath == nil ? .bad : .neutral, wrap: true),
            DiagItem(label: "engine binary", value: paths.engineBinaryPath ?? "NOT FOUND",
                     tone: paths.engineBinaryPath == nil ? .bad : .neutral, wrap: true),
        ]
        for message in snap.strings("health.messages") {
            items.append(DiagItem(label: "core message", value: message, wrap: true))
        }
        return items
    }

    // ── program ──────────────────────────────────────────────────────────────

    private var programItems: [DiagItem] {
        let width = snap.int("programFrame.width")
        let height = snap.int("programFrame.height")
        let size = (width != nil && height != nil && width! > 0) ? "\(width!)×\(height!)" : nil
        let warnings = snap.strings("programFrame.warnings")
        var items: [DiagItem] = [
            DiagItem(label: "health", value: snap.string("programFrame.health"),
                     tone: DiagTone.forStatus(snap.string("programFrame.health"))),
            DiagItem(label: "frame number",
                     value: DiagFormat.count(snap.number("programFrame.frameNumber"))),
            DiagItem(label: "canvas", value: size.map {
                $0 + " @ " + (snap.string("programFrame.fps") ?? "?") + "fps"
            }),
            DiagItem(label: "renderer", value: snap.string("programFrame.renderer")),
            DiagItem(label: "gpu composed",
                     value: DiagFormat.yesNo(snap.bool("programFrame.gpuComposed")),
                     tone: snap.bool("programFrame.gpuComposed") == false ? .warn : .good),
            DiagItem(label: "layers",
                     value: DiagFormat.count(snap.int("programFrame.layerCount"))),
            DiagItem(label: "render plan", value: snap.string("programFrame.renderPlanId")),
            DiagItem(label: "pixel signature",
                     value: snap.string("programFrame.programPixelSignature")),
            DiagItem(label: "program IOSurface",
                     value: model.programSurfaceId == 0 ? nil : "\(model.programSurfaceId)",
                     tone: .good),
            DiagItem(label: "multiview IOSurface",
                     value: model.multiviewSurfaceId == 0 ? nil : "\(model.multiviewSurfaceId)",
                     tone: .good),
            DiagItem(label: "preview scene", value: snap.string("previewScene.sceneId")),
            DiagItem(label: "preview layers",
                     value: DiagFormat.count(snap.int("previewScene.layerCount"))),
        ]
        if warnings.isEmpty {
            items.append(DiagItem(label: "warnings", value: "none", tone: .good))
        }
        for warning in warnings {
            items.append(DiagItem(label: "warning", value: warning, tone: .warn, wrap: true))
        }
        return items
    }

    // ── encoder ──────────────────────────────────────────────────────────────

    private var encoderItems: [DiagItem] {
        var items: [DiagItem] = [
            DiagItem(label: "status", value: snap.string("encoderSession.status"),
                     tone: DiagTone.forStatus(snap.string("encoderSession.status"))),
            DiagItem(label: "lifecycle", value: snap.string("encoderSession.lifecycle.status"),
                     tone: DiagTone.forStatus(snap.string("encoderSession.lifecycle.status"))),
            DiagItem(label: "last transition",
                     value: snap.string("encoderSession.lifecycle.lastTransition"), wrap: true),
            DiagItem(label: "encoder", value: snap.string("health.encoder")),
            DiagItem(label: "hardware encoder",
                     value: DiagFormat.yesNo(snap.bool("health.hardwareEncoder")),
                     tone: snap.bool("health.hardwareEncoder") == true ? .good : .warn),
            DiagItem(label: "codec", value: snap.string("health.codec")),
            DiagItem(label: "target bitrate",
                     value: DiagFormat.decimal(snap.number("health.targetBitrateMbps"), 1,
                                               suffix: " Mbps")),
            DiagItem(label: "encoded frames",
                     value: DiagFormat.count(snap.number("health.encodedFrameCount"))),
            DiagItem(label: "mixed audio frames",
                     value: DiagFormat.count(snap.number("health.mixedAudioFrames"))),
            DiagItem(label: "attached targets",
                     value: DiagFormat.count(snap.objects("encoderSession.targets").count)),
        ]
        for warning in snap.strings("encoderSession.warnings") {
            items.append(DiagItem(label: "warning", value: warning, tone: .warn, wrap: true))
        }
        return items
    }

    // ── recording ────────────────────────────────────────────────────────────

    private var recordingItems: [DiagItem] {
        let dropped = snap.number("recording.totalDroppedFrames")
        let status = snap.string("recording.status") ?? model.recordingStatus
        return [
            DiagItem(label: "status", value: status, tone: DiagTone.forStatus(status)),
            DiagItem(label: "writer", value: snap.string("recording.writerStatus"),
                     tone: DiagTone.forStatus(snap.string("recording.writerStatus"))),
            DiagItem(label: "elapsed",
                     value: DiagFormat.millis(snap.number("recording.elapsedMs"))),
            DiagItem(label: "target folder", value: snap.string("recording.targetFolder"),
                     wrap: true),
            DiagItem(label: "program file",
                     value: snap.string("recording.programPath")
                        ?? (model.recordingArtifactPath.isEmpty
                            ? nil : model.recordingArtifactPath),
                     wrap: true),
            DiagItem(label: "session dir", value: snap.string("recording.sessionDir"),
                     wrap: true),
            DiagItem(label: "manifest", value: snap.string("recording.manifestPath"),
                     wrap: true),
            DiagItem(label: "frames written",
                     value: DiagFormat.count(snap.number("recording.totalFramesWritten"))),
            DiagItem(label: "dropped frames", value: DiagFormat.count(dropped),
                     tone: (dropped ?? 0) > 0 ? .warn : .good),
            DiagItem(label: "bytes written",
                     value: DiagFormat.bytes(snap.number("recording.totalBytesWritten"))),
            DiagItem(label: "audio present",
                     value: DiagFormat.yesNo(snap.bool("recording.proof.audioPresent")),
                     tone: snap.bool("recording.proof.audioPresent") == false ? .bad : .good),
            DiagItem(label: "audio packets",
                     value: DiagFormat.count(snap.number("recording.proof.audioPacketsObserved"))),
            DiagItem(label: "metadata valid",
                     value: DiagFormat.yesNo(snap.bool("recording.proof.metadataValid")),
                     tone: snap.bool("recording.proof.metadataValid") == false ? .warn : .good),
            DiagItem(label: "container",
                     value: snap.string("recording.proof.containerFormat").map { container in
                         container + " · " + (snap.string("recording.proof.videoCodec") ?? "?")
                             + " / " + (snap.string("recording.proof.audioCodec") ?? "?")
                     }),
            DiagItem(label: "failures / recoveries",
                     value: snap.int("recording.proof.failureCount").map { failures in
                         "\(failures) / \(snap.int("recording.proof.recoveryCount") ?? 0)"
                     },
                     tone: (snap.int("recording.proof.failureCount") ?? 0) > 0 ? .warn : .good),
            DiagItem(label: "warning",
                     value: snap.string("recording.warning")
                        ?? (model.recordingWarning.isEmpty ? nil : model.recordingWarning),
                     tone: .warn, wrap: true),
            DiagItem(label: "error", value: snap.string("recording.error"),
                     tone: .bad, wrap: true),
        ]
    }

    private var recordingStreamItems: [DiagItem] {
        var items: [DiagItem] = []
        for stream in snap.objects("recording.streams") {
            let entry = Snap(stream)
            let kind = entry.string("kind") ?? "stream"
            let name = entry.string("displayName") ?? entry.string("sourceId") ?? kind
            let summary = [
                name,
                (DiagFormat.count(entry.number("framesWritten")) ?? "—") + " frames",
                "audio " + (DiagFormat.yesNo(entry.bool("hasAudio")) ?? "—"),
                entry.string("path"),
            ].compactMap { $0 }.joined(separator: " · ")
            items.append(DiagItem(label: kind, value: summary,
                                  tone: entry.string("warning") == nil ? .neutral : .warn,
                                  wrap: true))
            if let warning = entry.string("warning") {
                items.append(DiagItem(label: "", value: warning, tone: .warn, wrap: true))
            }
        }
        return items
    }

    private var recordingSection: some View {
        DiagSection(title: "Recording") {
            DiagRows(items: recordingItems)
            if !recordingStreamItems.isEmpty {
                MonoLabel("streams", dim: true)
                DiagRows(items: recordingStreamItems)
            }
        }
    }

    // ── streaming ────────────────────────────────────────────────────────────

    private var streamingItems: [DiagItem] {
        var items: [DiagItem] = [
            DiagItem(label: "session status", value: snap.string("outputSenderSession.status"),
                     tone: DiagTone.forStatus(snap.string("outputSenderSession.status"))),
            DiagItem(label: "active senders",
                     value: DiagFormat.count(snap.int("outputSenderSession.activeSenderCount"))),
            DiagItem(label: "operator intent",
                     value: model.streamingDesired ? "streaming requested" : "idle",
                     tone: model.streamingDesired ? .good : .neutral),
            DiagItem(label: "url", value: model.streamUrl.isEmpty ? nil : model.streamUrl,
                     wrap: true),
            // The key VALUE never appears in this pane or in a bundle; only its
            // presence, which is what an operator triaging a dead stream needs.
            DiagItem(label: "stream key",
                     value: model.streamKey.isEmpty
                        ? "not configured"
                        : "configured (Keychain — never exported)",
                     tone: model.streamKey.isEmpty ? .warn : .good),
        ]
        for sender in snap.objects("outputSenderSession.senders") {
            let entry = Snap(sender)
            items.append(DiagItem(label: entry.string("destination") ?? "sender",
                                  value: entry.string("status"),
                                  tone: DiagTone.forStatus(entry.string("status")), wrap: true))
            items.append(DiagItem(
                label: "  frames / retries",
                value: (DiagFormat.count(entry.number("framesSent")) ?? "—") + " / "
                    + (DiagFormat.count(entry.int("retryCount")) ?? "—"),
                tone: (entry.int("retryCount") ?? 0) > 0 ? .warn : .neutral))
            items.append(DiagItem(
                label: "  latency / bitrate",
                value: (DiagFormat.decimal(entry.number("latencyMs"), 0, suffix: " ms") ?? "—")
                    + " / "
                    + (DiagFormat.decimal(entry.number("bitrateMbps"), 2, suffix: " Mbps") ?? "—")))
            items.append(DiagItem(label: "  bytes sent",
                                  value: DiagFormat.bytes(entry.number("bytesSent"))))
            items.append(DiagItem(label: "  audio frames",
                                  value: DiagFormat.count(entry.number("audioFramesSent"))))
            items.append(DiagItem(label: "  destination health",
                                  value: entry.string("destinationHealth"),
                                  tone: DiagTone.forStatus(entry.string("destinationHealth"))))
            items.append(DiagItem(label: "  last result", value: entry.string("lastResultCode")))
            items.append(DiagItem(label: "  warning", value: entry.string("warning"),
                                  tone: .warn, wrap: true))
            items.append(DiagItem(label: "  last error", value: entry.string("lastError"),
                                  tone: .bad, wrap: true))
        }
        for warning in snap.strings("outputSenderSession.warnings") {
            items.append(DiagItem(label: "warning", value: warning, tone: .warn, wrap: true))
        }
        return items
    }

    // ── zoom / engine ────────────────────────────────────────────────────────

    private var zoomItems: [DiagItem] {
        let meeting = snap.string("meetingState") ?? model.meetingState
        var items: [DiagItem] = [
            DiagItem(label: "meeting state", value: meeting,
                     tone: DiagTone.forStatus(meeting)),
            DiagItem(label: "readiness", value: snap.string("zoom.readiness.status"),
                     tone: DiagTone.forStatus(snap.string("zoom.readiness.status"))),
            DiagItem(label: "mode", value: snap.string("zoom.readiness.mode"),
                     tone: snap.string("zoom.readiness.mode") == "stub" ? .warn : .neutral),
            DiagItem(label: "sdk available",
                     value: DiagFormat.yesNo(snap.bool("zoom.readiness.sdkAvailable")),
                     tone: snap.bool("zoom.readiness.sdkAvailable") == false ? .warn : .good),
            DiagItem(label: "sdk version",
                     value: snap.string("zoom.readiness.sdkVersion") ?? zoom.string("sdkVersion")),
            DiagItem(label: "evidence source", value: snap.string("zoom.evidence.source")),
            DiagItem(label: "synthetic evidence",
                     value: DiagFormat.yesNo(snap.bool("zoom.evidence.synthetic")),
                     tone: snap.bool("zoom.evidence.synthetic") == true ? .warn : .good),
            DiagItem(label: "joined",
                     value: DiagFormat.yesNo(snap.bool("zoom.evidence.joined"))),
            DiagItem(label: "participants (core)",
                     value: DiagFormat.count(snap.int("zoom.evidence.participantCount"))),
            DiagItem(label: "raw media active", value: model.rawMediaActive ? "yes" : "no",
                     tone: model.rawMediaActive ? .good : .warn),
            DiagItem(label: "capture armed", value: model.captureEnabled ? "yes" : "no",
                     tone: model.captureEnabled ? .good : .neutral),
            DiagItem(label: "roster / in show",
                     value: "\(model.roster.count) / \(model.liveInputCount)"),
            DiagItem(label: "zoom sign-in", value: model.zoomSignedIn ? "signed in" : "signed out",
                     tone: model.zoomSignedIn ? .good : .neutral),
            DiagItem(label: "zoom last error", value: zoom.string("lastError"),
                     tone: .bad, wrap: true),
        ]
        for check in snap.objects("zoom.readiness.checks") {
            let entry = Snap(check)
            items.append(DiagItem(
                label: entry.string("id") ?? "check",
                value: [entry.string("status"), entry.string("label")]
                    .compactMap { $0 }.joined(separator: " · "),
                tone: DiagTone.forStatus(entry.string("status")), wrap: true))
        }
        return items
    }

    // ── capture devices ──────────────────────────────────────────────────────

    private var captureItems: [DiagItem] {
        let devices = snap.objects("captureDevices")
        guard !devices.isEmpty else {
            return [DiagItem(label: "devices", value: "none enumerated", tone: .warn)]
        }
        var items: [DiagItem] = []
        for device in devices {
            let entry = Snap(device)
            let width = entry.int("resolution.width")
            let height = entry.int("resolution.height")
            let format = (width != nil && height != nil && width! > 0)
                ? "\(width!)×\(height!)@\(entry.string("frameRate") ?? "?")" : nil
            items.append(DiagItem(
                label: entry.string("name") ?? entry.string("id") ?? "device",
                value: [
                    entry.string("connectionState"),
                    "signal " + (DiagFormat.yesNo(entry.bool("signalPresent")) ?? "—"),
                    format,
                    "dropped " + (DiagFormat.count(entry.number("droppedFrames")) ?? "—"),
                    entry.string("vendor").map { "(" + $0 + ")" },
                ].compactMap { $0 }.joined(separator: " · "),
                tone: DiagTone.forStatus(entry.string("connectionState")), wrap: true))
            if let warning = entry.string("warning") {
                items.append(DiagItem(label: "", value: warning, tone: .warn, wrap: true))
            }
        }
        return items
    }

    // ── audio ────────────────────────────────────────────────────────────────

    private var audioItems: [DiagItem] {
        var items: [DiagItem] = [
            DiagItem(label: "mix status", value: snap.string("audioMixSession.status"),
                     tone: DiagTone.forStatus(snap.string("audioMixSession.status"))),
            DiagItem(label: "master level", value: "\(model.masterLevel)"),
            DiagItem(label: "short-term LUFS",
                     value: model.shortTermLufs <= -120
                        ? nil : String(format: "%.1f", model.shortTermLufs)),
            DiagItem(label: "true peak",
                     value: model.truePeakDbfs <= -120
                        ? nil : String(format: "%.1f dBFS", model.truePeakDbfs)),
            DiagItem(label: "limiter active", value: model.limiterActive ? "yes" : "no",
                     tone: model.limiterActive ? .warn : .good),
            DiagItem(label: "mastering ride",
                     value: String(format: "%.1f dB", model.masteringRideDb)),
            DiagItem(label: "mixed frames",
                     value: DiagFormat.count(snap.number("audioMixSession.mixedFrameCount"))),
            DiagItem(label: "monitor",
                     value: model.monitorStatus.isEmpty ? nil : model.monitorStatus,
                     tone: DiagTone.forStatus(model.monitorStatus)),
            DiagItem(label: "feedback risk", value: model.monitorFeedbackRisk ? "YES" : "no",
                     tone: model.monitorFeedbackRisk ? .bad : .good),
            DiagItem(label: "strips", value: "\(model.strips.count)"),
        ]
        for warning in snap.strings("audioMixSession.warnings") {
            items.append(DiagItem(label: "warning", value: warning, tone: .warn, wrap: true))
        }
        return items
    }

    // ── perf / lock guardrail ────────────────────────────────────────────────

    private var perfSection: some View {
        DiagSection(
            title: "Perf / lock guardrail",
            subtitle: "The core does NOT publish lock-hold or pacing telemetry in its "
                + "snapshot, so these lines are scraped from the core log. They are "
                + "evidence of past events, not a live health reading."
        ) {
            HStack(spacing: 8) {
                Button(perfScanned ? "Rescan core log" : "Scan core log") { loadPerfLines() }
                    .buttonStyle(GhostButtonStyle())
                Text(perfScanned ? "\(perfLines.count) matching line(s)" : "not scanned")
                    .font(.plexMono(10))
                    .foregroundStyle(Studio.textDim)
            }
            ForEach(Array(perfLines.enumerated()), id: \.offset) { _, line in
                Text(line)
                    .font(.plexMono(9))
                    .foregroundStyle(Studio.secondary)
                    .lineLimit(2)
                    .truncationMode(.middle)
                    .textSelection(.enabled)
            }
            if perfScanned, perfLines.isEmpty {
                DiagNote(text: "No guardrail, pacing or drop lines in the core-log tail.",
                         tone: .good)
            }
        }
    }

    private func loadPerfLines() {
        perfScanned = true
        guard let tail = LogTail.read(path: CoreLog.path, maxBytes: 512 * 1024) else {
            perfLines = []
            return
        }
        let needles = ["[lock", "guardrail", "over budget", "over-budget", "[applycommands]",
                       "[perf", "dropped", "starv", "shed", "no matching frame"]
        perfLines = tail.text
            .split(separator: "\n")
            .map(String.init)
            .filter { line in
                let lowered = line.lowercased()
                return needles.contains { lowered.contains($0) }
            }
            .suffix(40)
            .map { Redactor.scrub($0) }
    }

    // ── logs + export ────────────────────────────────────────────────────────

    private var logItems: [DiagItem] {
        [
            DiagItem(label: "shell log",
                     value: ShellLog.path + "  (" + DiagFormat.fileSize(ShellLog.path) + ")",
                     wrap: true),
            DiagItem(label: "core log",
                     value: CoreLog.path + "  (" + DiagFormat.fileSize(CoreLog.path) + ")",
                     wrap: true),
            DiagItem(label: "media bin", value: MediaBin.root, wrap: true),
            DiagItem(label: "crash reports",
                     value: NSHomeDirectory() + "/Library/Logs/DiagnosticReports", wrap: true),
            DiagItem(label: "bundle folder", value: SupportBundle.defaultRoot.path, wrap: true),
        ]
    }

    private var logsSection: some View {
        DiagSection(title: "Logs and support bundle") {
            DiagRows(items: logItems)
            HStack(spacing: 8) {
                Button("Reveal shell log") { reveal(path: ShellLog.path) }
                    .buttonStyle(GhostButtonStyle())
                Button("Reveal core log") { reveal(path: CoreLog.path) }
                    .buttonStyle(GhostButtonStyle())
            }
            HStack(spacing: 8) {
                Button(exporting ? "Exporting…" : "Export support bundle") { exportBundle() }
                    .buttonStyle(AccentButtonStyle())
                    .disabled(exporting)
                if let exportedURL {
                    Button("Reveal bundle") {
                        NSWorkspace.shared.activateFileViewerSelecting([exportedURL])
                    }
                    .buttonStyle(GhostButtonStyle())
                }
            }
            if !exportStatus.isEmpty {
                DiagNote(text: exportStatus, tone: exportTone)
            }
            Text("The bundle is written locally and redacted before it touches disk: stream "
                 + "keys, Zoom OAuth tokens, userZak, sdkJwt, passcodes and passphrases are "
                 + "stripped by key name, by value pattern and by literal match, then every "
                 + "written file is re-read and re-checked. File paths are kept verbatim and "
                 + "crash reports are listed, never copied. Nothing is uploaded anywhere.")
                .font(.grotesk(10))
                .foregroundStyle(Studio.textDim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func reveal(path: String) {
        let url = URL(fileURLWithPath: path)
        if FileManager.default.fileExists(atPath: path) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            NSWorkspace.shared.activateFileViewerSelecting([url.deletingLastPathComponent()])
        }
    }

    private func exportBundle() {
        exporting = true
        exportStatus = ""
        // Everything crossing the queue hop is pre-serialized Data plus value
        // types; the live snapshot dictionaries never leave the main actor.
        let input = model.supportBundleInput()
        DispatchQueue.global(qos: .userInitiated).async {
            let outcome = Result { try SupportBundle.write(input) }
            DispatchQueue.main.async {
                exporting = false
                switch outcome {
                case .success(let bundle):
                    exportedURL = bundle.revealTarget
                    var message = "Exported \(bundle.includedFiles.count) file(s) to "
                        + bundle.revealTarget.path
                    if bundle.archive == nil {
                        message += " — the zip step failed, so the folder IS the bundle."
                    }
                    if bundle.literalHits > 0 {
                        message += "  ⚠ \(bundle.literalHits) known-secret occurrence(s) "
                            + "reached the final verification pass and were scrubbed there. "
                            + "Please report this."
                        exportTone = .warn
                    } else {
                        exportTone = .good
                    }
                    exportStatus = message
                    ShellLog.write("support bundle exported to \(bundle.revealTarget.path)")
                case .failure(let error):
                    exportedURL = nil
                    exportTone = .bad
                    exportStatus = "Export failed: \(error.localizedDescription)"
                    ShellLog.write("support bundle export FAILED: \(error)")
                }
            }
        }
    }

    // ── warnings ─────────────────────────────────────────────────────────────

    private var warningsSection: some View {
        DiagSection(title: "Warnings") {
            if model.warnings.isEmpty {
                DiagRow(label: "session", value: "none", tone: .good)
            }
            ForEach(Array(model.warnings.enumerated()), id: \.offset) { _, warning in
                Text(warning)
                    .font(.plexMono(10))
                    .foregroundStyle(Studio.amber)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
