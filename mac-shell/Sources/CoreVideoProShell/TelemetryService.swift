// TelemetryService.swift: the opt-in event client and service (Phase 4 R4, Task 8). Ports of
// Windows TelemetryEventClient (MC/Services/TelemetryEventClient.cs) and TelemetryEventService
// (W/Services/TelemetryEventService.cs):
// - heartbeat every 24 h (first after 24 h), scheduled ONLY if configured at start, sent only if
//   consent is on at fire time, 15 s timeout; a RetryLater with Retry-After moves the next fire;
// - session-end once per process, 2 s timeout, never honours Retry-After;
// - the exact wire JSON is logged BEFORE each POST (an inspectable audit trail), and the API key
//   is never logged;
// - nothing is sent without consent AND a key (TelemetryConfig.enabled).
// Every interval and timeout runs on an injected TelemetryClock, so tests never wait.

import Foundation

enum TelemetrySendStatus: String, Equatable {
    // Windows' enum names, as they appear in the log line.
    case accepted = "Accepted", retryLater = "RetryLater", rejected = "Rejected"
}

struct TelemetrySendResult: Equatable {
    var status: TelemetrySendStatus
    var reportId: String? = nil
    var httpStatus: Int? = nil
    var message: String? = nil
    var retryAfter: TimeInterval? = nil
}

/// POST one event: (endpoint base, API key, wire JSON, timeout) -> outcome.
typealias TelemetryTransport = (_ url: URL, _ apiKey: String, _ body: String, _ timeout: TimeInterval) async -> TelemetrySendResult

enum TelemetryEventClient {
    /// Windows `new Uri(endpoint, "/v1/events")`: the absolute path replaces any base path.
    static func eventsURL(_ endpoint: URL) -> URL {
        URL(string: "/v1/events", relativeTo: endpoint)?.absoluteURL ?? endpoint
    }

    /// Windows' outcome classes: 2xx Accepted (reportId read); 429 or >= 500 RetryLater with
    /// Retry-After (delta seconds or an HTTP date relative to `now`); anything else Rejected.
    /// Error bodies are trimmed and cut to 300 characters.
    static func classify(status: Int, headers: [String: String], body: Data, now: Date) -> TelemetrySendResult {
        if (200..<300).contains(status) {
            let id = ((try? JSONSerialization.jsonObject(with: body)) as? [String: Any])?["reportId"] as? String
            return TelemetrySendResult(status: .accepted, reportId: id, httpStatus: status)
        }
        var text = String(decoding: body, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        if text.count > 300 { text = String(text.prefix(300)) }
        if status == 429 || status >= 500 {
            return TelemetrySendResult(status: .retryLater, httpStatus: status,
                                       message: "Server busy or unavailable (HTTP \(status)): \(text)",
                                       retryAfter: retryAfter(headers, now: now))
        }
        return TelemetrySendResult(status: .rejected, httpStatus: status,
                                   message: "Server rejected the event (HTTP \(status)): \(text)")
    }

    static func retryAfter(_ headers: [String: String], now: Date) -> TimeInterval? {
        guard let raw = headers.first(where: { $0.key.caseInsensitiveCompare("Retry-After") == .orderedSame })?.value
            .trimmingCharacters(in: .whitespaces), !raw.isEmpty else { return nil }
        if let seconds = Int(raw), seconds >= 0 { return TimeInterval(seconds) }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "GMT")
        formatter.dateFormat = "EEE, dd MMM yyyy HH:mm:ss zzz"
        guard let date = formatter.date(from: raw) else { return nil }
        return max(0, date.timeIntervalSince(now))
    }

    /// The real transport: an HTTPS POST on `session` (an ephemeral session in production).
    static func urlSessionTransport(_ session: URLSession, now: @escaping () -> Date = Date.init) -> TelemetryTransport {
        { endpoint, apiKey, body, timeout in
            var request = URLRequest(url: eventsURL(endpoint), timeoutInterval: timeout)
            request.httpMethod = "POST"
            request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = Data(body.utf8)
            do {
                let (data, response) = try await session.data(for: request)
                guard let http = response as? HTTPURLResponse else {
                    return TelemetrySendResult(status: .retryLater, message: "Send failed: no HTTP response")
                }
                var headers: [String: String] = [:]
                for (key, value) in http.allHeaderFields { headers["\(key)"] = "\(value)" }
                return classify(status: http.statusCode, headers: headers, body: data, now: now())
            } catch {
                return TelemetrySendResult(status: .retryLater, message: "Send failed: \(error.localizedDescription)")
            }
        }
    }
}

protocol TelemetryTimerToken { func cancel() }

/// The service's only source of time: `now`, and one-shot scheduling on the main actor.
protocol TelemetryClock {
    var now: Date { get }
    func schedule(after: TimeInterval, _ fire: @escaping @MainActor () -> Void) -> TelemetryTimerToken
}

/// Production clock: wall time, main-queue scheduling (serviced in every run-loop mode, so it
/// fires during the .terminateLater quit window).
struct RealTelemetryClock: TelemetryClock {
    final class Token: TelemetryTimerToken {
        let item: DispatchWorkItem
        init(_ item: DispatchWorkItem) { self.item = item }
        func cancel() { item.cancel() }
    }
    var now: Date { Date() }
    func schedule(after: TimeInterval, _ fire: @escaping @MainActor () -> Void) -> TelemetryTimerToken {
        let item = DispatchWorkItem { MainActor.assumeIsolated { fire() } }
        DispatchQueue.main.asyncAfter(deadline: .now() + after, execute: item)
        return Token(item)
    }
}

@MainActor
final class TelemetryService {
    static let heartbeatInterval: TimeInterval = 24 * 3600
    static let heartbeatSendTimeout: TimeInterval = 15
    static let sessionEndSendTimeout: TimeInterval = 2

    private let config: TelemetryConfig
    private let consent: TelemetryConsentStore
    private let snapshot: () -> JSONObject?
    private let crashReports: () -> [CrashReportRecord]
    private let version: () -> String
    private let machine: () -> TelemetryMachineClass
    private let transport: TelemetryTransport
    let clock: TelemetryClock
    private let log: (String) -> Void

    private var sessionStart: Date?
    private var heartbeatToken: TelemetryTimerToken?
    private var sessionEndSent = false

    init(config: TelemetryConfig, consent: TelemetryConsentStore, snapshot: @escaping () -> JSONObject?,
         crashReports: @escaping () -> [CrashReportRecord], version: @escaping () -> String,
         machine: @escaping () -> TelemetryMachineClass, transport: @escaping TelemetryTransport,
         clock: TelemetryClock, log: @escaping (String) -> Void) {
        self.config = config
        self.consent = consent
        self.snapshot = snapshot
        self.crashReports = crashReports
        self.version = version
        self.machine = machine
        self.transport = transport
        self.clock = clock
        self.log = log
    }

    var isConfigured: Bool { config.enabled }
    var configuration: TelemetryConfig { config }
    var consentStore: TelemetryConsentStore { consent }

    /// Starts the session clock; the heartbeat is scheduled only when configured NOW.
    func start() {
        guard sessionStart == nil else { return }
        sessionStart = clock.now
        if config.enabled { scheduleHeartbeat(after: Self.heartbeatInterval) }
    }

    var sessionLengthSeconds: Int64 {
        guard let sessionStart else { return 0 }
        return Int64(max(0, clock.now.timeIntervalSince(sessionStart)))
    }

    func buildPayload(_ name: String) -> TelemetryPayload {
        TelemetryPayloadBuilder.build(
            name: name, version: version(), sessionLengthSeconds: sessionLengthSeconds, snapshot: snapshot(),
            crashCountSinceLastSend: TelemetryCrashCount.countSince(reports: crashReports(), lastSentUtc: consent.load().lastSentUtc),
            machine: machine())
    }

    /// The indented session-end payload. Works with consent off (the operator may look first).
    func previewJSON() -> String { TelemetryPayloadBuilder.serializePreview(buildPayload(TelemetryEventNames.sessionEnd)) }

    private var ready: Bool { config.enabled && consent.load().enabled }

    /// Sends session-end once per process, or returns nil (no consent, no key, or already sent).
    func fireSessionEnd() -> Task<Void, Never>? {
        guard ready, !sessionEndSent else { return nil }
        sessionEndSent = true
        return Task { @MainActor in
            // Session-end NEVER honours Retry-After: the result is only logged.
            _ = await self.send(TelemetryEventNames.sessionEnd, timeout: Self.sessionEndSendTimeout)
        }
    }

    private func scheduleHeartbeat(after delay: TimeInterval) {
        heartbeatToken?.cancel()
        heartbeatToken = clock.schedule(after: delay) { [weak self] in self?.heartbeatFired() }
    }

    private func heartbeatFired() {
        scheduleHeartbeat(after: Self.heartbeatInterval)   // the cadence continues whatever happens
        guard ready else { return }
        Task { @MainActor in
            let result = await self.send(TelemetryEventNames.heartbeat, timeout: Self.heartbeatSendTimeout)
            if result.status == .retryLater, let delay = result.retryAfter, delay > 0 {
                self.scheduleHeartbeat(after: delay)   // only the heartbeat honours Retry-After
            }
        }
    }

    private func send(_ name: String, timeout: TimeInterval) async -> TelemetrySendResult {
        guard let apiKey = config.apiKey, let endpoint = URL(string: config.endpoint) else {
            return TelemetrySendResult(status: .rejected, message: "not configured")
        }
        let wire = TelemetryPayloadBuilder.serializeWire(buildPayload(name))
        // Audit trail: the exact JSON is logged locally before it leaves the machine.
        log("telemetry: sending '\(name)' (\(wire.utf8.count) bytes): \(wire)")
        let result = await transport(endpoint, apiKey, wire, timeout)
        if result.status == .accepted {
            do { try consent.markSent(clock.now) } catch { log("telemetry: could not record the send time (\(error.localizedDescription))") }
            log("telemetry: '\(name)' accepted (reportId=\(result.reportId ?? "none"))")
        } else {
            log("telemetry: '\(name)' not accepted (\(result.status.rawValue), http=\(result.httpStatus.map(String.init) ?? "n/a")): \(result.message ?? "")")
        }
        return result
    }
}
