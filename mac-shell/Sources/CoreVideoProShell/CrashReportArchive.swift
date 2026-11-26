// CrashReportArchive.swift: the consented crash report (Phase 4 R5, Task 11). Ports of Windows
// CrashReportArchiveBuilder, CrashReportUploader and CrashReportCoordinator.
//
// PRIVACY (decision D-X2): the Mac zip carries SANITIZED .ips reports (IpsSanitizer) and
// REDACTED log tails (Redactor.redactLines), and its manifest names files, never local paths.
// Windows uploads raw dumps and raw log tails, and its skip lines embed full local paths; those
// are Windows defects (listed as candidate issues), not behaviour to copy.

import Foundation

struct CrashArchiveRequest {
    var reports: [CrashReportFile]
    var logFiles: [String]
    var supportBundlePath: String?
    var maxTotalBytes: Int64 = 24 << 20
    var logTailBytes: Int64 = 2 << 20
    var appVersion: String
    /// Live secret VALUES held by the shell (stream key, SRT passphrase, meeting passcode): scrubbed
    /// by identity from every entry, then verified absent (the support bundle's two steps).
    var knownSecretLiterals: [String] = []
}

struct CrashArchiveResult: Equatable {
    var zipPath: String
    var zipLength: Int64
    var included: [String]
    var skipped: [String]
    var manifestText: String
}

struct CrashArchiveError: LocalizedError { let errorDescription: String? }

enum CrashReportArchiveBuilder {
    /// The last `maxBytes` of a file and its full length (Windows ReadTailBounded).
    static func readTail(_ path: String, maxBytes: Int64) throws -> (Data, Int64) {
        let handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: path))
        defer { try? handle.close() }
        let length = Int64(try handle.seekToEnd())
        let count = min(length, max(0, maxBytes))
        try handle.seek(toOffset: UInt64(length - count))
        return (try handle.read(upToCount: Int(count)) ?? Data(), length)
    }

    /// What leaves the machine from a log tail: every line through the shell's redactor
    /// (Redactor.redactLines: secret-named values, URL keys, Bearer tokens, JWTs, high-entropy
    /// runs). When the tail starts mid-file its first line is a fragment that a line-based
    /// pattern may not recognise (a key cut at the seek point loses its rtmp:// prefix and its
    /// length), so that partial line is DROPPED, never sent.
    static func redactTail(_ data: Data, truncated: Bool, literals: [String] = []) -> Data {
        var text = String(decoding: data, as: UTF8.self)
        if truncated {
            if let newline = text.firstIndex(of: "\n") { text = String(text[text.index(after: newline)...]) } else { text = "" }
        }
        // A line longer than the scrub bound is omitted, never scanned (the redactor is
        // quadratic on one long run) and never sent raw.
        let bounded = text.split(separator: "\n", omittingEmptySubsequences: false).map { line -> String in
            line.utf8.count > IpsSanitizer.maxScrubbedLength ? "[line omitted: \(line.utf8.count) characters]" : String(line)
        }.joined(separator: "\n")
        return Data(Redactor.redactLines(bounded, literals: literals).utf8)
    }

    /// A one-line reason with no local path (exception text can quote one).
    private static func reason(_ error: Error) -> String {
        IpsSanitizer.scrubPath(error.localizedDescription)
    }

    /// True when any known literal (4+ characters, as the support bundle) is still in `data`.
    /// The final check on an entry, read as the receiver reads it: the raw text, and, when the
    /// entry is JSON (or an .ips header + body), every unescaped key and string as well (JSON
    /// escaping of '"' and '\\' hides a literal from a byte search).
    static func carriesLiteral(_ data: Data, _ literals: [String]) -> Bool {
        // Every form: as typed and JSON-escaped (with and without "\/"), so a text line that
        // embeds a JSON string is covered, not only whole-JSON entries.
        if literals.contains(where: { literal in Redactor.literalForms(literal).contains { data.range(of: Data($0.utf8)) != nil } }) {
            return true
        }
        if let object = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]) {
            return IpsSanitizer.carriesLiteral(object, literals)
        }
        let text = String(decoding: data, as: UTF8.self)
        return text.split(separator: "\n").contains { line in
            guard let object = try? JSONSerialization.jsonObject(with: Data(line.utf8), options: [.fragmentsAllowed]) else { return false }
            return IpsSanitizer.carriesLiteral(object, literals)
        }
    }

    /// The manifest is generated text that can quote a file name: scrubbed with the literals,
    /// then any line that still carries one is withheld.
    static func scrubManifest(_ text: String, literals: [String]) -> String {
        Redactor.redactLines(text, literals: literals)
            .split(separator: "\n", omittingEmptySubsequences: false)
            .map { line in
                literals.contains { literal in Redactor.literalForms(literal).contains { line.contains($0) } }
                    ? "[line withheld: a known secret]" : String(line)
            }
            .joined(separator: "\n")
    }

    static func build(zipPath: String, request: CrashArchiveRequest, now: Date) throws -> CrashArchiveResult {
        let literals = request.knownSecretLiterals.filter { $0.count >= 4 }
        var included: [String] = []
        var skipped: [String] = []
        var manifest = ["CoreVideo Pro crash report", "Created: \(TelemetryConsentStore.formatDate(now))",
                        "App version: \(request.appVersion)", "Content budget: \(request.maxTotalBytes) bytes (uncompressed)", ""]

        var tails: [(entry: String, bytes: Data, original: Int64, truncated: Bool)] = []
        for path in request.logFiles {
            let name = (path as NSString).lastPathComponent
            guard FileManager.default.fileExists(atPath: path) else { skipped.append("\(name) — not found"); continue }
            do {
                let (raw, length) = try readTail(path, maxBytes: request.logTailBytes)
                let truncated = length > Int64(raw.count)
                tails.append(("logs/\(name)", redactTail(raw, truncated: truncated, literals: literals), length, truncated))
            } catch {
                skipped.append("\(name) — unreadable (\(reason(error)))")
            }
        }

        let bundleEntry = "support-bundle.json"
        var bundle: Data?
        if let bundlePath = request.supportBundlePath {
            if let data = FileManager.default.contents(atPath: bundlePath) { bundle = data } else {
                skipped.append("\(bundleEntry) — support bundle export produced no file")
            }
        } else {
            // The Mac support bundle keeps local file paths (recording folders carry the user
            // name), and the privacy rule says no path leaves the machine.
            skipped.append("\(bundleEntry) — not included (it keeps local file paths, which never leave this Mac)")
        }

        // Sanitize first: the budget is spent on what will actually be sent.
        var sanitized: [(file: CrashReportFile, bytes: Data)] = []
        for report in request.reports {
            let name = (report.path as NSString).lastPathComponent
            guard let raw = FileManager.default.contents(atPath: report.path) else {
                skipped.append("\(name) — unreadable"); continue
            }
            switch IpsSanitizer.sanitizeChecked(raw, literals: literals) {
            case .clean(let clean): sanitized.append((report, clean))
            case .literalSurvived: skipped.append("\(name) — withheld: a known secret survived redaction")
            case .unreadable: skipped.append("\(name) — not a readable crash report; never sent raw")
            }
        }

        // The support bundle's final check: an entry that still carries a known literal is
        // WITHHELD (the manifest says so, never quoting the secret).
        let withheld = "withheld: a known secret survived redaction"
        tails.removeAll { tail in
            guard carriesLiteral(tail.bytes, literals) else { return false }
            skipped.append("\((tail.entry as NSString).lastPathComponent) — \(withheld)"); return true
        }
        if let data = bundle, carriesLiteral(data, literals) { bundle = nil; skipped.append("\(bundleEntry) — \(withheld)") }
        sanitized.removeAll { item in
            guard carriesLiteral(item.bytes, literals) else { return false }
            skipped.append("\((item.file.path as NSString).lastPathComponent) — \(withheld)"); return true
        }

        let reserved = tails.reduce(Int64(0)) { $0 + Int64($1.bytes.count) } + Int64(bundle?.count ?? 0)
        let budget = max(0, request.maxTotalBytes - reserved - 64 * 1024)
        var remaining = budget
        var plan: [(file: CrashReportFile, bytes: Data)] = []
        // Largest-first greedy (Windows): prefer the biggest reports that fit.
        for item in sanitized.sorted(by: { $0.bytes.count > $1.bytes.count }) {
            let size = Int64(item.bytes.count)
            if size <= remaining { plan.append(item); remaining -= size } else {
                skipped.append("\((item.file.path as NSString).lastPathComponent) — \(size) bytes exceeds the remaining budget "
                               + "(\(remaining) of \(budget) report bytes left; total cap \(request.maxTotalBytes))")
            }
        }
        plan.sort { $0.file.lastWrite < $1.file.lastWrite }   // oldest first, as the scanner

        let fm = FileManager.default
        let staging = NSTemporaryDirectory() + "cvp-crash-staging-\(UUID().uuidString)"
        defer { try? fm.removeItem(atPath: staging) }
        try fm.createDirectory(atPath: staging + "/dumps", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: staging + "/logs", withIntermediateDirectories: true)
        for item in plan {
            let entry = "dumps/\((item.file.path as NSString).lastPathComponent)"
            try item.bytes.write(to: URL(fileURLWithPath: staging + "/" + entry))
            included.append(entry)
            manifest.append("Included: \(entry) (\(item.bytes.count) bytes, \(item.file.processName), last write "
                            + "\(TelemetryConsentStore.formatDate(item.file.lastWrite)), sanitized)")
        }
        for tail in tails {
            try tail.bytes.write(to: URL(fileURLWithPath: staging + "/" + tail.entry))
            included.append(tail.entry)
            manifest.append(tail.truncated
                ? "Included: \(tail.entry) (last \(tail.bytes.count) bytes of a \(tail.original)-byte file, redacted)"
                : "Included: \(tail.entry) (\(tail.bytes.count) bytes, complete, redacted)")
        }
        if let bundle {
            try bundle.write(to: URL(fileURLWithPath: staging + "/" + bundleEntry))
            included.append(bundleEntry)
            manifest.append("Included: \(bundleEntry) (\(bundle.count) bytes, redacted diagnostics snapshot)")
        }
        if !skipped.isEmpty { manifest.append(""); manifest += skipped.map { "Skipped: \($0)" } }
        let manifestText = scrubManifest(manifest.joined(separator: "\n") + "\n", literals: literals)
        try Data(manifestText.utf8).write(to: URL(fileURLWithPath: staging + "/manifest.txt"))
        included.insert("manifest.txt", at: 0)
        if plan.isEmpty { try? fm.removeItem(atPath: staging + "/dumps") }

        try fm.createDirectory(atPath: (zipPath as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        try? fm.removeItem(atPath: zipPath)
        let ditto = Process()
        ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        ditto.arguments = ["-c", "-k", staging, zipPath]   // entries at the root (no --keepParent)
        try ditto.run()
        ditto.waitUntilExit()
        guard ditto.terminationStatus == 0 else { throw CrashArchiveError(errorDescription: "ditto exited \(ditto.terminationStatus)") }
        let length = (try? fm.attributesOfItem(atPath: zipPath)[.size] as? NSNumber)?.int64Value ?? 0
        return CrashArchiveResult(zipPath: zipPath, zipLength: length, included: included, skipped: skipped, manifestText: manifestText)
    }
}

/// POST one crash zip: (endpoint, key, headers, body, timeout) -> outcome.
typealias CrashTransport = (_ url: URL, _ apiKey: String, _ headers: [String: String], _ body: Data,
                            _ timeout: TimeInterval) async -> TelemetrySendResult

enum CrashReportUploader {
    static let timeout: TimeInterval = 120   // 24 MB on venue Wi-Fi (Windows)

    static func crashesURL(_ endpoint: URL) -> URL {
        URL(string: "/v1/crashes", relativeTo: endpoint)?.absoluteURL ?? endpoint
    }

    /// Windows' classes, with its crash wording.
    static func classify(status: Int, headers: [String: String], body: Data, now: Date) -> TelemetrySendResult {
        var result = TelemetryEventClient.classify(status: status, headers: headers, body: body, now: now)
        if result.status == .rejected, let message = result.message {
            result.message = message.replacingOccurrences(of: "rejected the event", with: "rejected the report")
        }
        return result
    }

    static func upload(endpoint: URL, apiKey: String, version: String?, machineClass: String?, reason: String?,
                       zipPath: String, transport: CrashTransport) async -> TelemetrySendResult {
        let body: Data
        do { body = try Data(contentsOf: URL(fileURLWithPath: zipPath)) } catch {
            return TelemetrySendResult(status: .rejected, message: "Could not read the report archive: \(IpsSanitizer.scrubPath(error.localizedDescription))")
        }
        var headers = ["Content-Type": "application/zip"]
        if let version, !version.isEmpty { headers["X-CoreVideo-Version"] = version }
        if let machineClass, !machineClass.isEmpty { headers["X-CoreVideo-Machine-Class"] = machineClass }
        if let reason, !reason.isEmpty { headers["X-CoreVideo-Reason"] = reason }
        return await transport(crashesURL(endpoint), apiKey, headers, body, timeout)
    }

    /// The real transport: the telemetry session (redirects refused, total timeout, neutral UA).
    static func urlSessionTransport(base: URLSessionConfiguration = .ephemeral, now: @escaping () -> Date = Date.init) -> CrashTransport {
        { url, apiKey, headers, body, timeout in
            var request = URLRequest(url: url, timeoutInterval: timeout)
            request.httpMethod = "POST"
            request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
            request.setValue(TelemetryEventClient.userAgent, forHTTPHeaderField: "User-Agent")
            for (key, value) in headers { request.setValue(value, forHTTPHeaderField: key) }
            request.httpBody = body
            let session = TelemetryEventClient.session(timeout: timeout, base: base)
            defer { session.finishTasksAndInvalidate() }
            do {
                let (data, response) = try await session.data(for: request)
                guard let http = response as? HTTPURLResponse else {
                    return TelemetrySendResult(status: .retryLater, message: "Upload failed: no HTTP response")
                }
                var responseHeaders: [String: String] = [:]
                for (key, value) in http.allHeaderFields { responseHeaders["\(key)"] = "\(value)" }
                return classify(status: http.statusCode, headers: responseHeaders, body: data, now: now())
            } catch {
                return TelemetrySendResult(status: .retryLater, message: "Upload failed: \(error.localizedDescription)")
            }
        }
    }
}

struct CrashSendOutcome: Equatable {
    var success: Bool
    var canRetry: Bool
    var message: String
    var reportId: String?
    var zipPath: String?
}

@MainActor
final class CrashReportCoordinator {
    let config: TelemetryConfig
    let store: CrashReportWatermarkStore
    let directories: [String]
    let logFiles: [String]
    let bundleDirectory: String
    let version: String
    let machineClass: String
    let transport: CrashTransport
    let now: () -> Date
    let log: (String) -> Void
    /// The shell's live secret values at send time (the support bundle's source).
    let secretLiterals: () -> [String]

    private init(config: TelemetryConfig, store: CrashReportWatermarkStore, directories: [String], logFiles: [String],
                 bundleDirectory: String, version: String, machineClass: String, transport: @escaping CrashTransport,
                 now: @escaping () -> Date, log: @escaping (String) -> Void, secretLiterals: @escaping () -> [String]) {
        self.config = config; self.store = store; self.directories = directories; self.logFiles = logFiles
        self.bundleDirectory = bundleDirectory; self.version = version; self.machineClass = machineClass
        self.transport = transport; self.now = now; self.log = log; self.secretLiterals = secretLiterals
    }

    /// nil without a key: no scan, no bar, nothing sent (Windows gates on the key; consent for
    /// a crash report is the explicit "Send report" click).
    static func make(config: TelemetryConfig, store: CrashReportWatermarkStore, directories: [String],
                     logFiles: [String], bundleDirectory: String, version: String, machineClass: String,
                     transport: @escaping CrashTransport, now: @escaping () -> Date = Date.init,
                     log: @escaping (String) -> Void = { ShellLog.write($0) },
                     secretLiterals: @escaping () -> [String] = { [] }) -> CrashReportCoordinator? {
        guard config.enabled else { return nil }
        return CrashReportCoordinator(config: config, store: store, directories: directories, logFiles: logFiles,
                                      bundleDirectory: bundleDirectory, version: version, machineClass: machineClass,
                                      transport: transport, now: now, log: log, secretLiterals: secretLiterals)
    }

    func scanAndAdvance(fs: CrashFS = .real) -> [CrashReportFile] {
        CrashReportOffer.scanAndAdvance(directories: directories, store: store, now: now(), fs: fs)
    }

    /// "ips: <processes>" (distinct, case-insensitive, comma-joined). `wer-dump` is a Windows word.
    static func reason(_ reports: [CrashReportFile]) -> String {
        var seen = Set<String>(), names: [String] = []
        for report in reports where seen.insert(report.processName.lowercased()).inserted { names.append(report.processName) }
        return names.isEmpty ? "ips" : "ips: " + names.joined(separator: ", ")
    }

    func send(_ reports: [CrashReportFile], writeSupportBundle: (String) async -> Bool) async -> CrashSendOutcome {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX"); formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyyMMddHHmmss"
        let stamp = formatter.string(from: now())
        let zipPath = bundleDirectory + "/crash-report-\(stamp).zip"
        let bundlePath = bundleDirectory + "/crash-report-\(stamp)-support-bundle.json"
        let wroteBundle = await writeSupportBundle(bundlePath)
        defer { try? FileManager.default.removeItem(atPath: bundlePath) }
        let archive: CrashArchiveResult
        do {
            archive = try CrashReportArchiveBuilder.build(
                zipPath: zipPath,
                request: CrashArchiveRequest(reports: reports, logFiles: logFiles,
                                             supportBundlePath: wroteBundle ? bundlePath : nil, appVersion: version,
                                             knownSecretLiterals: secretLiterals()),
                now: now())
        } catch {
            log("crash-report: could not assemble (\(IpsSanitizer.scrubPath(error.localizedDescription)))")
            return CrashSendOutcome(success: false, canRetry: true,
                                    message: "Could not assemble the crash report: \(IpsSanitizer.scrubPath(error.localizedDescription))",
                                    reportId: nil, zipPath: nil)
        }
        guard let apiKey = config.apiKey, let endpoint = URL(string: config.endpoint) else {
            return CrashSendOutcome(success: false, canRetry: false, message: "Telemetry is not configured.", reportId: nil, zipPath: archive.zipPath)
        }
        let result = await CrashReportUploader.upload(endpoint: endpoint, apiKey: apiKey, version: version,
                                                      machineClass: machineClass, reason: Self.reason(reports),
                                                      zipPath: archive.zipPath, transport: transport)
        if result.status == .accepted {
            if let id = result.reportId, !id.trimmingCharacters(in: .whitespaces).isEmpty {
                var watermark = store.load(now: now())
                watermark.reports.append(CrashReportRecord(reportId: id, sentAtUtc: now(),
                                                           dumpFiles: reports.map { ($0.path as NSString).lastPathComponent }))
                try? store.save(watermark)
            }
            log("crash-report: accepted, reportId=\(result.reportId ?? "(none)")")
            let idText = result.reportId.map { " Reference ID: \($0)." } ?? ""
            return CrashSendOutcome(success: true, canRetry: false,
                                    message: "Thanks — the report was sent.\(idText) A copy stays at \(archive.zipPath).",
                                    reportId: result.reportId, zipPath: archive.zipPath)
        }
        let canRetry = result.status == .retryLater
        log("crash-report: upload \(canRetry ? "deferred" : "rejected") (http=\(result.httpStatus.map(String.init) ?? "n/a")): \(result.message ?? "")")
        return CrashSendOutcome(success: false, canRetry: canRetry,
                                message: "\(result.message ?? "") The report is saved at \(archive.zipPath) — nothing was lost.",
                                reportId: nil, zipPath: archive.zipPath)
    }

    /// The Mac's crash report folder: ~/Library/Application Support/CoreVideoPro/support-bundles.
    nonisolated static func defaultBundleDirectory() -> String {
        ((TelemetryConsentStore.defaultPath() as NSString).deletingLastPathComponent as NSString)
            .appendingPathComponent("support-bundles")
    }
}
