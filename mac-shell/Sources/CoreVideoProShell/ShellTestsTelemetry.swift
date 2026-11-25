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
            ("telemetry/client-classify", testTelemetryClientClassify),
            ("telemetry/client-request-shape", testTelemetryClientRequestShape),
            ("telemetry/no-send-without-consent", testTelemetryNoSendWithoutConsent),
            ("telemetry/no-send-without-key", testTelemetryNoSendWithoutKey),
            ("telemetry/session-end-once", testTelemetrySessionEndOnce),
            ("telemetry/heartbeat-cadence", testTelemetryHeartbeatCadence),
            ("telemetry/audit-log-precedes-send", testTelemetryAuditLogPrecedesSend),
            ("telemetry/marks-sent-only-on-accept", testTelemetryMarksSentOnlyOnAccept),
            ("quit/idle-waits-for-session-end", testQuitIdleWaitsForSessionEnd),
            ("quit/idle-unchanged-without-consent", testQuitIdleUnchangedWithoutConsent),
            ("quit/idle-replies-when-send-finishes", testQuitIdleRepliesWhenSendFinishes),
            ("quit/idle-waits-for-comms-and-session-end", testQuitIdleWaitsForCommsAndSessionEnd),
            ("telemetry/card-copy", testTelemetryCardCopy),
            ("telemetry/toggle-persists-not-on-load", testTelemetryTogglePersistsNotOnLoad),
            ("telemetry/preview-without-consent", testTelemetryPreviewWithoutConsent),
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
                read: { path in
                    self.reads += 1
                    if let error = self.readError { throw error }
                    return self.files[path]
                },
                write: { data, path in self.writes += 1; self.files[path] = data },
                sleep: { _ in self.sleeps += 1 })
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

    // ── Task 8: event client, service, quit ──────────────────────────────────

    /// A manual clock: `advance(by:)` fires due timers in time order, on the main actor.
    final class ManualTelemetryClock: TelemetryClock, @unchecked Sendable {
        final class Token: TelemetryTimerToken {
            var cancelled = false
            func cancel() { cancelled = true }
        }
        var now = Date(timeIntervalSince1970: 1_795_341_600)
        private var timers: [(at: Date, token: Token, fire: @MainActor () -> Void)] = []
        func schedule(after: TimeInterval, _ fire: @escaping @MainActor () -> Void) -> TelemetryTimerToken {
            let token = Token()
            timers.append((now.addingTimeInterval(after), token, fire))
            return token
        }
        var pending: [Date] { timers.filter { !$0.token.cancelled }.map(\.at).sorted() }
        /// Moves time forward, firing every due timer (and any it schedules) in order, and lets
        /// the main actor run the Tasks they start. Call from the main thread.
        func advance(by seconds: TimeInterval) {
            let target = now.addingTimeInterval(seconds)
            ShellTests.drainMainActor()
            while let next = timers.enumerated().filter({ !$0.element.token.cancelled && $0.element.at <= target })
                .min(by: { $0.element.at < $1.element.at }) {
                timers.remove(at: next.offset)
                now = next.element.at
                let fire = next.element.fire
                MainActor.assumeIsolated { fire() }
                ShellTests.drainMainActor()
            }
            now = target
            ShellTests.drainMainActor()
        }
    }

    /// Lets queued main-actor Tasks run (no wall-clock wait beyond a few run-loop turns).
    static func drainMainActor() {
        for _ in 0..<20 { RunLoop.main.run(until: Date().addingTimeInterval(0.002)) }
    }

    /// A recorded call to a fake transport.
    final class TransportLog: @unchecked Sendable {
        var calls: [(url: URL, key: String, body: String, timeout: TimeInterval)] = []
    }

    static func fakeTransport(_ log: TransportLog, _ result: TelemetrySendResult) -> TelemetryTransport {
        { url, key, body, timeout in log.calls.append((url, key, body, timeout)); return result }
    }

    @MainActor
    static func telemetryHarness(key: String? = "k", consentOn: Bool = true, transport: @escaping TelemetryTransport)
        -> (TelemetryService, ManualTelemetryClock, FakeTelemetryFiles, [String]) {
        let files = FakeTelemetryFiles()
        let path = "/fake/telemetry-consent.json"
        if consentOn { files.files[path] = Data(#"{"enabled":true}"#.utf8) }
        let clock = ManualTelemetryClock()
        let box = LinesBox()
        let service = TelemetryService(
            config: .resolve(endpointOverride: "https://t.example/base", apiKey: key),
            consent: TelemetryConsentStore(path: path, fileIO: files.io),
            snapshot: { nil }, crashReports: { [] }, version: { "1.2.3" },
            machine: { TelemetryMachineClass(label: "mac-arm64-cpu12-ram36gb", cpuCores: 12, ramBand: "32-64GB", gpuTier: nil) },
            transport: transport, clock: clock, log: { box.lines.append($0) })
        harnessLines = box
        return (service, clock, files, [])
    }

    final class LinesBox { var lines: [String] = [] }
    nonisolated(unsafe) static var harnessLines = LinesBox()

    /// Windows TelemetryEventClient's classes. Mutation: honour 400 as RetryLater.
    static func testTelemetryClientClassify() {
        let now = Date(timeIntervalSince1970: 1_795_341_600)
        let ok = TelemetryEventClient.classify(status: 202, headers: [:], body: Data(#"{"reportId":"r1"}"#.utf8), now: now)
        expectEqual(ok.status, .accepted, "202 accepted"); expectEqual(ok.reportId, "r1", "reportId read")
        let busy = TelemetryEventClient.classify(status: 429, headers: ["Retry-After": "30"], body: Data(), now: now)
        expectEqual(busy.status, .retryLater, "429 retry later"); expectEqual(busy.retryAfter, 30, "Retry-After delta")
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX"); formatter.timeZone = TimeZone(identifier: "GMT")
        formatter.dateFormat = "EEE, dd MMM yyyy HH:mm:ss 'GMT'"
        let date = formatter.string(from: now.addingTimeInterval(90))
        let unavailable = TelemetryEventClient.classify(status: 503, headers: ["retry-after": date], body: Data(), now: now)
        expectEqual(unavailable.status, .retryLater, "503 retry later")
        expectEqual(unavailable.retryAfter, 90, "an HTTP-date Retry-After is relative to the injected now (\(date))")
        let rejected = TelemetryEventClient.classify(status: 400, headers: [:], body: Data(String(repeating: "x", count: 400).utf8), now: now)
        expectEqual(rejected.status, .rejected, "400 rejected")
        expectEqual(rejected.message, "Server rejected the event (HTTP 400): " + String(repeating: "x", count: 300), "body cut to 300")
    }

    /// A URLProtocol that records the request and answers 202.
    final class RecordingURLProtocol: URLProtocol {
        nonisolated(unsafe) static var requests: [(URLRequest, Data)] = []
        override class func canInit(with request: URLRequest) -> Bool { true }
        override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
        override func startLoading() {
            var body = request.httpBody ?? Data()
            if body.isEmpty, let stream = request.httpBodyStream {
                stream.open(); defer { stream.close() }
                var buffer = [UInt8](repeating: 0, count: 4096)
                while stream.hasBytesAvailable { let n = stream.read(&buffer, maxLength: buffer.count); if n <= 0 { break }; body.append(buffer, count: n) }
            }
            Self.requests.append((request, body))
            let response = HTTPURLResponse(url: request.url!, statusCode: 202, httpVersion: "HTTP/1.1", headerFields: [:])!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: Data(#"{"reportId":"stub-1"}"#.utf8))
            client?.urlProtocolDidFinishLoading(self)
        }
        override func stopLoading() {}
    }

    /// The real transport against an in-process stub (no network): POST /v1/events (a base path
    /// is replaced, as Windows' new Uri(endpoint, "/v1/events")), Bearer, JSON, body = wire.
    @MainActor
    static func testTelemetryClientRequestShape() {
        RecordingURLProtocol.requests = []
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [RecordingURLProtocol.self]
        let transport = TelemetryEventClient.urlSessionTransport(URLSession(configuration: configuration))
        let box = Box<TelemetrySendResult>()
        Task { box.value = await transport(URL(string: "https://t.example/base")!, "k", #"{"name":"heartbeat"}"#, 5) }
        spin(until: { box.value != nil }, limit: 5)
        expectEqual(box.value?.status, .accepted, "stub answered 202")
        expectEqual(box.value?.reportId, "stub-1", "reportId")
        guard let (request, body) = RecordingURLProtocol.requests.first else { expect(false, "a request was made"); return }
        expectEqual(request.httpMethod, "POST", "POST")
        expectEqual(request.url?.absoluteString, "https://t.example/v1/events", "/v1/events replaces the base path")
        expectEqual(request.value(forHTTPHeaderField: "Authorization"), "Bearer k", "Bearer key")
        expectEqual(request.value(forHTTPHeaderField: "Content-Type"), "application/json", "JSON")
        expectEqual(String(decoding: body, as: UTF8.self), #"{"name":"heartbeat"}"#, "body is the wire JSON")
    }

    /// Consent off: the transport is never called, by heartbeat or session-end.
    @MainActor
    static func testTelemetryNoSendWithoutConsent() {
        let log = TransportLog()
        let (service, clock, _, _) = telemetryHarness(consentOn: false, transport: fakeTransport(log, .init(status: .accepted)))
        service.start()
        clock.advance(by: 49 * 3600)
        expect(service.fireSessionEnd() == nil, "no session-end without consent")
        drainMainActor()
        expectEqual(log.calls.count, 0, "nothing sent without consent")
        expect(!service.previewJSON().isEmpty, "the preview still works with consent off")
    }

    /// No key: nothing sent, and the heartbeat timer is not even scheduled.
    /// Mutation: schedule the heartbeat without a key.
    @MainActor
    static func testTelemetryNoSendWithoutKey() {
        let log = TransportLog()
        let (service, clock, _, _) = telemetryHarness(key: nil, transport: fakeTransport(log, .init(status: .accepted)))
        service.start()
        expectEqual(clock.pending.count, 0, "no heartbeat scheduled without a key")
        clock.advance(by: 49 * 3600)
        expect(service.fireSessionEnd() == nil, "no session-end without a key")
        expectEqual(log.calls.count, 0, "nothing sent without a key")
    }

    /// Session-end is sent once per process, with the 2 s cap. Mutation: send it twice.
    @MainActor
    static func testTelemetrySessionEndOnce() {
        let log = TransportLog()
        let (service, _, _, _) = telemetryHarness(transport: fakeTransport(log, .init(status: .retryLater, retryAfter: 30)))
        service.start()
        let first = service.fireSessionEnd()
        expect(first != nil, "session-end fires")
        expect(service.fireSessionEnd() == nil, "a second call does nothing")
        drainMainActor()
        expectEqual(log.calls.count, 1, "one POST")
        expectEqual(log.calls.first?.timeout, 2, "the 2 s session-end timeout")
        expect(log.calls.first?.body.contains(#""name":"session-end""#) == true, "the session-end event")
    }

    /// Nothing before 24 h; one at 24 h; consent off at 30 h -> the 48 h fire sends nothing; a
    /// RetryLater of 600 s moves the next fire to +600 s. Session-end never reschedules.
    /// Mutation: honour Retry-After on session-end (it would schedule a fire).
    @MainActor
    static func testTelemetryHeartbeatCadence() {
        let log = TransportLog()
        var result = TelemetrySendResult(status: .accepted)
        let (service, clock, files, _) = telemetryHarness(transport: { url, key, body, timeout in
            log.calls.append((url, key, body, timeout)); return result })
        service.start()
        clock.advance(by: 24 * 3600 - 1)
        expectEqual(log.calls.count, 0, "nothing before 24 h")
        clock.advance(by: 1)
        expectEqual(log.calls.count, 1, "one heartbeat at 24 h")
        expectEqual(log.calls.first?.timeout, 15, "the 15 s heartbeat timeout")
        expect(log.calls.first?.body.contains(#""name":"heartbeat""#) == true, "a heartbeat event")
        clock.advance(by: 6 * 3600)                                   // 30 h
        files.files["/fake/telemetry-consent.json"] = Data(#"{"enabled":false}"#.utf8)
        clock.advance(by: 18 * 3600)                                  // 48 h
        expectEqual(log.calls.count, 1, "consent off at 30 h: the 48 h fire sends nothing")
        files.files["/fake/telemetry-consent.json"] = Data(#"{"enabled":true}"#.utf8)
        result = TelemetrySendResult(status: .retryLater, httpStatus: 429, retryAfter: 600)
        clock.advance(by: 24 * 3600)                                  // 72 h
        expectEqual(log.calls.count, 2, "the 72 h heartbeat")
        expectEqual(clock.pending.first, clock.now.addingTimeInterval(600), "RetryLater 600 s: the next fire is at +600 s")
        result = TelemetrySendResult(status: .accepted)
        clock.advance(by: 600)
        expectEqual(log.calls.count, 3, "the retry fires at +600 s")
        let before = clock.pending
        result = TelemetrySendResult(status: .retryLater, retryAfter: 5)
        _ = service.fireSessionEnd()
        drainMainActor()
        expectEqual(clock.pending, before, "session-end never honours Retry-After (no timer moved)")
    }

    /// The exact wire is logged BEFORE the POST. Mutation: log after the POST.
    @MainActor
    static func testTelemetryAuditLogPrecedesSend() {
        var linesAtSend: [String] = []
        let (service, _, _, _) = telemetryHarness(transport: { _, _, _, _ in
            linesAtSend = harnessLines.lines; return TelemetrySendResult(status: .accepted, reportId: "r9") })
        service.start()
        _ = service.fireSessionEnd()
        drainMainActor()
        let wire = TelemetryPayloadBuilder.serializeWire(service.buildPayload(TelemetryEventNames.sessionEnd))
        expect(linesAtSend.contains { $0.hasPrefix("telemetry: sending 'session-end' (") && $0.hasSuffix(wire) },
               "the exact wire was logged before the transport ran (\(linesAtSend))")
        expect(harnessLines.lines.contains("telemetry: 'session-end' accepted (reportId=r9)"), "the outcome line")
        expect(!harnessLines.lines.joined().contains("Bearer"), "the key never reaches the log")
    }

    /// markSent only on Accepted. Mutation: mark on RetryLater.
    @MainActor
    static func testTelemetryMarksSentOnlyOnAccept() {
        let log = TransportLog()
        let (rejected, _, rejectedFiles, _) = telemetryHarness(transport: fakeTransport(log, .init(status: .rejected, httpStatus: 400, message: "no")))
        rejected.start(); _ = rejected.fireSessionEnd(); drainMainActor()
        expectEqual(TelemetryConsentStore(path: "/fake/telemetry-consent.json", fileIO: rejectedFiles.io).load().lastSentUtc, nil,
                    "a rejected send records nothing")
        expect(harnessLines.lines.contains("telemetry: 'session-end' not accepted (Rejected, http=400): no"), "Windows' outcome line")
        let (accepted, clock, acceptedFiles, _) = telemetryHarness(transport: fakeTransport(log, .init(status: .accepted)))
        accepted.start(); _ = accepted.fireSessionEnd(); drainMainActor()
        expectEqual(TelemetryConsentStore(path: "/fake/telemetry-consent.json", fileIO: acceptedFiles.io).load().lastSentUtc, clock.now,
                    "an accepted send records the clock's time")
    }

    /// A transport that never returns: one ignores its timeout, one honours it by answering
    /// RetryLater at the deadline on the manual clock (rule 9). The quit must reply after
    /// exactly 2 s against both. Mutation: reply only when the send finishes.
    @MainActor
    static func testQuitIdleWaitsForSessionEnd() {
        for honoursTimeout in [false, true] {
            var clockRef: ManualTelemetryClock?
            let transport: TelemetryTransport = { _, _, _, timeout in
                await withCheckedContinuation { (resume: CheckedContinuation<TelemetrySendResult, Never>) in
                    guard honoursTimeout, let clock = clockRef else { return }   // ignores the timeout: never resumes
                    _ = clock.schedule(after: timeout) { resume.resume(returning: TelemetrySendResult(status: .retryLater, message: "timeout")) }
                }
            }
            let (service, clock, _, _) = telemetryHarness(transport: transport)
            clockRef = clock
            service.start()
            let model = AppModel()
            model.telemetryService = service
            var replies: [(Bool, Date)] = []
            model.quitReplyForTesting = { replies.append(($0, clock.now)) }
            let label = honoursTimeout ? "timeout-honouring transport" : "timeout-ignoring transport"
            expectEqual(model.handleQuitRequest(), .terminateLater, "\(label): an idle quit with consent and a key waits")
            let start = clock.now
            clock.advance(by: 1.9)
            expectEqual(replies.count, 0, "\(label): no reply before 2 s")
            clock.advance(by: 0.1)
            expectEqual(replies.count, 1, "\(label): one reply at 2 s")
            expectEqual(replies.first?.0, true, "\(label): the quit proceeds")
            expectEqual(replies.first.map { $0.1.timeIntervalSince(start) }, 2, "\(label): at exactly 2 s")
            clock.advance(by: 5)
            expectEqual(replies.count, 1, "\(label): never a second reply")
        }
    }

    /// No consent: the idle quit is exactly as before.
    @MainActor
    static func testQuitIdleUnchangedWithoutConsent() {
        let log = TransportLog()
        let (service, _, _, _) = telemetryHarness(consentOn: false, transport: fakeTransport(log, .init(status: .accepted)))
        service.start()
        let model = AppModel()
        model.telemetryService = service
        expectEqual(model.handleQuitRequest(), .terminateNow, "no consent: terminate now")
        let bare = AppModel()
        expectEqual(bare.handleQuitRequest(), .terminateNow, "no telemetry service: terminate now")
    }

    /// The transport returns at 0.3 s: the quit replies at 0.3 s, not 2 s.
    @MainActor
    static func testQuitIdleRepliesWhenSendFinishes() {
        var clockRef: ManualTelemetryClock?
        let transport: TelemetryTransport = { _, _, _, _ in
            await withCheckedContinuation { (resume: CheckedContinuation<TelemetrySendResult, Never>) in
                _ = clockRef?.schedule(after: 0.3) { resume.resume(returning: TelemetrySendResult(status: .accepted)) }
            }
        }
        let (service, clock, _, _) = telemetryHarness(transport: transport)
        clockRef = clock
        service.start()
        let model = AppModel()
        model.telemetryService = service
        var replies: [Date] = []
        model.quitReplyForTesting = { _ in replies.append(clock.now) }
        let start = clock.now
        expectEqual(model.handleQuitRequest(), .terminateLater, "waits")
        clock.advance(by: 0.3)
        expectEqual(replies.count, 1, "replied when the send finished")
        expect(replies.first.map { abs($0.timeIntervalSince(start) - 0.3) < 0.001 } == true,
               "at 0.3 s on the manual clock (Date arithmetic: 1 ms tolerance)")
        clock.advance(by: 3)
        expectEqual(replies.count, 1, "the 2 s cap does not reply again")
    }

    // ── Task 9: the card ─────────────────────────────────────────────────────

    /// The four strings, verbatim from Windows SettingsViewModel.cs:140-152 / SettingsPage.xaml.
    static func testTelemetryCardCopy() {
        expectEqual(TelemetryCardCopy.title, "Diagnostics & telemetry", "title")
        expectEqual(TelemetryCardCopy.toggle, "Share anonymous health data", "toggle")
        expectEqual(TelemetryCardCopy.body,
                    "Send anonymous health data on app close and once a day: app version, session length, output counts (recording / streaming / virtual camera on-off, ISO / capture / participant counts), crashes since the last send, and a hardware class (CPU cores, RAM band, GPU tier). No names, meeting IDs, stream keys, URLs, or file paths are ever included.",
                    "what-is-sent copy")
        expectEqual(TelemetryCardCopy.status(configured: true), "Telemetry endpoint and API key configured.", "configured")
        expectEqual(TelemetryCardCopy.status(configured: false),
                    "Telemetry API key is missing in this build (endpoint is configured) — nothing is sent even when enabled.",
                    "missing key")
        expectEqual(TelemetryCardCopy.preview, "Preview what's sent", "preview button")
    }

    /// Loading a stored opt-in writes nothing; flipping writes once and logs the decision.
    /// Mutation: persist during the initial load.
    @MainActor
    static func testTelemetryTogglePersistsNotOnLoad() {
        let log = TransportLog()
        let (service, _, files, _) = telemetryHarness(transport: fakeTransport(log, .init(status: .accepted)))
        let model = AppModel()
        model.telemetryService = service
        let writes = files.writes
        model.loadTelemetryConsent()
        expectEqual(model.telemetryConsentOn, true, "the stored opt-in is shown")
        expectEqual(files.writes, writes, "loading writes nothing")
        model.setTelemetryConsent(false)
        expectEqual(files.writes, writes + 1, "flipping writes once")
        expectEqual(TelemetryConsentStore(path: "/fake/telemetry-consent.json", fileIO: files.io).load().enabled, false, "revoked on disk")
        expect(harnessLines.lines.contains("telemetry: consent disabled by operator"), "the decision is logged")
        model.setTelemetryConsent(false)
        expectEqual(files.writes, writes + 1, "the same value writes nothing more")
    }

    /// The preview works with consent off and is the indented session-end payload.
    /// Mutation: gate the preview on consent.
    @MainActor
    static func testTelemetryPreviewWithoutConsent() {
        let log = TransportLog()
        let (service, _, _, _) = telemetryHarness(consentOn: false, transport: fakeTransport(log, .init(status: .accepted)))
        let model = AppModel()
        model.telemetryService = service
        model.loadTelemetryConsent()
        expectEqual(model.telemetryConsentOn, false, "consent off")
        model.previewTelemetry()
        expect(model.telemetryPreviewJSON.contains("\"name\": \"session-end\""), "the session-end payload, indented (\(model.telemetryPreviewJSON.prefix(80)))")
        expectEqual(log.calls.count, 0, "previewing sends nothing")
    }

    /// In a meeting AND with consent and a key: the Comms release-all and session-end run side
    /// by side, and the quit replies ONCE, after both. (The two holds were merged when #1236's
    /// Comms release-first quit met this batch.) Mutations: reply after the release alone;
    /// reply twice (once per hold).
    @MainActor
    static func testQuitIdleWaitsForCommsAndSessionEnd() {
        let (m, _) = heldOnComms()
        m.quitCoreGoneForTesting = false
        var clockRef: ManualTelemetryClock?
        let transport: TelemetryTransport = { _, _, _, _ in
            await withCheckedContinuation { (resume: CheckedContinuation<TelemetrySendResult, Never>) in
                _ = clockRef?.schedule(after: 0.3) { resume.resume(returning: TelemetrySendResult(status: .accepted)) }
            }
        }
        let (service, clock, _, _) = telemetryHarness(transport: transport)
        clockRef = clock
        service.start()
        m.telemetryService = service
        var released = false
        let wrapped = m.quitRequestForTesting
        m.quitRequestForTesting = { body, t in
            if body["type"] as? String == "comms-release-all" { released = true }
            return try await wrapped!(body, t)
        }
        var replies: [Bool] = []
        m.quitReplyForTesting = { replies.append($0) }
        expectEqual(m.handleQuitRequest(), .terminateLater, "in a meeting with telemetry on, the quit waits")
        spin(until: { released }, limit: 2)
        drainMainActor()
        expect(released, "the Comms release-all went out")
        expectEqual(replies.count, 0, "released, but session-end is still in flight: no reply yet")
        clock.advance(by: 0.3)
        expectEqual(replies, [true], "both done: ONE reply, and the quit proceeds")
        clock.advance(by: 3)
        drainMainActor()
        expectEqual(replies.count, 1, "never a second reply")
    }
}
