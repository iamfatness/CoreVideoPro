// CrashReports.swift: crash input for the consented crash report (Phase 4 R5, Task 10;
// docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md).
//
// - CrashReportScanner: macOS .ips reports (the OS-level twin of Windows' WER LocalDumps,
//   decision D-X1) for the shell, core, engine and helpers, newer than the watermark, oldest
//   first. Port of Windows CrashDumpScanner (MC/Services/CrashDumpScanner.cs).
// - CrashReportWatermarkStore: offer each report once (Windows CrashReportWatermarkStore,
//   same JSON, 3-day first-run lookback).
// - IpsSanitizer (D-X2): an ALLOW-LIST copy of a report. Everything not named is dropped,
//   including crashReporterKey, bootSessionUUID, userID, incident ids, vmSummary and
//   trialInfo, and any "/Users/<name>" left in a string becomes "/Users/USER". An unparsable
//   report yields nil and is never sent raw.

import Foundation

struct CrashReportFile: Equatable {
    var path: String
    var processName: String
    var lastWrite: Date
    var length: Int64
}

/// The file-system seam for the scanner.
struct CrashFS {
    /// File names in a directory; throws when the directory cannot be listed, [] when it is absent.
    var list: (String) throws -> [String]
    /// Modification date and length of a file, or nil when it vanished.
    var attributes: (String) -> (Date, Int64)?

    static let real = CrashFS(
        list: { directory in
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: directory, isDirectory: &isDirectory), isDirectory.boolValue else { return [] }
            return try FileManager.default.contentsOfDirectory(atPath: directory)
        },
        attributes: { path in
            guard let attributes = try? FileManager.default.attributesOfItem(atPath: path),
                  let date = attributes[.modificationDate] as? Date else { return nil }
            return (date, (attributes[.size] as? NSNumber)?.int64Value ?? 0)
        })
}

enum CrashReportScanner {
    static let monitoredProcesses = ["CoreVideoProShell", "corevideo-native", "corevideo-zoom-engine",
                                     "corevideo-plugin-host", "corevideo-browser-host"]

    /// "<process>-YYYY-MM-DD-HHMMSS[.<digits>].ips" for an EXACT monitored name, else nil
    /// ("corevideo-native-tests-..." is not "corevideo-native").
    static func processName(forFile name: String) -> String? {
        for process in monitoredProcesses {
            let pattern = "^" + NSRegularExpression.escapedPattern(for: process) + #"-\d{4}-\d{2}-\d{2}-\d{6}(\.\d+)?\.ips$"#
            if name.range(of: pattern, options: .regularExpression) != nil { return process }
        }
        return nil
    }

    /// Reports newer than `after` across `directories`, each listed once, oldest first. A
    /// missing or unreadable directory is skipped without hiding the others.
    static func scan(directories: [String], after: Date, fs: CrashFS = .real) -> [CrashReportFile] {
        var byPath: [String: CrashReportFile] = [:]
        for directory in directories {
            guard let names = try? fs.list(directory) else { continue }
            for name in names {
                guard let process = processName(forFile: name) else { continue }
                let path = (directory as NSString).appendingPathComponent(name)
                guard let (date, length) = fs.attributes(path), date > after else { continue }
                let key = (path as NSString).standardizingPath
                if byPath[key] == nil { byPath[key] = CrashReportFile(path: path, processName: process, lastWrite: date, length: length) }
            }
        }
        return byPath.values.sorted { ($0.lastWrite, $0.path) < ($1.lastWrite, $1.path) }
    }

    /// ~/Library/Logs/DiagnosticReports and its Retired folder.
    static func defaultDirectories() -> [String] {
        let base = NSHomeDirectory() + "/Library/Logs/DiagnosticReports"
        return [base, base + "/Retired"]
    }
}

struct CrashReportWatermark: Codable, Equatable {
    var lastSeenUtc: Date
    var reports: [CrashReportRecord] = []
}

final class CrashReportWatermarkStore {
    static let firstRunLookback: TimeInterval = 3 * 86400
    let path: String

    init(path: String) { self.path = path }

    /// ~/Library/Application Support/CoreVideoPro/crash-watermark.json
    static func defaultPath() -> String {
        ((TelemetryConsentStore.defaultPath() as NSString).deletingLastPathComponent as NSString)
            .appendingPathComponent("crash-watermark.json")
    }

    private static let decoder: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .custom { decoder in
            let text = try decoder.singleValueContainer().decode(String.self)
            guard let date = TelemetryConsentStore.parseDate(text) else {
                throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "date"))
            }
            return date
        }
        return decoder
    }()

    private static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted]
        encoder.dateEncodingStrategy = .custom { date, encoder in
            var container = encoder.singleValueContainer()
            try container.encode(TelemetryConsentStore.formatDate(date))
        }
        return encoder
    }()

    /// A missing, corrupt or unreadable file is the fresh default: lookback from `now`.
    func load(now: Date) -> CrashReportWatermark {
        if let data = FileManager.default.contents(atPath: path),
           let watermark = try? Self.decoder.decode(CrashReportWatermark.self, from: data) {
            return watermark
        }
        return CrashReportWatermark(lastSeenUtc: now.addingTimeInterval(-Self.firstRunLookback))
    }

    func save(_ watermark: CrashReportWatermark) throws {
        try FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                                withIntermediateDirectories: true)
        let data = try Self.encoder.encode(watermark)
        let temp = path + ".\(getpid()).tmp"
        try data.write(to: URL(fileURLWithPath: temp))
        defer { try? FileManager.default.removeItem(atPath: temp) }
        guard rename(temp, path) == 0 else { throw CocoaError(.fileWriteUnknown) }
    }
}

enum IpsSanitizer {
    static let headerKeys: Set<String> = ["app_name", "app_version", "build_version", "bug_type", "os_version",
                                          "platform", "slice_uuid", "timestamp", "name"]
    static let bodyKeys: Set<String> = ["procName", "cpuType", "osVersion", "version", "captureTime", "exception",
                                        "termination", "faultingThread", "threads", "usedImages", "legacyInfo", "asi",
                                        "ktriageinfo", "sharedCache"]
    static let imageKeys: Set<String> = ["uuid", "base", "size", "arch", "name", "source"]
    static let threadKeys: Set<String> = ["triggered", "id", "threadState", "queue", "frames"]

    /// The allow-listed copy: header line, "\n", body JSON. nil when the input is not an .ips.
    /// Kept free text (asi, exception, termination, ktriageinfo, legacyInfo, queue names) goes
    /// through the shell's redactor and the known secret literals, not only the path scrub.
    static func sanitize(_ ips: Data, literals: [String] = []) -> Data? {
        if case .clean(let data) = sanitizeChecked(ips, literals: literals) { return data }
        return nil
    }

    enum Outcome: Equatable { case clean(Data), unreadable, literalSurvived }

    /// True when any known literal (4+ characters) is in a KEY or STRING of `value`, read as
    /// the receiver will read it: unescaped, before serialization.
    static func carriesLiteral(_ value: Any, _ literals: [String]) -> Bool {
        let wanted = literals.filter { $0.count >= 4 }
        guard !wanted.isEmpty else { return false }
        func walk(_ value: Any) -> Bool {
            switch value {
            case let text as String: return wanted.contains { literal in Redactor.literalForms(literal).contains { text.contains($0) } }
            case let array as [Any]: return array.contains(where: walk)
            case let object as [String: Any]:
                return object.contains { key, element in
                    wanted.contains { literal in Redactor.literalForms(literal).contains { key.contains($0) } } || walk(element)
                }
            default: return false
            }
        }
        return walk(value)
    }

    /// sanitize, with the support bundle's final check run on the unescaped keys and values.
    static func sanitizeChecked(_ ips: Data, literals: [String] = []) -> Outcome {
        guard let text = String(data: ips, encoding: .utf8) else { return .unreadable }
        let parts = text.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false)
        guard parts.count == 2,
              let header = (try? JSONSerialization.jsonObject(with: Data(parts[0].utf8))) as? [String: Any],
              let body = (try? JSONSerialization.jsonObject(with: Data(parts[1].utf8))) as? [String: Any] else { return .unreadable }
        var cleanBody = body.filter { bodyKeys.contains($0.key) }
        if let images = cleanBody["usedImages"] as? [[String: Any]] {
            cleanBody["usedImages"] = images.map { $0.filter { imageKeys.contains($0.key) } }
        }
        if let threads = cleanBody["threads"] as? [[String: Any]] {
            cleanBody["threads"] = threads.map { $0.filter { threadKeys.contains($0.key) } }
        }
        let cleanHeader = scrub(header.filter { headerKeys.contains($0.key) }, literals: literals)
        let scrubbedBody = scrub(cleanBody, literals: literals)
        if carriesLiteral(cleanHeader, literals) || carriesLiteral(scrubbedBody, literals) { return .literalSurvived }
        // Slashes unescaped: a reader (and any text search for a path) sees the real characters.
        guard let headerData = try? JSONSerialization.data(withJSONObject: cleanHeader, options: [.sortedKeys, .withoutEscapingSlashes]),
              let bodyData = try? JSONSerialization.data(withJSONObject: scrubbedBody,
                                                         options: [.sortedKeys, .prettyPrinted, .withoutEscapingSlashes])
        else { return .unreadable }
        return .clean(headerData + Data("\n".utf8) + bodyData)
    }

    /// Every string value: "/Users/<name>" -> "/Users/USER", then the shell's redactor (secret
    /// URLs, key=value, Bearer, JWTs, high-entropy runs) and the known literals. Keys: path scrub.
    /// Longer kept strings are OMITTED, not scanned: the redactor's high-entropy rule is quadratic
    /// on a long run (a 700 KiB string never finished), and nothing that long belongs in a report.
    static let maxScrubbedLength = 8192

    static func scrub(_ value: Any, literals: [String] = []) -> Any {
        switch value {
        case let text as String:
            if text.utf8.count > maxScrubbedLength { return "[omitted: \(text.utf8.count) characters]" }
            return Redactor.scrub(scrubPath(text), literals: literals)
        case let array as [Any]: return array.map { scrub($0, literals: literals) }
        case let object as [String: Any]:
            // Keys get the same redactor, literals and bound as values. Two keys that redact to
            // the same text are both kept ("<key> (2)", ...), never silently overwritten.
            var out: [String: Any] = [:]
            for key in object.keys.sorted() {
                var clean = key.utf8.count > maxScrubbedLength
                    ? "[omitted: \(key.utf8.count) characters]" : Redactor.scrub(scrubPath(key), literals: literals)
                if out[clean] != nil {
                    var n = 2
                    while out["\(clean) (\(n))"] != nil { n += 1 }
                    clean = "\(clean) (\(n))"
                }
                out[clean] = scrub(object[key] as Any, literals: literals)
            }
            return out
        default: return value
        }
    }

    static func scrubPath(_ text: String) -> String {
        guard text.contains("/Users/") else { return text }
        return text.replacingOccurrences(of: #"/Users/(?!USER(/|$))[^/\s"]+"#, with: "/Users/USER", options: .regularExpression)
    }
}

enum CrashReportOffer {
    /// The launch scan: reports newer than the watermark, and the watermark advanced to the
    /// newest one AT ONCE (Windows: a dump is offered once, whether or not the operator sends
    /// it; a relaunch after dismissing offers nothing). A save failure only risks a re-offer.
    static func scanAndAdvance(directories: [String], store: CrashReportWatermarkStore, now: Date,
                               fs: CrashFS = .real) -> [CrashReportFile] {
        var watermark = store.load(now: now)
        let found = CrashReportScanner.scan(directories: directories, after: watermark.lastSeenUtc, fs: fs)
        if let newest = found.map(\.lastWrite).max(), newest > watermark.lastSeenUtc {
            watermark.lastSeenUtc = newest
            try? store.save(watermark)
        }
        return found
    }
}
