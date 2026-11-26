// Phase 4 R5 crash reports (docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md,
// Batch X). The fixture mac-shell/Tests/Fixtures/crash/CoreVideoProShell-fixture.ips is a trimmed
// real report with every identity value replaced by a SENTINEL, one planted "/Users/jdoe" path in
// an asi message and one unredacted usedImages[1].path. Fixture invariants: the faulting thread
// (0) has frame 0 at imageIndex 0; usedImages[0].uuid is 11111111-2222-3333-4444-555555555555 and
// its base 4294967296.

import Foundation

extension ShellTests {

    @MainActor
    static var crashCases: [(String, () -> Void)] {
        [
            ("crash/name-match", testCrashNameMatch),
            ("crash/scan-order-and-watermark", testCrashScanOrderAndWatermark),
            ("crash/offer-once", testCrashOfferOnce),
            ("crash/watermark-first-run", testCrashWatermarkFirstRun),
            ("crash/sanitize-drops-identity", testCrashSanitizeDropsIdentity),
            ("crash/sanitize-keeps-symbolication", testCrashSanitizeKeepsSymbolication),
            ("crash/sanitize-garbage-is-nil", testCrashSanitizeGarbageIsNil),
            ("crash/archive-log-tail-redacted", testCrashArchiveLogTailRedacted),
            ("crash/archive-log-tail-cut-mid-secret", testCrashArchiveLogTailCutMidSecret),
            ("crash/archive-entries-and-manifest", testCrashArchiveEntriesAndManifest),
            ("crash/archive-budget-largest-first", testCrashArchiveBudgetLargestFirst),
            ("crash/archive-single-oversized", testCrashArchiveSingleOversized),
            ("crash/upload-request-shape", testCrashUploadRequestShape),
            ("crash/upload-outcomes", testCrashUploadOutcomes),
            ("crash/coordinator-disabled-without-key", testCrashCoordinatorDisabledWithoutKey),
            ("crash/coordinator-records-report-id", testCrashCoordinatorRecordsReportId),
        ]
    }

    static func crashFixture() -> Data {
        guard let data = repoFile("mac-shell/Tests/Fixtures/crash/CoreVideoProShell-fixture.ips") else {
            expect(false, "the crash fixture is readable from the checkout"); return Data()
        }
        return data
    }

    /// Mutation: prefix-match process names (the -tests row goes red).
    static func testCrashNameMatch() {
        let table: [(String, String?)] = [
            ("corevideo-native-tests-2026-11-22-025659.ips", nil),
            ("CoreVideoProShell-2026-11-22-032121.ips", "CoreVideoProShell"),
            ("corevideo-zoom-engine-2026-11-22-010101.1.ips", "corevideo-zoom-engine"),
            ("CoreVideoProShell-2026-11-25-131615.000.ips", "CoreVideoProShell"),   // a real macOS suffix
            ("corevideo-native-2026-11-22-010101.ips", "corevideo-native"),
            ("Safari-2026-11-22-010101.ips", nil),
            ("CoreVideoProShell-2026-11-22-032121.ips.bak", nil),
            ("corevideo-native.ips", nil),
        ]
        for (name, expected) in table {
            expectEqual(CrashReportScanner.processName(forFile: name), expected, "process for \(name)")
        }
    }

    /// A fake file system: directory -> [file name: (date, length)]; a directory in `unreadable` throws.
    static func fakeCrashFS(_ tree: [String: [String: (Date, Int64)]], unreadable: Set<String> = []) -> CrashFS {
        CrashFS(
            list: { directory in
                if unreadable.contains(directory) { throw CocoaError(.fileReadNoPermission) }
                return Array((tree[directory] ?? [:]).keys)
            },
            attributes: { path in
                let directory = (path as NSString).deletingLastPathComponent
                return tree[directory]?[(path as NSString).lastPathComponent]
            })
    }

    /// Three directories, one unreadable: the others still yield, newer than the watermark only,
    /// oldest first, non-monitored names skipped.
    static func testCrashScanOrderAndWatermark() {
        let t = { (s: TimeInterval) in Date(timeIntervalSince1970: 1_795_341_600 + s) }
        let fs = fakeCrashFS([
            "/a": ["corevideo-native-2026-11-22-100500.ips": (t(500), 10), "CoreVideoProShell-2026-11-22-100000.ips": (t(0), 10)],
            "/b": ["corevideo-zoom-engine-2026-11-22-100200.ips": (t(200), 10), "corevideo-native-tests-2026-11-22-100300.ips": (t(300), 10)],
            "/c": ["corevideo-plugin-host-2026-11-22-100400.ips": (t(400), 10)],
        ], unreadable: ["/c"])
        let found = CrashReportScanner.scan(directories: ["/a", "/b", "/c", "/missing"], after: t(100), fs: fs)
        expectEqual(found.map(\.processName), ["corevideo-zoom-engine", "corevideo-native"],
                    "newer than the watermark, oldest first; the unreadable /c and the -tests file skipped")
        expectEqual(found.map(\.path), ["/b/corevideo-zoom-engine-2026-11-22-100200.ips", "/a/corevideo-native-2026-11-22-100500.ips"], "paths")
        let duplicate = CrashReportScanner.scan(directories: ["/a", "/a/"], after: t(-1), fs: fs)
        expectEqual(duplicate.count, 2, "a directory listed twice yields each report once")
    }

    /// Offer once: scan advances the watermark at once; a rescan finds nothing; two crashes of
    /// different processes before the next launch are both offered, oldest first.
    /// Mutation: advance the watermark only after Send.
    static func testCrashOfferOnce() {
        let dir = NSTemporaryDirectory() + "cvp-crash-offer-\(getpid())"
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let store = CrashReportWatermarkStore(path: dir + "/crash-watermark.json")
        let t = { (s: TimeInterval) in Date(timeIntervalSince1970: 1_795_341_600 + s) }
        var tree: [String: [String: (Date, Int64)]] = ["/r": ["CoreVideoProShell-2026-11-22-100000.ips": (t(0), 1)]]
        expectEqual(CrashReportOffer.scanAndAdvance(directories: ["/r"], store: store, now: t(60), fs: fakeCrashFS(tree)).count, 1, "offered")
        expectEqual(CrashReportOffer.scanAndAdvance(directories: ["/r"], store: store, now: t(120), fs: fakeCrashFS(tree)).count, 0,
                    "a relaunch (sent or dismissed) offers it no more")
        tree["/r"]?["corevideo-native-2026-11-22-100100.ips"] = (t(100), 1)
        tree["/r"]?["corevideo-zoom-engine-2026-11-22-100050.ips"] = (t(50), 1)
        let both = CrashReportOffer.scanAndAdvance(directories: ["/r"], store: store, now: t(200), fs: fakeCrashFS(tree))
        expectEqual(both.map(\.processName), ["corevideo-zoom-engine", "corevideo-native"], "two crashes: both, oldest first")
        expectEqual(store.load(now: t(300)).lastSeenUtc, t(100), "the watermark is the newest report")
    }

    static func testCrashWatermarkFirstRun() {
        let store = CrashReportWatermarkStore(path: NSTemporaryDirectory() + "cvp-no-such-\(getpid())/crash-watermark.json")
        let now = Date(timeIntervalSince1970: 1_795_341_600)
        expectEqual(store.load(now: now).lastSeenUtc, now.addingTimeInterval(-3 * 86400), "first run looks back 3 days")
        let dir = NSTemporaryDirectory() + "cvp-crash-corrupt-\(getpid())"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        FileManager.default.createFile(atPath: dir + "/crash-watermark.json", contents: Data("{nope".utf8))
        expectEqual(CrashReportWatermarkStore(path: dir + "/crash-watermark.json").load(now: now).lastSeenUtc,
                    now.addingTimeInterval(-3 * 86400), "a corrupt file is the fresh default")
        let record = CrashReportRecord(reportId: "r1", sentAtUtc: now, dumpFiles: ["a.ips"])
        try? CrashReportWatermarkStore(path: dir + "/w2.json").save(CrashReportWatermark(lastSeenUtc: now, reports: [record]))
        let text = String(decoding: FileManager.default.contents(atPath: dir + "/w2.json") ?? Data(), as: UTF8.self)
        expect(text.contains(#""lastSeenUtc" : "2026-11-22T10:00:00+00:00""#), "Windows' camelCase and DateTimeOffset form (\(text))")
        expectEqual(CrashReportWatermarkStore(path: dir + "/w2.json").load(now: now).reports, [record], "round trip")
    }

    /// None of the sentinels, vmSummary, trialInfo, coalitionName, parentProc, and no /Users/jdoe.
    /// Mutation: keep usedImages.path (the fixture's unredacted /Users/jdoe path goes red).
    static func testCrashSanitizeDropsIdentity() {
        guard let clean = IpsSanitizer.sanitize(crashFixture()) else { expect(false, "the fixture sanitizes"); return }
        let text = String(decoding: clean, as: UTF8.self)
        for banned in ["SENTINEL-", "crashReporterKey", "bootSessionUUID", "sleepWakeUUID", "incident", "userID",
                       "vmSummary", "trialInfo", "coalitionName", "parentProc", "modelCode", "procPath", "/Users/jdoe"] {
            expect(!text.contains(banned), "the sanitized report carries no '\(banned)'")
        }
        expect(!text.contains("\\/"), "slashes are not escaped, so the text checks above see real paths")
        expect(text.contains("/Users/USER/Secret/x.dylib"), "a home path inside a kept string is rewritten, not dropped")
        let body = json(String(text.split(separator: "\n", maxSplits: 1).last ?? ""))
        let asi = (body["asi"] as? [String: [String]])?.values.flatMap { $0 } ?? []
        expect(asi.contains("loaded from /Users/USER/Secret/x.dylib"), "the parsed asi message is rewritten (\(asi))")
        expect(!asi.joined().contains("jdoe"), "no planted user name in the parsed value")
        expect(text.hasPrefix("{") && text.contains("}\n{"), "header line, newline, body JSON")
    }

    /// The fault thread's frames and usedImages[0].uuid and base equal the fixture constants.
    /// Mutation: drop usedImages.uuid.
    static func testCrashSanitizeKeepsSymbolication() {
        guard let clean = IpsSanitizer.sanitize(crashFixture()) else { expect(false, "the fixture sanitizes"); return }
        let text = String(decoding: clean, as: UTF8.self)
        let parts = text.split(separator: "\n", maxSplits: 1)
        let body = parts.count == 2 ? json(String(parts[1])) : [:]
        let images = body["usedImages"] as? [[String: Any]] ?? []
        expectEqual(images.first?["uuid"] as? String, "11111111-2222-3333-4444-555555555555", "usedImages[0].uuid kept")
        expectEqual((images.first?["base"] as? NSNumber)?.int64Value, 4_294_967_296, "usedImages[0].base kept")
        expect(images.allSatisfy { $0["path"] == nil }, "no image path survives")
        let threads = body["threads"] as? [[String: Any]] ?? []
        let fault = body["faultingThread"] as? Int ?? -1
        let frames = (fault >= 0 && fault < threads.count) ? threads[fault]["frames"] as? [[String: Any]] ?? [] : []
        expectEqual(frames.first?["imageIndex"] as? Int, 0, "the fault thread's frame 0 points at image 0")
        expect(frames.count == 4, "the fault thread keeps its frames (\(frames.count))")
        expect(threads.first?["threadState"] != nil, "registers kept")
        let header = json(String(parts.first ?? ""))
        expectEqual(header["app_name"] as? String, "CoreVideoProShell", "header app_name kept")
        expect(header["incident_id"] == nil, "header incident_id dropped")
    }

    static func testCrashSanitizeGarbageIsNil() {
        expect(IpsSanitizer.sanitize(Data("not a report".utf8)) == nil, "plain text")
        expect(IpsSanitizer.sanitize(Data(#"{"a":1}"#.utf8)) == nil, "a header with no body")
        expect(IpsSanitizer.sanitize(Data([0xff, 0xfe, 0x00])) == nil, "binary")
    }

    // ── Task 11: archive, uploader, coordinator ─────────────────────────────

    static func crashTempDir(_ tag: String) -> String {
        let dir = NSTemporaryDirectory() + "cvp-crash-\(tag)-\(getpid())-\(UUID().uuidString.prefix(6))"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    /// Unzips `zip` and returns entry name -> bytes.
    static func unzipEntries(_ zip: String) -> [String: Data] {
        let out = crashTempDir("unzip")
        defer { try? FileManager.default.removeItem(atPath: out) }
        let ditto = Process()
        ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        ditto.arguments = ["-x", "-k", zip, out]
        try? ditto.run(); ditto.waitUntilExit()
        var entries: [String: Data] = [:]
        let enumerator = FileManager.default.enumerator(atPath: out)
        while let relative = enumerator?.nextObject() as? String {
            var isDirectory: ObjCBool = false
            let full = out + "/" + relative
            if FileManager.default.fileExists(atPath: full, isDirectory: &isDirectory), !isDirectory.boolValue {
                entries[relative] = FileManager.default.contents(atPath: full)
            }
        }
        return entries
    }

    static let plantedStreamKey = "live_SUPERSECRETSTREAMKEY_9f3a2b"

    /// THE PRIVACY GATE (coordinator, 2026-11-25; plan D-X2): a stream key planted in the last
    /// 2 MiB of a log never leaves the machine. Windows ships its tails raw. Red first: the first
    /// version of the builder copied the tail as Windows does and this case failed.
    /// Mutation: write the raw tail.
    static func testCrashArchiveLogTailRedacted() {
        let dir = crashTempDir("tail")
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let log = dir + "/CoreVideoPro-shell.log"
        var text = String(repeating: "filler line before the tail\n", count: 200)
        text += "stream: starting rtmp://a.rtmp.youtube.com/live2/\(plantedStreamKey)\n"
        text += "stream: key=\(plantedStreamKey) accepted\n"
        text += "last line of the log\n"
        FileManager.default.createFile(atPath: log, contents: Data(text.utf8))
        guard let result = try? CrashReportArchiveBuilder.build(
            zipPath: dir + "/out.zip", request: CrashArchiveRequest(reports: [], logFiles: [log], supportBundlePath: nil, appVersion: "1.2.3"),
            now: Date(timeIntervalSince1970: 1_795_341_600)) else { expect(false, "the archive builds"); return }
        let entries = unzipEntries(result.zipPath)
        let tail = String(decoding: entries["logs/CoreVideoPro-shell.log"] ?? Data(), as: UTF8.self)
        expect(!tail.isEmpty, "the log tail is in the zip")
        expect(!tail.contains(plantedStreamKey), "the planted stream key is NOT in the log entry")
        expect(tail.contains("last line of the log"), "the tail keeps the last line")
        let everything = entries.values.map { String(decoding: $0, as: UTF8.self) }.joined()
        expect(!everything.contains(plantedStreamKey), "the planted key is in NO entry of the zip")
        expect(!result.manifestText.contains(dir), "the manifest names files, not local paths")
    }

    static func stageReport(_ dir: String, name: String, data: Data, at date: Date) -> CrashReportFile {
        let path = dir + "/" + name
        FileManager.default.createFile(atPath: path, contents: data)
        return CrashReportFile(path: path, processName: CrashReportScanner.processName(forFile: name) ?? "?",
                               lastWrite: date, length: Int64(data.count))
    }

    /// Two reports, two logs, a bundle: the entries and the manifest's fixed lines.
    /// Mutation: --keepParent (entry names gain a folder).
    static func testCrashArchiveEntriesAndManifest() {
        let dir = crashTempDir("entries")
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let t0 = Date(timeIntervalSince1970: 1_795_341_600)
        let older = stageReport(dir, name: "corevideo-native-2026-11-22-100000.ips", data: crashFixture(), at: t0)
        let newer = stageReport(dir, name: "CoreVideoProShell-2026-11-22-100500.ips", data: crashFixture(), at: t0.addingTimeInterval(300))
        FileManager.default.createFile(atPath: dir + "/CoreVideoPro-shell.log", contents: Data("shell\n".utf8))
        FileManager.default.createFile(atPath: dir + "/CoreVideoPro-core.log", contents: Data("core\n".utf8))
        FileManager.default.createFile(atPath: dir + "/bundle.json", contents: Data(#"{"redacted":true}"#.utf8))
        guard let result = try? CrashReportArchiveBuilder.build(
            zipPath: dir + "/out.zip",
            request: CrashArchiveRequest(reports: [newer, older], logFiles: [dir + "/CoreVideoPro-shell.log", dir + "/CoreVideoPro-core.log", dir + "/gone.log"],
                                         supportBundlePath: dir + "/bundle.json", appVersion: "1.2.3"),
            now: t0) else { expect(false, "the archive builds"); return }
        expectEqual(result.included, ["manifest.txt", "dumps/corevideo-native-2026-11-22-100000.ips", "dumps/CoreVideoProShell-2026-11-22-100500.ips",
                                      "logs/CoreVideoPro-shell.log", "logs/CoreVideoPro-core.log", "support-bundle.json"],
                    "entries: manifest, reports oldest first, logs, bundle")
        expectEqual(Set(unzipEntries(result.zipPath).keys), Set(result.included), "the zip holds exactly those entries, at its root")
        let lines = result.manifestText.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        expectEqual(Array(lines.prefix(5)), ["CoreVideo Pro crash report", "Created: 2026-11-22T10:00:00+00:00", "App version: 1.2.3",
                                             "Content budget: 25165824 bytes (uncompressed)", ""], "the manifest header")
        expect(lines.contains("Skipped: gone.log — not found"), "a missing log is a skip line naming the file only")
        expect(lines.contains("Included: logs/CoreVideoPro-shell.log (6 bytes, complete, redacted)"), "a log line says redacted")
        let report = unzipEntries(result.zipPath)["dumps/corevideo-native-2026-11-22-100000.ips"] ?? Data()
        expect(!String(decoding: report, as: UTF8.self).contains("SENTINEL-"), "the zipped report is the SANITIZED copy")
    }

    /// A synthetic report whose sanitized copy is about `kib` KiB (padding in a kept field).
    static func paddedReport(kib: Int) -> Data {
        let header = #"{"app_name":"CoreVideoProShell","name":"CoreVideoProShell","bug_type":"309"}"#
        let body = #"{"procName":"CoreVideoProShell","asi":{"pad":[""# + String(repeating: "x", count: kib * 1024) + #""]}}"#
        return Data((header + "\n" + body).utf8)
    }

    /// Budget 1 MiB, reports of ~700, ~500, ~200 KiB: 700 and 200 in, 500 skipped with Windows' sentence.
    /// Mutation: smallest-first fit.
    static func testCrashArchiveBudgetLargestFirst() {
        let dir = crashTempDir("budget")
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let t0 = Date(timeIntervalSince1970: 1_795_341_600)
        let big = stageReport(dir, name: "corevideo-native-2026-11-22-100000.ips", data: paddedReport(kib: 700), at: t0)
        let mid = stageReport(dir, name: "corevideo-zoom-engine-2026-11-22-100100.ips", data: paddedReport(kib: 500), at: t0.addingTimeInterval(60))
        let small = stageReport(dir, name: "CoreVideoProShell-2026-11-22-100200.ips", data: paddedReport(kib: 200), at: t0.addingTimeInterval(120))
        guard let result = try? CrashReportArchiveBuilder.build(
            zipPath: dir + "/out.zip",
            request: CrashArchiveRequest(reports: [big, mid, small], logFiles: [], supportBundlePath: nil, maxTotalBytes: 1 << 20, appVersion: "1.2.3"),
            now: t0) else { expect(false, "the archive builds"); return }
        expectEqual(result.included.filter { $0.hasPrefix("dumps/") },
                    ["dumps/corevideo-native-2026-11-22-100000.ips", "dumps/CoreVideoProShell-2026-11-22-100200.ips"], "700 and 200 fit, oldest first")
        expect(result.skipped.contains { $0.hasPrefix("corevideo-zoom-engine-2026-11-22-100100.ips — ") && $0.contains("exceeds the remaining budget") },
               "500 is skipped with Windows' sentence (\(result.skipped))")
    }

    static func testCrashArchiveSingleOversized() {
        let dir = crashTempDir("oversized")
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let t0 = Date(timeIntervalSince1970: 1_795_341_600)
        let huge = stageReport(dir, name: "corevideo-native-2026-11-22-100000.ips", data: paddedReport(kib: 300), at: t0)
        guard let result = try? CrashReportArchiveBuilder.build(
            zipPath: dir + "/out.zip",
            request: CrashArchiveRequest(reports: [huge], logFiles: [], supportBundlePath: nil, maxTotalBytes: 200 * 1024, appVersion: "1.2.3"),
            now: t0) else { expect(false, "the archive builds"); return }
        expect(!result.included.contains { $0.hasPrefix("dumps/") }, "an oversized report is not included")
        expect(result.manifestText.contains("Skipped: corevideo-native-2026-11-22-100000.ips — "), "and the manifest says so")
    }

    /// POST /v1/crashes, Bearer, application/zip, the three X-CoreVideo headers, body = zip bytes.
    @MainActor
    static func testCrashUploadRequestShape() {
        let dir = crashTempDir("upload")
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let zip = dir + "/r.zip"
        let bytes = Data((0..<4096).map { UInt8($0 % 251) })
        FileManager.default.createFile(atPath: zip, contents: bytes)
        RecordingURLProtocol.requests = []
        RecordingURLProtocol.mode = .accept
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [RecordingURLProtocol.self]
        let box = Box<TelemetrySendResult>()
        Task {
            box.value = await CrashReportUploader.upload(
                endpoint: URL(string: "https://t.example/base")!, apiKey: "crash-key-sentinel", version: "1.2.3",
                machineClass: "mac-arm64-cpu12-ram36gb", reason: "ips: corevideo-native", zipPath: zip,
                transport: CrashReportUploader.urlSessionTransport(base: configuration))
        }
        spin(until: { box.value != nil }, limit: 10)
        expectEqual(box.value?.status, .accepted, "accepted")
        guard let (request, body) = RecordingURLProtocol.requests.first else { expect(false, "a request was made"); return }
        expectEqual(request.httpMethod, "POST", "POST")
        expectEqual(request.url?.absoluteString, "https://t.example/v1/crashes", "/v1/crashes")
        expectEqual(request.value(forHTTPHeaderField: "Authorization"), "Bearer crash-key-sentinel", "Bearer")
        expectEqual(request.value(forHTTPHeaderField: "Content-Type"), "application/zip", "application/zip")
        expectEqual(request.value(forHTTPHeaderField: "X-CoreVideo-Version"), "1.2.3", "version header")
        expectEqual(request.value(forHTTPHeaderField: "X-CoreVideo-Machine-Class"), "mac-arm64-cpu12-ram36gb", "machine class header")
        expectEqual(request.value(forHTTPHeaderField: "X-CoreVideo-Reason"), "ips: corevideo-native", "reason header (ips:, not wer-dump)")
        expectEqual(body, bytes, "the body is the zip, byte for byte")
    }

    @MainActor
    static func testCrashUploadOutcomes() {
        let now = Date(timeIntervalSince1970: 1_795_341_600)
        let tooBig = CrashReportUploader.classify(status: 413, headers: [:], body: Data("too large".utf8), now: now)
        expectEqual(tooBig.status, .rejected, "413 rejected (no retry)")
        expectEqual(tooBig.message, "Server rejected the report (HTTP 413): too large", "Windows' crash wording")
        let busy = CrashReportUploader.classify(status: 429, headers: ["Retry-After": "30"], body: Data(), now: now)
        expectEqual(busy.status, .retryLater, "429 retry"); expectEqual(busy.retryAfter, 30, "Retry-After")
        let box = Box<TelemetrySendResult>()
        Task {
            box.value = await CrashReportUploader.upload(endpoint: URL(string: "https://t.example")!, apiKey: "k", version: nil, machineClass: nil,
                                                         reason: nil, zipPath: "/nonexistent/cvp-crash.zip",
                                                         transport: { _, _, _, _, _ in TelemetrySendResult(status: .accepted) })
        }
        spin(until: { box.value != nil }, limit: 3)
        expectEqual(box.value?.status, .rejected, "a missing zip is rejected")
        expect(box.value?.message?.hasPrefix("Could not read the report archive: ") == true, "Windows' wording (\(box.value?.message ?? ""))")
    }

    static func testCrashCoordinatorDisabledWithoutKey() {
        let store = CrashReportWatermarkStore(path: NSTemporaryDirectory() + "cvp-nokey/w.json")
        let none = MainActor.assumeIsolated {
            CrashReportCoordinator.make(config: .resolve(endpointOverride: nil, apiKey: nil), store: store, directories: [], logFiles: [],
                                        bundleDirectory: "/tmp", version: "1", machineClass: "m", transport: { _, _, _, _, _ in .init(status: .accepted) })
        }
        expect(none == nil, "no key: no coordinator (no scan, no bar)")
        let blank = MainActor.assumeIsolated {
            CrashReportCoordinator.make(config: .resolve(endpointOverride: nil, apiKey: "  "), store: store, directories: [], logFiles: [],
                                        bundleDirectory: "/tmp", version: "1", machineClass: "m", transport: { _, _, _, _, _ in .init(status: .accepted) })
        }
        expect(blank == nil, "a blank key: no coordinator")
    }

    /// Accepted with an id: the watermark records it; RetryLater records nothing.
    /// Mutation: record a report on RetryLater.
    @MainActor
    static func testCrashCoordinatorRecordsReportId() {
        let dir = crashTempDir("coord")
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let t0 = Date(timeIntervalSince1970: 1_795_341_600)
        let report = stageReport(dir, name: "corevideo-native-2026-11-22-100000.ips", data: crashFixture(), at: t0)
        let store = CrashReportWatermarkStore(path: dir + "/crash-watermark.json")
        var answer = TelemetrySendResult(status: .retryLater, httpStatus: 503, message: "Server busy or unavailable (HTTP 503): later")
        func coordinator() -> CrashReportCoordinator? {
            CrashReportCoordinator.make(config: .resolve(endpointOverride: "https://t.example", apiKey: "k"), store: store,
                                        directories: [dir], logFiles: [], bundleDirectory: dir + "/bundles", version: "1.2.3",
                                        machineClass: "mac-arm64", transport: { _, _, _, _, _ in answer }, now: { t0 }, log: { _ in })
        }
        let box = Box<CrashSendOutcome>()
        Task { box.value = await coordinator()?.send([report], writeSupportBundle: { _ in false }) }
        spin(until: { box.value != nil }, limit: 10)
        expectEqual(box.value?.canRetry, true, "503: retryable")
        expect(box.value?.message.hasSuffix("— nothing was lost.") == true, "Windows' saved-copy wording (\(box.value?.message ?? ""))")
        expectEqual(store.load(now: t0).reports.count, 0, "RetryLater records nothing")
        answer = TelemetrySendResult(status: .accepted, reportId: "rep-42", httpStatus: 202)
        let ok = Box<CrashSendOutcome>()
        Task { ok.value = await coordinator()?.send([report], writeSupportBundle: { _ in false }) }
        spin(until: { ok.value != nil }, limit: 10)
        expectEqual(ok.value?.success, true, "accepted")
        expect(ok.value?.message.hasPrefix("Thanks — the report was sent. Reference ID: rep-42. A copy stays at ") == true, "Windows' thanks wording")
        expectEqual(store.load(now: t0).reports.map(\.reportId), ["rep-42"], "the accepted id is recorded")
        expectEqual(store.load(now: t0).reports.first?.dumpFiles, ["corevideo-native-2026-11-22-100000.ips"], "with file names, not paths")
        expect(FileManager.default.fileExists(atPath: ok.value?.zipPath ?? "/nope"), "the zip stays after upload")
    }

    /// A key cut at the tail's seek point: the fragment "STREAMKEY_9f3a2b" has no rtmp:// prefix
    /// and is too short for the high-entropy rule, so only dropping the partial first line keeps
    /// it home. Fixture invariant: the tail starts exactly at "STREAMKEY" (computed from the text).
    /// Mutation: keep the partial first line.
    static func testCrashArchiveLogTailCutMidSecret() {
        let dir = crashTempDir("cut")
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let log = dir + "/CoreVideoPro-core.log"
        var text = String(repeating: "early line\n", count: 50)
        text += "sender: rtmp://a.rtmp.youtube.com/live2/\(plantedStreamKey) opened\n"
        text += String(repeating: "later line\n", count: 20) + "final line\n"
        let bytes = Data(text.utf8)
        guard let cut = bytes.range(of: Data("STREAMKEY_9f3a2b".utf8))?.lowerBound else { expect(false, "fixture"); return }
        let tailBytes = Int64(bytes.count - cut)
        FileManager.default.createFile(atPath: log, contents: bytes)
        guard let result = try? CrashReportArchiveBuilder.build(
            zipPath: dir + "/out.zip",
            request: CrashArchiveRequest(reports: [], logFiles: [log], supportBundlePath: nil, logTailBytes: tailBytes, appVersion: "1"),
            now: Date(timeIntervalSince1970: 1_795_341_600)) else { expect(false, "the archive builds"); return }
        let tail = String(decoding: unzipEntries(result.zipPath)["logs/CoreVideoPro-core.log"] ?? Data(), as: UTF8.self)
        expect(String(decoding: (try? CrashReportArchiveBuilder.readTail(log, maxBytes: tailBytes).0) ?? Data(), as: UTF8.self)
                .hasPrefix("STREAMKEY_9f3a2b"), "fixture: the raw tail really starts mid-key")
        expect(!tail.contains("STREAMKEY_9f3a2b") && !tail.contains("9f3a2b"), "no fragment of the cut key reaches the zip")
        expect(tail.hasPrefix("later line"), "the partial first line is dropped; whole lines follow")
        expect(tail.contains("final line"), "the tail keeps the last line")
        expect(result.manifestText.contains("Included: logs/CoreVideoPro-core.log (last "), "the manifest says it is a tail")
    }
}
