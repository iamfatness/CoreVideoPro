// ControlOscServer.swift — the OSC UDP transport (Windows OscControlServer +
// MainWindow gating). A BSD datagram socket on a DispatchSourceRead: the T16 spike
// found NWListener silently truncating a 65,000-byte datagram to 9,216 bytes
// (docs/superpowers/notes/2026-11-mac-osc-udp-spike.md). Replies and feedback leave
// FROM the bound port, which Companion matches on.
//
// Gating is Windows': loopback unless BOTH COREVIDEO_OSC_LAN=1 and
// COREVIDEO_OSC_TRUSTED_NETWORK=1; no token (OSC has none on either platform).
// Every datagram's sender becomes a feedback target (Windows records senders of
// junk too); the 16-target cap is Mac-only (Windows is unbounded).
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 19).

import Foundation

struct ControlOscOptions: Equatable {
    var port: UInt16 = 8010          // 0 = ephemeral (tests)
    var lan = false                  // binds 0.0.0.0 when true
    var feedbackTargetCap = 16
    var senderTtl: TimeInterval = 300

    static let loopbackNotice = "control: OSC remains on loopback. Unauthenticated LAN OSC requires "
        + "COREVIDEO_OSC_TRUSTED_NETWORK=1 on a trusted network."

    static func fromEnvironment(_ env: [String: String]) -> (options: ControlOscOptions, notice: String?) {
        var options = ControlOscOptions()
        if let raw = env["COREVIDEO_OSC_PORT"], let port = Int(raw), (1...65535).contains(port) {
            options.port = UInt16(port)
        }
        let wantsLan = env["COREVIDEO_OSC_LAN"] == "1"
        options.lan = wantsLan && env["COREVIDEO_OSC_TRUSTED_NETWORK"] == "1"
        return (options, wantsLan && !options.lan ? loopbackNotice : nil)
    }

    var bindAddress: String { lan ? "0.0.0.0" : "127.0.0.1" }
}

final class ControlOscServer: @unchecked Sendable {
    private let router: ControlOscRouter
    let options: ControlOscOptions
    private let now: () -> Date
    private let queue = DispatchQueue(label: "corevideo.control-osc")
    // Everything below is touched only on `queue`.
    private var fd: Int32 = -1
    private var source: DispatchSourceRead?
    private var targets: [String: (address: sockaddr_in, seen: Date)] = [:]
    private var routing: Task<Void, Never>?
    private var _lastRefusal: (actionId: String, error: String)?
    private var _lastRefusalAt: Date?
    private var lastLogAt = Date.distantPast
    private var suppressedLogs = 0

    init(router: ControlOscRouter, options: ControlOscOptions, now: @escaping () -> Date = Date.init) {
        self.router = router
        self.options = options
        self.now = now
    }

    func start(completion: @escaping (Result<UInt16, ControlServerError>) -> Void) {
        queue.async { [self] in completion(open()) }
    }

    private func open() -> Result<UInt16, ControlServerError> {
        let endpoint = "\(options.bindAddress):\(options.port)"
        let sock = socket(AF_INET, SOCK_DGRAM, 0)
        guard sock >= 0 else { return .failure(.listenFailed("OSC socket failed (errno \(errno))")) }
        var address = sockaddr_in()
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = options.port.bigEndian
        address.sin_addr.s_addr = inet_addr(options.bindAddress)
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0 else {
            let code = errno
            close(sock)
            return .failure(code == EADDRINUSE ? .portInUse(endpoint) : .listenFailed("could not bind OSC \(endpoint) (errno \(code))"))
        }
        var size: Int32 = 1 << 20   // a full 64 KiB datagram in and out (the default send cap is 9216)
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &size, socklen_t(MemoryLayout<Int32>.size))
        _ = fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK)
        var actual = sockaddr_in(); var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &actual) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(sock, $0, &length) } }
        fd = sock
        let source = DispatchSource.makeReadSource(fileDescriptor: sock, queue: queue)
        source.setEventHandler { [weak self] in self?.drain() }
        source.setCancelHandler { close(sock) }
        source.resume()
        self.source = source
        return .success(UInt16(bigEndian: actual.sin_port))
    }

    func stop() {
        queue.sync {
            source?.cancel()
            source = nil
            fd = -1
            routing?.cancel()
        }
    }

    private static func key(_ address: sockaddr_in) -> String {
        var addr = address.sin_addr
        var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &addr, &text, socklen_t(INET_ADDRSTRLEN))
        return "\(String(cString: text)):\(UInt16(bigEndian: address.sin_port))"
    }

    /// Reads every pending datagram (queue only).
    private func drain() {
        guard fd >= 0 else { return }
        var buffer = [UInt8](repeating: 0, count: ControlOscCodec.maxDatagram + 1)
        while true {
            var from = sockaddr_in(); var length = socklen_t(MemoryLayout<sockaddr_in>.size)
            let count = withUnsafeMutablePointer(to: &from) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buffer, buffer.count, 0, $0, &length) }
            }
            guard count >= 0 else { return }   // EAGAIN: nothing left
            remember(from)
            let messages = ControlOscCodec.decode(Data(buffer[0..<count]))   // oversize/junk -> []
            guard !messages.isEmpty else { continue }
            let sender = Self.key(from)
            // One chain, so commands run in arrival order. No reply is ever sent (Windows).
            let previous = routing
            routing = Task { [weak self] in
                await previous?.value
                for message in messages {
                    guard let self, let result = await self.router.route(message), !result.ok else { continue }
                    let id = ControlOscAddressMap.actionId(for: message.address) ?? message.address
                    self.queue.async { self.refused(id, result.error ?? "Invocation failed.", from: sender) }
                }
            }
        }
    }

    private func remember(_ address: sockaddr_in) {
        targets[Self.key(address)] = (address, now())
        while targets.count > options.feedbackTargetCap,
              let oldest = targets.min(by: { $0.value.seen < $1.value.seen })?.key {
            targets[oldest] = nil
        }
    }

    private func refused(_ id: String, _ error: String, from sender: String) {
        _lastRefusal = (id, error)
        _lastRefusalAt = Date()
        let at = Date()
        guard at.timeIntervalSince(lastLogAt) >= 1 else { suppressedLogs += 1; return }
        let suppressed = suppressedLogs > 0 ? " (\(suppressedLogs) more refusals suppressed)" : ""
        ShellLog.write("control: OSC refused \(id) from \(sender): \(error)\(suppressed)")
        lastLogAt = at
        suppressedLogs = 0
    }

    /// Each message is its own datagram, in order, to each current target, from the
    /// bound port. Targets older than senderTtl are pruned first. Best effort.
    func sendFeedback(_ messages: [ControlOscMessage]) {
        let packets = messages.map(ControlOscCodec.encode)
        queue.async { [self] in
            let cutoff = now().addingTimeInterval(-options.senderTtl)
            targets = targets.filter { $0.value.seen >= cutoff }
            guard fd >= 0 else { return }
            for (_, target) in targets {
                var to = target.address
                for packet in packets {
                    _ = packet.withUnsafeBytes { raw in
                        withUnsafePointer(to: &to) {
                            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                                sendto(fd, raw.baseAddress, packet.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                            }
                        }
                    }
                }
            }
        }
    }

    /// "ip:port", most recently seen first (tests, the Settings row).
    var feedbackTargets: [String] {
        queue.sync { targets.sorted { $0.value.seen > $1.value.seen }.map(\.key) }
    }

    var lastRefusal: (actionId: String, error: String)? { queue.sync { _lastRefusal } }
    var lastRefusalAt: Date? { queue.sync { _lastRefusalAt } }
}
