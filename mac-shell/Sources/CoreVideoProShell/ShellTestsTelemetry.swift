// Phase 4 R4 telemetry (docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md,
// Batch T). Golden data comes from contracts/telemetry.fixtures.json, written by the WINDOWS
// MediaCore tests (TelemetryFixtureExportTests) from the real Windows telemetry code.
// Each test names the mutation it must turn red (plan-authoring rule 7).

import Foundation

extension ShellTests {

    @MainActor
    static var telemetryCases: [(String, () -> Void)] {
        [
            ("telemetry/payload-matches-windows", testTelemetryPayloadMatchesWindows),
            ("telemetry/ram-bands", testTelemetryRamBands),
            ("telemetry/ram-rounding-half-even", testTelemetryRamRoundingHalfEven),
            ("telemetry/machine-label", testTelemetryMachineLabel),
            ("telemetry/config-matches-windows", testTelemetryConfigMatchesWindows),
            ("telemetry/consent-defaults-off", testTelemetryConsentDefaultsOff),
            ("telemetry/consent-file-shape", testTelemetryConsentFileShape),
            ("telemetry/revoke-lands-when-unreadable", testTelemetryRevokeLandsWhenUnreadable),
            ("telemetry/marksent-never-revokes", testTelemetryMarkSentNeverRevokes),
            ("telemetry/crash-count", testTelemetryCrashCount),
        ]
    }

    static func telemetryFixture() -> [String: Any] {
        guard let fixture = repoJSON("contracts/telemetry.fixtures.json") else {
            expect(false, "contracts/telemetry.fixtures.json is readable from the checkout")
            return [:]
        }
        return fixture
    }

    /// The ordered key paths of a JSON object text ("a", "a.b", ...), via the order-keeping parser.
    static func orderedKeys(_ text: String) -> [String] {
        func walk(_ value: JSONValue, _ prefix: String) -> [String] {
            guard case .object(let members) = value else { return [] }
            return members.flatMap { name, member in [prefix + name] + walk(member, prefix + name + ".") }
        }
        return (try? JSONValue.parseDocument(text)).map { walk($0, "") } ?? []
    }

    /// Each fixture input: the Mac wire JSON parses to Windows' object, with Windows' key ORDER at
    /// every level, and carries none of the secrets in the core snapshot.
    /// Mutations: emit gpuTier null (engine-off input); read streaming from intent instead of the
    /// sender status (ndi-only input); swap two payload keys (order).
    static func testTelemetryPayloadMatchesWindows() {
        let payloads = telemetryFixture()["payloads"] as? [[String: Any]] ?? []
        expectEqual(payloads.count, 3, "three payload inputs")
        for row in payloads {
            let name = row["name"] as? String ?? "?"
            let snapshot = (row["response"] as? String).flatMap { json($0)["snapshot"] as? JSONObject }
            let machine = row["machine"] as? [String: Any] ?? [:]
            let payload = TelemetryPayloadBuilder.build(
                name: TelemetryEventNames.sessionEnd, version: row["version"] as? String ?? "",
                sessionLengthSeconds: Int64(row["sessionLengthSeconds"] as? Int ?? 0), snapshot: snapshot,
                crashCountSinceLastSend: row["crashCount"] as? Int ?? 0,
                machine: TelemetryMachineClass(label: machine["label"] as? String ?? "", cpuCores: machine["cpuCores"] as? Int ?? 0,
                                               ramBand: machine["ramBand"] as? String ?? "", gpuTier: machine["gpuTier"] as? String))
            let mac = TelemetryPayloadBuilder.serializeWire(payload)
            let windows = row["wire"] as? String ?? ""
            expect(NSDictionary(dictionary: json(mac)).isEqual(to: json(windows)), "\(name): the Mac wire equals Windows' (\(mac))")
            expectEqual(orderedKeys(mac), orderedKeys(windows), "\(name): key order equals Windows' at every level")
            expectEqual(orderedKeys(TelemetryPayloadBuilder.serializePreview(payload)), orderedKeys(windows),
                        "\(name): the preview carries the same members in the same order")
            for secret in ["live_SUPERSECRETSTREAMKEY_9f3a2b", "srt-passphrase", "Alice Q. Attendee", "Confidential",
                           "pid-secret", "rtmp://", "zoom.us", "/Users/"] {
                expect(!mac.contains(secret), "\(name): '\(secret)' never leaves the machine")
            }
        }
    }

    /// Mutation: shift a band edge.
    static func testTelemetryRamBands() {
        for row in telemetryFixture()["ramBands"] as? [[String: Any]] ?? [] {
            let gb = row["gb"] as? Int ?? -1
            expectEqual(MachineClassProbe.ramBand(gb), row["band"] as? String ?? "?", "Windows band for \(gb) GB")
        }
    }

    /// Mutation: round half away from zero (8.5 GiB -> 9).
    static func testTelemetryRamRoundingHalfEven() {
        for row in telemetryFixture()["ramRounding"] as? [[String: Any]] ?? [] {
            let bytes = UInt64(row["bytes"] as? Int ?? 0)
            expectEqual(MachineClassProbe.ramGigabytes(bytes), row["gb"] as? Int ?? -1, "Math.Round of \(bytes) bytes")
        }
    }

    static func testTelemetryMachineLabel() {
        expectEqual(MachineClassProbe.describe(cpu: 12, physicalMemoryBytes: 36 << 30), "mac-arm64-cpu12-ram36gb", "label")
        expectEqual(MachineClassProbe.describe(cpu: 12, physicalMemoryBytes: 0), "mac-arm64", "fallback label")
        expectEqual(MachineClassProbe.gpuTier(brandString: "Apple M3 Pro"), "apple-m3-pro", "M3 Pro")
        expectEqual(MachineClassProbe.gpuTier(brandString: "Apple M2"), "apple-m2", "M2")
        expectEqual(MachineClassProbe.gpuTier(brandString: "Apple M4 Max"), "apple-m4-max", "M4 Max")
        expectEqual(MachineClassProbe.gpuTier(brandString: "Intel(R) Core(TM) i9"), nil, "Intel")
        expectEqual(MachineClassProbe.gpuTier(brandString: nil), nil, "no brand")
        let probed = MachineClassProbe.probe()
        expect(probed.label.hasPrefix("mac-arm64"), "the live probe has the Mac shape (\(probed.label))")
        expect(!probed.label.contains(Host.current().localizedName ?? "\u{0}"), "no machine name in the label")
    }

    /// Mutation: accept ftp, or fail to trim the override.
    static func testTelemetryConfigMatchesWindows() {
        for row in telemetryFixture()["config"] as? [[String: Any]] ?? [] {
            let config = TelemetryConfig.resolve(endpointOverride: row["endpointOverride"] as? String, apiKey: row["apiKey"] as? String)
            let label = "config(\(row["endpointOverride"] as? String ?? "nil"))"
            expectEqual(config.endpoint, row["endpoint"] as? String ?? "?", "\(label): endpoint")
            expectEqual(config.enabled, row["enabled"] as? Bool ?? !config.enabled, "\(label): enabled")
            expectEqual(config.endpointHost, row["endpointHost"] as? String ?? "?", "\(label): endpointHost")
        }
        let env = TelemetryConfig.fromEnvironment(["COREVIDEO_TELEMETRY_API_KEY": "  env-key-sentinel  "])
        expect(env.enabled, "the key comes from COREVIDEO_TELEMETRY_API_KEY")
        expect(!TelemetryConfig.fromEnvironment([:]).enabled, "no key: quietly disabled")
        expect(!"\(env)".contains("env-key-sentinel"), "the key never appears in a description or log")
    }

    /// An in-memory file seam: `files[path]`, plus a read that can be forced to fail.
    final class FakeTelemetryFiles {
        var files: [String: Data] = [:]
        var readError: Error?
        var reads = 0, writes = 0, sleeps = 0
        var io: TelemetryFileIO {
            TelemetryFileIO(
                read: { [unowned self] path in
                    reads += 1
                    if let readError { throw readError }
                    return files[path]
                },
                write: { [unowned self] data, path in writes += 1; files[path] = data },
                sleep: { [unowned self] _ in sleeps += 1 })
        }
    }

    /// Absent file, empty file, {}, {"enabled":"yes"}, a directory, and a file locked for every
    /// attempt: all OFF. Mutation: a corrupt file defaults to the last in-memory value.
    static func testTelemetryConsentDefaultsOff() {
        let path = "/fake/telemetry-consent.json"
        for (label, data) in [("no file", nil), ("empty file", Data()), ("{}", Data("{}".utf8)),
                              ("enabled as a string", Data(#"{"enabled":"yes"}"#.utf8))] as [(String, Data?)] {
            let fake = FakeTelemetryFiles()
            fake.files[path] = data
            expectEqual(TelemetryConsentStore(path: path, fileIO: fake.io).load().enabled, false, "\(label): OFF")
        }
        let locked = FakeTelemetryFiles()
        locked.files[path] = Data(#"{"enabled":true}"#.utf8)
        locked.readError = TelemetryFileBusy()
        expectEqual(TelemetryConsentStore(path: path, fileIO: locked.io).load().enabled, false, "locked every attempt: OFF")
        expectEqual(locked.reads, 20, "bounded retry: 20 attempts")
        expectEqual(locked.sleeps, 19, "the backoff comes from the seam, so the test never waits")
        // A real directory at the path (the real seam).
        let dir = NSTemporaryDirectory() + "cvp-consent-dir-\(getpid())"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        expectEqual(TelemetryConsentStore(path: dir).load().enabled, false, "a directory at the path: OFF")
        let enabled = FakeTelemetryFiles()
        enabled.files[path] = Data(#"{"enabled":true}"#.utf8)
        let sameStore = TelemetryConsentStore(path: path, fileIO: enabled.io)
        expectEqual(sameStore.load().enabled, true, "a valid opt-in reads ON")
        enabled.files[path] = Data("{not json".utf8)
        expectEqual(sameStore.load().enabled, false, "the same store: a file that turns corrupt reads OFF, never the last value")
        enabled.files[path] = Data(#"{"enabled":true}"#.utf8)
        enabled.readError = TelemetryFileBusy()
        expectEqual(sameStore.load().enabled, false, "the same store: a file that turns locked reads OFF")
    }

    /// The Mac writes Windows' consent JSON (consentFiles), object-equal and text-equal.
    static func testTelemetryConsentFileShape() {
        let rows = telemetryFixture()["consentFiles"] as? [[String: Any]] ?? []
        let states: [String: TelemetryConsentState] = [
            "enabled-with-last-sent": TelemetryConsentState(enabled: true, lastSentUtc: Date(timeIntervalSince1970: 1_795_341_600)),
            "default": TelemetryConsentState(),
        ]
        expectEqual(rows.count, 2, "two consent files")
        for row in rows {
            let name = row["name"] as? String ?? "?"
            guard let state = states[name] else { expect(false, "\(name): a known fixture"); continue }
            let mac = String(decoding: TelemetryConsentStore.encode(state), as: UTF8.self)
            let windows = row["json"] as? String ?? ""
            expect(NSDictionary(dictionary: json(mac)).isEqual(to: json(windows)), "\(name): object-equal to Windows' file")
            expectEqual(mac, windows, "\(name): text-equal to Windows' file")
            expectEqual(try? TelemetryConsentStore.decode(Data(windows.utf8)), state, "\(name): the Mac reads Windows' file")
        }
        expectEqual(try? TelemetryConsentStore.decode(Data(#"{"enabled":true,"lastSentUtc":"2026-11-22T10:00:00Z"}"#.utf8)),
                    TelemetryConsentState(enabled: true, lastSentUtc: Date(timeIntervalSince1970: 1_795_341_600)), "a Z timestamp reads too")
    }

    /// Read throws every attempt: setEnabled(false) writes the disabled default; setEnabled(true)
    /// throws and writes nothing. Mutation: let a revoke fail on an unreadable file.
    static func testTelemetryRevokeLandsWhenUnreadable() {
        let path = "/fake/telemetry-consent.json"
        let fake = FakeTelemetryFiles()
        fake.files[path] = Data(#"{"enabled":true}"#.utf8)
        fake.readError = CocoaError(.fileReadNoPermission)
        let store = TelemetryConsentStore(path: path, fileIO: fake.io)
        expect((try? store.setEnabled(false)) != nil, "revoke succeeds on an unreadable file")
        fake.readError = nil
        expectEqual(store.load().enabled, false, "the file now says disabled")
        fake.readError = CocoaError(.fileReadNoPermission)
        let writes = fake.writes
        expect((try? store.setEnabled(true)) == nil, "a grant on an unreadable file throws")
        expectEqual(fake.writes, writes, "and writes nothing")
    }

    /// Read throws: markSent throws and the file still says enabled. Mutation: markSent rewrites the
    /// default on an unreadable file.
    static func testTelemetryMarkSentNeverRevokes() {
        let path = "/fake/telemetry-consent.json"
        let fake = FakeTelemetryFiles()
        fake.files[path] = Data(#"{"enabled":true}"#.utf8)
        fake.readError = CocoaError(.fileReadNoPermission)
        let store = TelemetryConsentStore(path: path, fileIO: fake.io)
        expect((try? store.markSent(Date(timeIntervalSince1970: 1_795_341_600))) == nil, "markSent throws on an unreadable file")
        fake.readError = nil
        expectEqual(store.load().enabled, true, "the file still says enabled")
        expect((try? store.markSent(Date(timeIntervalSince1970: 1_795_341_600))) != nil, "markSent on a readable file")
        expectEqual(store.load(), TelemetryConsentState(enabled: true, lastSentUtc: Date(timeIntervalSince1970: 1_795_341_600)),
                    "markSent keeps consent and records the time")
    }

    static func testTelemetryCrashCount() {
        let t1 = Date(timeIntervalSince1970: 1000), t2 = Date(timeIntervalSince1970: 2000), t3 = Date(timeIntervalSince1970: 3000)
        let reports = [t1, t2, t3].enumerated().map { CrashReportRecord(reportId: "r\($0.offset)", sentAtUtc: $0.element, dumpFiles: []) }
        expectEqual(TelemetryCrashCount.countSince(reports: reports, lastSentUtc: Date(timeIntervalSince1970: 1500)), 2, "after t1")
        expectEqual(TelemetryCrashCount.countSince(reports: reports, lastSentUtc: nil), 3, "never sent: all")
    }
}
