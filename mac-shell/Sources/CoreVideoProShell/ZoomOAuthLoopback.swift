// ZoomOAuthLoopback — receives the Zoom sign-in callback on a one-shot
// loopback HTTP listener (RFC 8252 §7.3), instead of the corevideo:// scheme.
//
// Why (owner, signed 1.0.0, 2026-11-15): the OBS plugin product ALSO registers
// corevideo:// (its CoreVideoOAuthCallback.app, com.corevideo.oauth-callback),
// and LaunchServices had made THAT the default handler. The browser handed the
// broker's redirect to the OBS helper and CoreVideo Pro never finished signing
// in. Any OBS plugin install or rebuild can re-take the scheme, so a scheme
// callback is not something this app can rely on. A loopback port is owned by
// exactly one process. The deployed broker (site-worker.js
// isAllowedOauthReturnUri) already accepts http://127.0.0.1:<port>/oauth/callback
// — path exactly /oauth/callback — and redirects the browser there with
// `state` + `broker_token` (or `error`).
//
// The listener binds 127.0.0.1 ONLY (never the LAN), on an ephemeral port, and
// is NOT the control API server (8011). It answers exactly one GET to
// /oauth/callback; anything else (a browser's /favicon.ico) is 404 and does not
// consume the one-shot. It stops after that callback, after a 5 minute timeout,
// or when a new sign-in begins. The URL carries the broker token, so nothing
// here ever logs a request line or a query.

import Foundation
import Network

/// The pure HTTP pieces, tested without a socket.
enum OAuthLoopbackHttp {
    static let callbackPath = "/oauth/callback"
    /// A request head larger than this is not a browser redirect.
    static let maxHeadBytes = 16 * 1024

    struct RequestLine: Equatable {
        var method: String
        var path: String
        var query: String
    }

    /// "GET /oauth/callback?state=a&broker_token=b HTTP/1.1" -> method/path/query.
    /// Takes the whole head (or just its first line); nil for anything malformed.
    static func parseRequestLine(_ head: String) -> RequestLine? {
        guard let line = head.split(maxSplits: 1, omittingEmptySubsequences: false,
                                    whereSeparator: \.isNewline).first
        else { return nil }
        let parts = line.split(separator: " ", omittingEmptySubsequences: true)
        guard parts.count == 3, parts[2].hasPrefix("HTTP/1."), parts[1].hasPrefix("/") else {
            return nil
        }
        let target = String(parts[1])
        let pathAndQuery = target.split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false)
        return RequestLine(method: String(parts[0]),
                           path: String(pathAndQuery[0]),
                           query: pathAndQuery.count > 1 ? String(pathAndQuery[1]) : "")
    }

    /// Is this THE callback? (Exactly GET + exactly the broker's path.)
    static func isCallback(_ request: RequestLine?) -> Bool {
        request?.method == "GET" && request?.path == callbackPath
    }

    /// One query parameter, percent-decoded (nil when absent).
    static func queryValue(_ query: String, _ name: String) -> String? {
        var components = URLComponents()
        components.percentEncodedQuery = query
        return components.queryItems?.first { $0.name == name }?.value
    }

    static func returnUri(port: UInt16) -> String {
        "http://127.0.0.1:\(port)\(callbackPath)"
    }

    /// The URL handed to ZoomOAuth.handleCallback — same query, so the state
    /// check and the broker_token/error handling are the scheme path's own.
    static func callbackURL(port: UInt16, query: String) -> URL? {
        URL(string: returnUri(port: port) + (query.isEmpty ? "" : "?" + query))
    }

    /// The page the browser shows. `status` is handleCallback's result ("" = signed in).
    static func page(status: String) -> String {
        let signedIn = status.isEmpty
        let title = signedIn ? "CoreVideo Pro is signed in to Zoom" : "Zoom sign-in did not finish"
        let detail = signedIn
            ? "You can close this tab and return to CoreVideo Pro."
            : escape(status) + " Return to CoreVideo Pro and choose Sign in with Zoom again."
        return """
        <!doctype html><html lang="en"><head><meta charset="utf-8"><title>\(title)</title>
        <style>body{font-family:system-ui,sans-serif;margin:0;min-height:100vh;display:grid;\
        place-items:center;background:#090b12;color:#f7f8ff}main{max-width:520px;padding:16px}\
        p{color:#b9c0d4;line-height:1.5}</style></head>
        <body><main><h1>\(title)</h1><p>\(detail)</p></main></body></html>
        """
    }

    static func response(status: Int, reason: String, body: String) -> Data {
        let bytes = Data(body.utf8)
        let head = "HTTP/1.1 \(status) \(reason)\r\n"
            + "Content-Type: text/html; charset=utf-8\r\n"
            + "Content-Length: \(bytes.count)\r\n"
            + "Cache-Control: no-store\r\n"
            + "Connection: close\r\n\r\n"
        return Data(head.utf8) + bytes
    }

    static func escape(_ text: String) -> String {
        text.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
            .replacingOccurrences(of: "\"", with: "&quot;")
    }
}

/// The listener's lifecycle, as a pure state machine. Each sign-in is a new
/// `generation`; a stale timer or a late request for an older one is ignored.
struct OAuthLoopbackLifecycle: Equatable {
    enum Phase: Equatable { case idle, listening, handling, stopped }
    private(set) var phase: Phase = .idle
    private(set) var generation = 0

    private var expectedState = ""

    /// `wrongState`: a callback-shaped request whose `state` is not this
    /// sign-in's. Fix round 1 (review M1): it used to be taken, so any local
    /// process or a web page's no-cors fetch/<img> to the port could send
    /// `?error=x` and end a pending sign-in. It is now refused WITHOUT
    /// consuming the one-shot, and never reaches handleCallback.
    enum RequestVerdict: Equatable { case callback, notFound, wrongState, closed }

    /// A new sign-in. Returns true when an older listener must be stopped first.
    mutating func begin(expectedState: String) -> Bool {
        let hadOne = phase == .listening || phase == .handling
        generation += 1
        phase = .listening
        self.expectedState = expectedState
        return hadOne
    }

    /// One request arrived. Only the FIRST callback carrying THIS sign-in's
    /// state is taken; other paths and other states never consume the one-shot.
    mutating func request(_ line: OAuthLoopbackHttp.RequestLine?, generation: Int) -> RequestVerdict {
        guard generation == self.generation, phase == .listening else { return .closed }
        guard OAuthLoopbackHttp.isCallback(line) else { return .notFound }
        guard !expectedState.isEmpty,
              OAuthLoopbackHttp.queryValue(line?.query ?? "", "state") == expectedState
        else { return .wrongState }
        phase = .handling
        return .callback
    }

    /// The callback was handled (success or error): stop listening.
    mutating func finished(generation: Int) {
        if generation == self.generation, phase == .handling { phase = .stopped }
    }

    /// The timer fired. True when it should stop THIS listener (and say so).
    mutating func timedOut(generation: Int) -> Bool {
        guard generation == self.generation, phase == .listening else { return false }
        phase = .stopped
        return true
    }

    mutating func cancel() {
        if phase != .idle { phase = .stopped }
    }
}

/// The socket half. All mutable state is confined to `queue`.
final class OAuthLoopbackListener: @unchecked Sendable {
    static let defaultTimeout: TimeInterval = 5 * 60
    /// A connection gets this long to send a complete request head. Fix round 1
    /// (review M4): without it a connection that never finished its head held
    /// one of the 16 slots until the 5 minute timeout.
    static let defaultRequestTimeout: TimeInterval = 10

    private let requestTimeout: TimeInterval
    private var deadlines: [ObjectIdentifier: DispatchWorkItem] = [:]

    init(requestTimeout: TimeInterval = OAuthLoopbackListener.defaultRequestTimeout) {
        self.requestTimeout = requestTimeout
    }

    typealias CallbackHandler = @Sendable (URL) async -> String
    private let queue = DispatchQueue(label: "corevideo.oauth-loopback")
    private var listener: NWListener?
    private var lifecycle = OAuthLoopbackLifecycle()
    private var port: UInt16 = 0
    private var timer: DispatchWorkItem?
    private var connections: [ObjectIdentifier: NWConnection] = [:]

    /// Starts a fresh one-shot listener (stopping any previous one) and reports
    /// the bound port. `onCallback` gets the callback URL and returns the status
    /// string the page should show ("" = signed in). `onTimeout` fires if no
    /// callback arrives within `timeout`.
    func start(expectedState: String,
               timeout: TimeInterval = OAuthLoopbackListener.defaultTimeout,
               onCallback: @escaping CallbackHandler,
               onTimeout: @escaping @Sendable () -> Void,
               completion: @escaping @Sendable (Result<UInt16, Error>) -> Void) {
        queue.async { [self] in
            if lifecycle.begin(expectedState: expectedState) { teardownLocked() }
            let generation = lifecycle.generation
            let next: NWListener
            do { next = try NWListener(using: Self.listenParameters()) } catch {
                lifecycle.cancel()
                completion(.failure(error))
                return
            }
            var reported = false
            next.stateUpdateHandler = { [weak self, weak next] state in
                guard let self else { return }
                switch state {
                case .ready:
                    guard !reported else { return }
                    reported = true
                    let bound = next?.port?.rawValue ?? 0
                    self.port = bound
                    completion(.success(bound))
                case .cancelled:
                    // Cancelled before .ready: never leave beginSignIn waiting.
                    if !reported { reported = true; completion(.failure(NWError.posix(.ECANCELED))) }
                case .failed(let error), .waiting(let error):
                    if !reported { reported = true; completion(.failure(error)) }
                    if self.lifecycle.generation == generation {
                        self.lifecycle.cancel()
                        self.teardownLocked()
                    }
                default:
                    break
                }
            }
            next.newConnectionHandler = { [weak self] connection in
                self?.accept(connection, generation: generation, onCallback: onCallback)
            }
            listener = next
            next.start(queue: queue)
            let work = DispatchWorkItem { [weak self] in
                guard let self, self.lifecycle.timedOut(generation: generation) else { return }
                self.teardownLocked()
                onTimeout()
            }
            timer = work
            queue.asyncAfter(deadline: .now() + timeout, execute: work)
        }
    }

    /// 127.0.0.1 only, ephemeral port. Anything wider would put a sign-in
    /// endpoint on the LAN for five minutes.
    static func listenParameters() -> NWParameters {
        let parameters = NWParameters.tcp
        parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: .any)
        return parameters
    }

    /// Stops listening (a new sign-in, sign-out, or a scheme callback won).
    func stop() {
        queue.async { [self] in
            lifecycle.cancel()
            teardownLocked()
        }
    }

    /// Tests: is a socket still bound?
    var isListening: Bool { queue.sync { listener != nil } }

    private func teardownLocked() {
        timer?.cancel()
        timer = nil
        listener?.cancel()
        listener = nil
        for connection in connections.values { connection.cancel() }
        connections.removeAll()
        for deadline in deadlines.values { deadline.cancel() }
        deadlines.removeAll()
    }

    /// Close every accepted connection except `keep` (a browser's speculative
    /// second socket, a stray local client) once the callback is taken.
    private func closeOthersLocked(keeping keep: NWConnection) {
        for connection in connections.values where connection !== keep { close(connection) }
    }

    private func accept(_ connection: NWConnection, generation: Int,
                        onCallback: @escaping CallbackHandler) {
        guard generation == lifecycle.generation, listener != nil, connections.count < 16 else {
            connection.cancel()
            return
        }
        connections[ObjectIdentifier(connection)] = connection
        let deadline = DispatchWorkItem { [weak self, weak connection] in
            guard let self, let connection else { return }
            self.close(connection)
        }
        deadlines[ObjectIdentifier(connection)] = deadline
        queue.asyncAfter(deadline: .now() + requestTimeout, execute: deadline)
        connection.start(queue: queue)
        receive(connection, buffer: Data(), generation: generation, onCallback: onCallback)
    }

    private func receive(_ connection: NWConnection, buffer: Data, generation: Int,
                         onCallback: @escaping CallbackHandler) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 8192) {
            [weak self] data, _, complete, error in
            guard let self else { return }
            var buffer = buffer
            if let data { buffer.append(data) }
            let terminator = Data("\r\n\r\n".utf8)
            if buffer.range(of: terminator) == nil {
                if complete || error != nil || buffer.count > OAuthLoopbackHttp.maxHeadBytes {
                    self.close(connection)
                } else {
                    self.receive(connection, buffer: buffer, generation: generation, onCallback: onCallback)
                }
                return
            }
            // A full head arrived: the read deadline is done (the callback's
            // token redeem may legitimately take longer than it).
            self.deadlines.removeValue(forKey: ObjectIdentifier(connection))?.cancel()
            let head = String(decoding: buffer, as: UTF8.self)
            let line = OAuthLoopbackHttp.parseRequestLine(head)
            switch self.lifecycle.request(line, generation: generation) {
            case .notFound:
                self.reply(connection, OAuthLoopbackHttp.response(
                    status: 404, reason: "Not Found", body: "Not found"))
            case .wrongState:
                self.reply(connection, OAuthLoopbackHttp.response(
                    status: 400, reason: "Bad Request",
                    body: OAuthLoopbackHttp.page(status: "This sign-in link is not for the current sign-in.")))
            case .closed:
                self.close(connection)
            case .callback:
                // The listening socket goes now — nothing else can reach this
                // sign-in — but this connection stays open for the page.
                self.timer?.cancel()
                self.listener?.cancel()
                self.listener = nil
                self.closeOthersLocked(keeping: connection)
                guard let url = OAuthLoopbackHttp.callbackURL(port: self.port, query: line?.query ?? "")
                else {
                    self.lifecycle.finished(generation: generation)
                    self.reply(connection, OAuthLoopbackHttp.response(
                        status: 400, reason: "Bad Request",
                        body: OAuthLoopbackHttp.page(status: "The sign-in callback was malformed.")))
                    return
                }
                Task {
                    let status = await onCallback(url)
                    self.queue.async {
                        self.lifecycle.finished(generation: generation)
                        self.reply(connection, OAuthLoopbackHttp.response(
                            status: 200, reason: "OK", body: OAuthLoopbackHttp.page(status: status)))
                    }
                }
            }
        }
    }

    private func reply(_ connection: NWConnection, _ data: Data) {
        connection.send(content: data, contentContext: .finalMessage, isComplete: true,
                        completion: .contentProcessed { [weak self] _ in self?.close(connection) })
    }

    private func close(_ connection: NWConnection) {
        connection.cancel()
        connections.removeValue(forKey: ObjectIdentifier(connection))
        deadlines.removeValue(forKey: ObjectIdentifier(connection))?.cancel()
    }

    /// Tests: accepted connections still open.
    var openConnectionCount: Int { queue.sync { connections.count } }
}
