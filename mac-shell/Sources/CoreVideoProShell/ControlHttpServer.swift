// ControlHttpServer.swift — the control API's transport: HTTP/1.1 + the /ws
// WebSocket on Network.framework (NWListener), the Swift counterpart of Windows
// `Http/HttpControlServer.cs`. Routing and bodies are ControlRouter's
// (ControlApi.swift); this file owns sockets, bounds, and the security rules.
//
// SECURITY (same rules as Windows, plus three hardenings — see each site):
//   - Binds 127.0.0.1 by default. A non-loopback bind REFUSES TO START without a
//     non-blank COREVIDEO_CONTROL_TOKEN (Windows HttpControlServerOptions.Validate,
//     same message), so there is never an unauthenticated non-loopback surface.
//   - When a token is configured every request needs `Authorization: Bearer
//     <token>` (scheme case-insensitive); `?token=` is accepted ONLY on the /ws
//     upgrade (browsers cannot set WS headers). Compared in constant time
//     (Windows uses string.Equals; constant time is a strict improvement).
//   - No CORS header at all. Windows sends `Access-Control-Allow-Origin: *`; the
//     Mac deliberately does not, so no web page can read a response.
//   - Hardening 1: POST /invoke requires `Content-Type: application/json`. A web
//     page can send a cross-origin "simple" POST (text/plain) WITHOUT a preflight
//     and the action would run even though the page cannot read the reply; JSON
//     forces a preflight, which this server never approves.
//   - Hardening 2: on a loopback bind the Host header must name loopback, which
//     defeats DNS rebinding (a hostile name re-pointed at 127.0.0.1).
//   - Hardening 3: a WebSocket upgrade carrying an Origin header (i.e. from a
//     browser) is refused. Companion's `ws` client sends none.
//
// BOUNDS: request head <= 16 KiB, body <= 64 KiB (Content-Length only; chunked is
// refused), whole request within 10 s, <= 64 concurrent connections, WebSocket
// client frames <= 64 KiB. Every response closes the connection. I/O runs on one
// private serial queue; model calls happen behind ControlSurface on the main actor.

import CryptoKit
import Foundation
import Network

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Options / policy
// ─────────────────────────────────────────────────────────────────────────────

struct ControlServerOptions: Equatable {
    /// 0 = ephemeral (tests).
    var port: UInt16 = 8011
    /// "127.0.0.1" by default; "+" (Windows' wildcard) = every interface.
    var host = "127.0.0.1"
    var authToken: String?

    /// Windows `Validate`: "localhost" or a loopback IP literal. Anything else —
    /// including a hostname that happens to resolve to loopback — is a network
    /// binding; DNS is not a security boundary.
    var isLoopback: Bool {
        if host.caseInsensitiveCompare("localhost") == .orderedSame { return true }
        let literal = host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        // .NET IPAddress.IsLoopback: all of 127.0.0.0/8 (Network's isLoopback is 127.0.0.1 only).
        if let v4 = IPv4Address(literal) { return v4.rawValue.first == 127 }
        if let v6 = IPv6Address(literal) { return v6.isLoopback }
        return false
    }

    var hasToken: Bool { !(authToken ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }

    /// Windows `HttpControlServerOptions.Validate`, before any listener exists.
    func validate() throws {
        if host.trimmingCharacters(in: .whitespaces).isEmpty {
            throw ControlServerError.policy("A control HTTP bind host is required.")
        }
        if !isLoopback && !hasToken {
            throw ControlServerError.policy(
                "LAN HTTP/WS control requires a non-empty COREVIDEO_CONTROL_TOKEN. "
                    + "Set a token or disable COREVIDEO_HTTP_LAN to use loopback.")
        }
    }

    /// Windows MainWindow: COREVIDEO_HTTP_LAN must be exactly "1"; COREVIDEO_HTTP_PORT
    /// is used only when it parses into 1...65535; COREVIDEO_CONTROL_TOKEN as given.
    static func fromEnvironment(_ environment: [String: String]) -> ControlServerOptions {
        var options = ControlServerOptions()
        if environment["COREVIDEO_HTTP_LAN"] == "1" { options.host = "+" }
        if let raw = environment["COREVIDEO_HTTP_PORT"], let port = Int(raw), (1...65535).contains(port) {
            options.port = UInt16(port)
        }
        options.authToken = environment["COREVIDEO_CONTROL_TOKEN"]
        return options
    }

    var displayEndpoint: String { "\(host):\(port)" }
}

enum ControlServerError: Error, Equatable, CustomStringConvertible {
    case policy(String)
    case portInUse(String)
    case listenFailed(String)

    var description: String {
        switch self {
        case .policy(let why): return why
        case .portInUse(let endpoint):
            return "port \(endpoint) is already in use (another CoreVideo Pro, or set COREVIDEO_HTTP_PORT)"
        case .listenFailed(let why): return why
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Auth
// ─────────────────────────────────────────────────────────────────────────────

enum ControlAuth {
    /// Equality whose running time does not depend on WHERE the strings differ.
    static func constantTimeEquals(_ a: String, _ b: String) -> Bool {
        let left = Array(a.utf8), right = Array(b.utf8)
        var difference = UInt8(left.count == right.count ? 0 : 1)
        let length = max(left.count, right.count)
        for index in 0..<length {
            let x = index < left.count ? left[index] : 0
            let y = index < right.count ? right[index] : 0
            difference |= x ^ y
        }
        return difference == 0
    }

    /// Windows `IsAuthorized`.
    static func isAuthorized(token: String?, authorization: String?, isWebSocketUpgrade: Bool,
                             queryToken: String?) -> Bool {
        guard let token, !token.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return true }
        if let header = authorization, header.count >= 7,
           header.prefix(7).caseInsensitiveCompare("Bearer ") == .orderedSame,
           constantTimeEquals(String(header.dropFirst(7)), token) {
            return true
        }
        // The token on the query string is allowed for the WS upgrade only.
        if isWebSocketUpgrade, let queryToken, constantTimeEquals(queryToken, token) { return true }
        return false
    }

    /// Hardening 2 (DNS rebinding): on a loopback bind the Host header must name
    /// loopback. A missing Host (HTTP/1.0 tooling) is allowed — browsers always
    /// send one. A LAN bind is guarded by the mandatory token instead.
    static func hostAllowed(_ hostHeader: String?, loopbackBind: Bool) -> Bool {
        guard loopbackBind, let raw = hostHeader?.trimmingCharacters(in: .whitespaces), !raw.isEmpty else { return true }
        var name = raw.lowercased()
        if name.hasPrefix("[") {
            name = String(name.prefix { $0 != "]" }.dropFirst())   // [::1]:8011 -> ::1
        } else if let colon = name.lastIndex(of: ":") {
            name = String(name[..<colon])
        }
        return name == "127.0.0.1" || name == "localhost" || name == "::1"
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - HTTP/1.1 request head parsing (bounded)
// ─────────────────────────────────────────────────────────────────────────────

struct ControlRequestHead: Equatable {
    var method: String
    var target: String
    var path: String
    var query: [String: String]
    /// Lower-cased names.
    var headers: [String: String]
    var contentLength: Int

    var isWebSocketUpgrade: Bool {
        (headers["upgrade"] ?? "").lowercased() == "websocket"
            && (headers["connection"] ?? "").lowercased().contains("upgrade")
    }
}

enum ControlHttpParse: Equatable {
    case incomplete
    case head(ControlRequestHead, bodyOffset: Int)
    case reject(status: Int, error: String)
}

enum ControlHttpParser {
    static let maxHeaderBytes = 16 * 1024
    static let maxBodyBytes = 64 * 1024
    private static let terminator = Data("\r\n\r\n".utf8)

    static func parseHead(_ buffer: Data) -> ControlHttpParse {
        guard let end = buffer.range(of: terminator) else {
            return buffer.count > maxHeaderBytes
                ? .reject(status: 431, error: "Request header too large.") : .incomplete
        }
        let headLength = end.lowerBound - buffer.startIndex
        if headLength > maxHeaderBytes { return .reject(status: 431, error: "Request header too large.") }
        guard let text = String(data: buffer[buffer.startIndex..<end.lowerBound], encoding: .utf8) else {
            return .reject(status: 400, error: "Malformed request.")
        }
        var lines = text.components(separatedBy: "\r\n")
        let requestLine = lines.removeFirst().split(separator: " ", omittingEmptySubsequences: false)
        guard requestLine.count == 3,
              !requestLine[0].isEmpty, requestLine[0].allSatisfy({ $0.isASCII && $0.isUppercase }),
              requestLine[1].hasPrefix("/"),
              requestLine[2] == "HTTP/1.1" || requestLine[2] == "HTTP/1.0" else {
            return .reject(status: 400, error: "Malformed request.")
        }
        var headers: [String: String] = [:]
        var contentLengths: [String] = []
        for line in lines {
            guard let colon = line.firstIndex(of: ":"), colon != line.startIndex else {
                return .reject(status: 400, error: "Malformed request.")
            }
            let name = line[..<colon].lowercased()
            let value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            if name == "content-length" { contentLengths.append(value) }
            headers[name] = value
        }
        if headers["transfer-encoding"] != nil {
            return .reject(status: 411, error: "Content-Length is required.")
        }
        var contentLength = 0
        if !contentLengths.isEmpty {
            guard Set(contentLengths).count == 1, let parsed = Int(contentLengths[0]), parsed >= 0 else {
                return .reject(status: 400, error: "Malformed request.")
            }
            contentLength = parsed
        }
        if contentLength > maxBodyBytes { return .reject(status: 413, error: "Request body too large.") }
        let target = String(requestLine[1])
        let parts = target.split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false)
        var query: [String: String] = [:]
        if parts.count == 2 {
            for pair in parts[1].split(separator: "&") {
                let kv = pair.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
                let key = String(kv[0]).removingPercentEncoding ?? String(kv[0])
                let value = kv.count > 1 ? (String(kv[1]).replacingOccurrences(of: "+", with: " ")
                    .removingPercentEncoding ?? String(kv[1])) : ""
                query[key] = value
            }
        }
        let path = String(parts[0]).removingPercentEncoding ?? String(parts[0])
        let head = ControlRequestHead(method: String(requestLine[0]), target: target, path: path,
                                      query: query, headers: headers, contentLength: contentLength)
        return .head(head, bodyOffset: end.upperBound - buffer.startIndex)
    }

    static func reason(_ status: Int) -> String {
        switch status {
        case 101: return "Switching Protocols"
        case 200: return "OK"
        case 400: return "Bad Request"
        case 401: return "Unauthorized"
        case 403: return "Forbidden"
        case 404: return "Not Found"
        case 408: return "Request Timeout"
        case 411: return "Length Required"
        case 413: return "Payload Too Large"
        case 415: return "Unsupported Media Type"
        case 422: return "Unprocessable Entity"
        case 431: return "Request Header Fields Too Large"
        case 503: return "Service Unavailable"
        default: return "Error"
        }
    }

    static func response(_ status: Int, _ body: String) -> Data {
        let payload = Data(body.utf8)
        let head = "HTTP/1.1 \(status) \(reason(status))\r\n"
            + "Content-Type: application/json; charset=utf-8\r\n"
            + "Content-Length: \(payload.count)\r\n"
            + "Cache-Control: no-store\r\n"
            + "Connection: close\r\n\r\n"
        return Data(head.utf8) + payload
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - WebSocket framing (RFC 6455, server side, text only)
// ─────────────────────────────────────────────────────────────────────────────

enum ControlWebSocket {
    static let maxClientFrame = 64 * 1024

    static func acceptKey(_ key: String) -> String {
        let digest = Insecure.SHA1.hash(data: Data((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").utf8))
        return Data(digest).base64EncodedString()
    }

    static func frame(opcode: UInt8, _ payload: Data) -> Data {
        var data = Data([0x80 | opcode])
        if payload.count < 126 {
            data.append(UInt8(payload.count))
        } else if payload.count <= 0xFFFF {
            data.append(126)
            data.append(UInt8(payload.count >> 8)); data.append(UInt8(payload.count & 0xFF))
        } else {
            data.append(127)
            for shift in stride(from: 56, through: 0, by: -8) { data.append(UInt8((UInt64(payload.count) >> UInt64(shift)) & 0xFF)) }
        }
        return data + payload
    }

    static func text(_ text: String) -> Data { frame(opcode: 0x1, Data(text.utf8)) }

    static func close(_ code: UInt16) -> Data { frame(opcode: 0x8, Data([UInt8(code >> 8), UInt8(code & 0xFF)])) }

    enum ClientFrame { case incomplete, data(opcode: UInt8, payload: Data, consumed: Int), violation(UInt16) }

    /// One client frame. Clients MUST mask; oversized frames are refused (1009).
    static func parse(_ buffer: Data) -> ClientFrame {
        let bytes = [UInt8](buffer.prefix(14))
        guard bytes.count >= 2 else { return .incomplete }
        let opcode = bytes[0] & 0x0F
        guard bytes[1] & 0x80 != 0 else { return .violation(1002) }
        var length = Int(bytes[1] & 0x7F)
        var offset = 2
        if length == 126 {
            guard bytes.count >= 4 else { return .incomplete }
            length = Int(bytes[2]) << 8 | Int(bytes[3]); offset = 4
        } else if length == 127 {
            guard bytes.count >= 10 else { return .incomplete }
            var value: UInt64 = 0
            for index in 2..<10 { value = value << 8 | UInt64(bytes[index]) }
            guard value <= UInt64(maxClientFrame) else { return .violation(1009) }
            length = Int(value); offset = 10
        }
        guard length <= maxClientFrame else { return .violation(1009) }
        guard bytes.count >= offset + 4, buffer.count >= offset + 4 + length else { return .incomplete }
        let mask = [UInt8](bytes[offset..<offset + 4])
        let start = buffer.startIndex + offset + 4
        var payload = [UInt8](buffer[start..<start + length])
        for index in payload.indices { payload[index] ^= mask[index % 4] }
        return .data(opcode: opcode, payload: Data(payload), consumed: offset + 4 + length)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Server
// ─────────────────────────────────────────────────────────────────────────────

final class ControlHttpServer: @unchecked Sendable {
    static let requestTimeout: TimeInterval = 10
    static let maxConnections = 64

    let options: ControlServerOptions
    private let router: ControlRouter
    private let queue = DispatchQueue(label: "corevideo.control-http")
    // All mutable state below is touched ONLY on `queue`.
    private var listener: NWListener?
    private var connections: [ObjectIdentifier: Connection] = [:]
    private var stopped = false

    init(router: ControlRouter, options: ControlServerOptions) {
        self.router = router
        self.options = options
    }

    /// Validates policy BEFORE allocating a listener (a refused LAN bind never
    /// listens), then reports the bound port or why it could not bind.
    func start(completion: @escaping (Result<UInt16, ControlServerError>) -> Void) {
        do { try options.validate() } catch {
            completion(.failure(error as? ControlServerError ?? .policy("\(error)")))
            return
        }
        let parameters = NWParameters.tcp
        parameters.allowLocalEndpointReuse = false
        let port = NWEndpoint.Port(rawValue: options.port) ?? .any
        if options.isLoopback {
            let literal = options.host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
            // "localhost" -> 127.0.0.1; a literal binds exactly that address.
            let host: NWEndpoint.Host = IPv6Address(literal).map { NWEndpoint.Host.ipv6($0) }
                ?? IPv4Address(literal).map { NWEndpoint.Host.ipv4($0) } ?? .ipv4(.loopback)
            parameters.requiredLocalEndpoint = .hostPort(host: host, port: port)
        } else if !["+", "*", "0.0.0.0", "[::]", "::"].contains(options.host) {
            parameters.requiredLocalEndpoint = .hostPort(host: NWEndpoint.Host(options.host), port: port)
        }
        let listener: NWListener
        do {
            listener = options.isLoopback || parameters.requiredLocalEndpoint != nil
                ? try NWListener(using: parameters)
                : try NWListener(using: parameters, on: port)
        } catch {
            completion(.failure(Self.classify(error, options)))
            return
        }
        var reported = false
        func report(_ result: Result<UInt16, ControlServerError>) {
            guard !reported else { return }
            reported = true
            completion(result)
        }
        listener.stateUpdateHandler = { [weak self, weak listener] state in
            switch state {
            case .ready:
                report(.success(listener?.port?.rawValue ?? 0))
            case .failed(let error), .waiting(let error):
                listener?.cancel()
                self?.listener = nil
                report(.failure(Self.classify(error, self?.options ?? ControlServerOptions())))
            case .cancelled:
                report(.failure(.listenFailed("listener cancelled")))
            default:
                break
            }
        }
        listener.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
        queue.async { [self] in
            guard !stopped else { report(.failure(.listenFailed("stopped"))); return }
            self.listener = listener
            listener.start(queue: queue)
        }
    }

    private static func classify(_ error: Error, _ options: ControlServerOptions) -> ControlServerError {
        if case .posix(let code) = error as? NWError, code == .EADDRINUSE {
            return .portInUse(options.displayEndpoint)
        }
        return .listenFailed("could not listen on \(options.displayEndpoint): \(error)")
    }

    func stop() {
        queue.sync {
            stopped = true
            listener?.cancel()
            listener = nil
            for connection in connections.values { connection.close() }
            connections.removeAll()
        }
    }

    /// Pushes a ControlState JSON to every open /ws client (Windows BroadcastAsync).
    func broadcastState(_ json: String) {
        let frame = ControlWebSocket.text(json)
        queue.async { [self] in
            for connection in connections.values where connection.isWebSocket { connection.send(frame) }
        }
    }

    // ── per connection (queue only) ──────────────────────────────────────────

    private func accept(_ nw: NWConnection) {
        guard !stopped, connections.count < Self.maxConnections else {
            nw.cancel()
            return
        }
        let connection = Connection(nw, server: self)
        connections[ObjectIdentifier(connection)] = connection
        connection.begin(on: queue)
    }

    fileprivate func forget(_ connection: Connection) {
        connections.removeValue(forKey: ObjectIdentifier(connection))
    }

    fileprivate final class Connection: @unchecked Sendable {   // confined to `queue`
        private let nw: NWConnection
        private weak var server: ControlHttpServer?
        private var buffer = Data()
        private var head: ControlRequestHead?
        private var bodyOffset = 0
        private(set) var isWebSocket = false
        private var finished = false
        /// Set once the request is handed to the router: later bytes never dispatch again.
        private var dispatched = false
        private var timeout: DispatchWorkItem?

        init(_ nw: NWConnection, server: ControlHttpServer) {
            self.nw = nw
            self.server = server
        }

        func begin(on queue: DispatchQueue) {
            nw.stateUpdateHandler = { [weak self] state in
                switch state {
                case .failed, .cancelled: self?.close()
                default: break
                }
            }
            let timer = DispatchWorkItem { [weak self] in
                guard let self, !self.isWebSocket, !self.finished else { return }
                self.respond(408, ControlJSON.error("Request timed out."))
            }
            timeout = timer
            queue.asyncAfter(deadline: .now() + ControlHttpServer.requestTimeout, execute: timer)
            nw.start(queue: queue)
            receive()
        }

        private func receive() {
            nw.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, complete, error in
                guard let self, !self.finished else { return }
                // After dispatch, later bytes are drained: never buffered, never re-dispatched.
                if let data, !data.isEmpty, !self.dispatched {
                    self.buffer.append(data)
                    // Hard cap: a head plus the largest admissible body (plus one read).
                    if !self.isWebSocket,
                       self.buffer.count > ControlHttpParser.maxHeaderBytes + ControlHttpParser.maxBodyBytes + 64 * 1024 {
                        return self.respond(413, ControlJSON.error("Request body too large."))
                    }
                    self.isWebSocket ? self.processFrames() : self.processRequest()
                }
                if self.finished { return }
                if error != nil { return self.close() }
                // A half-close after a complete request still gets its answer; respond() closes.
                if complete { return self.dispatched ? () : self.close() }
                self.receive()
            }
        }

        private func processRequest() {
            guard let server else { return close() }
            if head == nil {
                switch ControlHttpParser.parseHead(buffer) {
                case .incomplete:
                    return
                case .reject(let status, let error):
                    return respond(status, ControlJSON.error(error))
                case .head(let parsed, let offset):
                    head = parsed
                    bodyOffset = offset
                    if let refusal = admit(parsed, server.options) {
                        return respond(refusal.0, ControlJSON.error(refusal.1))
                    }
                    if parsed.isWebSocketUpgrade && ControlRouter.normalize(parsed.path) == "/ws" {
                        return upgrade(parsed)
                    }
                }
            }
            guard let head else { return }
            // The parser already refused anything over the body bound, so this
            // buffer is bounded by head + 64 KiB.
            guard buffer.count - bodyOffset >= head.contentLength else { return }
            let body = head.contentLength > 0
                ? buffer.subdata(in: (buffer.startIndex + bodyOffset)..<(buffer.startIndex + bodyOffset + head.contentLength))
                : nil
            timeout?.cancel()
            dispatched = true
            buffer = Data()
            let router = server.router
            let queue = server.queue
            Task.detached { [weak self] in
                let response = await router.handle(method: head.method, path: head.path, body: body)
                queue.async { self?.respond(response.status, response.body) }
            }
        }

        /// Host (rebinding), auth, then the JSON content-type rule. Order matters:
        /// an unauthenticated caller learns nothing beyond 401.
        private func admit(_ head: ControlRequestHead, _ options: ControlServerOptions) -> (Int, String)? {
            if !ControlAuth.hostAllowed(head.headers["host"], loopbackBind: options.isLoopback) {
                return (403, "Forbidden")
            }
            let isWs = head.isWebSocketUpgrade && ControlRouter.normalize(head.path) == "/ws"
            if !ControlAuth.isAuthorized(token: options.authToken, authorization: head.headers["authorization"],
                                         isWebSocketUpgrade: isWs, queryToken: head.query["token"]) {
                return (401, "Unauthorized")
            }
            if isWs && head.headers["origin"] != nil { return (403, "Forbidden") }
            if head.method == "POST" && ControlRouter.normalize(head.path) == "/invoke" {
                let type = (head.headers["content-type"] ?? "").split(separator: ";").first
                    .map { $0.trimmingCharacters(in: .whitespaces).lowercased() } ?? ""
                if type != "application/json" { return (415, "Content-Type must be application/json.") }
            }
            return nil
        }

        private func upgrade(_ head: ControlRequestHead) {
            guard let server, head.method == "GET", let key = head.headers["sec-websocket-key"], !key.isEmpty,
                  head.headers["sec-websocket-version"] == "13" else {
                return respond(400, ControlJSON.error("Bad WebSocket upgrade."))
            }
            timeout?.cancel()
            isWebSocket = true
            buffer.removeSubrange(buffer.startIndex..<(buffer.startIndex + bodyOffset))
            let handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                + "Sec-WebSocket-Accept: \(ControlWebSocket.acceptKey(key))\r\n\r\n"
            nw.send(content: Data(handshake.utf8), completion: .contentProcessed { _ in })
            // Send the current state immediately so a fresh client lights up.
            let router = server.router
            let queue = server.queue
            Task.detached { [weak self] in
                let state = await router.stateJSON()
                queue.async { self?.send(ControlWebSocket.text(state)) }
            }
            if !buffer.isEmpty { processFrames() }
        }

        private func processFrames() {
            while !finished {
                switch ControlWebSocket.parse(buffer) {
                case .incomplete:
                    return
                case .violation(let code):
                    nw.send(content: ControlWebSocket.close(code), completion: .contentProcessed { [weak self] _ in self?.close() })
                    finished = true
                    return
                case .data(let opcode, let payload, let consumed):
                    buffer.removeSubrange(buffer.startIndex..<(buffer.startIndex + consumed))
                    switch opcode {
                    case 0x8:   // close: complete the handshake
                        nw.send(content: ControlWebSocket.close(1000), completion: .contentProcessed { [weak self] _ in self?.close() })
                        finished = true
                    case 0x9:   // ping -> pong
                        send(ControlWebSocket.frame(opcode: 0xA, payload))
                    default:    // commands are not accepted over WS (Windows: drained)
                        break
                    }
                }
            }
        }

        func send(_ data: Data) {
            guard !finished else { return }
            nw.send(content: data, completion: .contentProcessed { _ in })
        }

        private func respond(_ status: Int, _ body: String) {
            guard !finished else { return }
            finished = true
            timeout?.cancel()
            nw.send(content: ControlHttpParser.response(status, body), isComplete: true,
                    completion: .contentProcessed { [weak self] _ in self?.close() })
        }

        func close() {
            finished = true
            timeout?.cancel()
            nw.stateUpdateHandler = nil
            nw.cancel()
            server?.forget(self)
        }
    }
}
