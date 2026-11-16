// ControlUdpSpikeTests.swift — RC4 decision spike (plan Task 16): can an
// NWListener over UDP receive from many senders on one port and reply FROM that
// port to each sender, the way Windows UdpClient does? Clients are BSD sockets so
// they see a reply's true source address. Each check is one expect line, so the
// harness output is the table. Kept only if Network.framework wins.
// Decision record: docs/superpowers/notes/2026-11-mac-osc-udp-spike.md.

import Foundation
import Network

extension ShellTests {
    /// Minimal NWListener UDP echo server.
    final class NWUdpEchoSpike: @unchecked Sendable {
        let queue = DispatchQueue(label: "control-udp-spike")
        private var listener: NWListener?
        private var connections: [NWConnection] = []
        private let lock = NSLock()
        private var _received = 0
        private var _largest = 0
        private(set) var listenerStates: [String] = []
        var received: Int { lock.withLock { _received } }
        var largest: Int { lock.withLock { _largest } }
        /// Mutation hooks (the spike proves its checks can fail).
        var dropOddClients = false
        var replyFrom: NWUdpEchoSpike?   // reply through another listener's connections

        func start() -> UInt16? {
            let parameters = NWParameters.udp
            parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: .any)
            guard let listener = try? NWListener(using: parameters) else { return nil }
            self.listener = listener
            let ready = DispatchSemaphore(value: 0)
            listener.stateUpdateHandler = { [weak self] state in
                self?.lock.withLock { self?.listenerStates.append("\(state)") }
                if case .ready = state { ready.signal() }
            }
            listener.newConnectionHandler = { [weak self] connection in
                guard let self else { return }
                self.lock.withLock { self.connections.append(connection) }
                connection.start(queue: self.queue)
                self.receive(connection)
            }
            listener.start(queue: queue)
            guard ready.wait(timeout: .now() + 2) == .success else { return nil }
            return listener.port?.rawValue
        }

        var isReady: Bool {
            if case .ready? = listener?.state { return true }
            return false
        }

        private func receive(_ connection: NWConnection) {
            connection.receiveMessage { [weak self] data, _, _, error in
                guard let self else { return }
                if let data {
                    self.lock.withLock { self._received += 1; self._largest = max(self._largest, data.count) }
                    let port = Self.remotePort(connection) ?? 0
                    if !(self.dropOddClients && port % 2 == 1) && data.count <= 1024 {
                        let target = self.replyFrom?.connection(toPort: port) ?? connection
                        target.send(content: data, completion: .contentProcessed { _ in })
                    }
                }
                if error == nil { self.receive(connection) }
            }
        }

        static func remotePort(_ connection: NWConnection) -> UInt16? {
            if case .hostPort(_, let port) = connection.endpoint { return port.rawValue }
            return nil
        }

        /// A connection to `port`, opened FROM this listener's port if none exists yet.
        func connection(toPort port: UInt16) -> NWConnection? {
            if let existing = lock.withLock({ connections.first { Self.remotePort($0) == port } }) { return existing }
            guard let local = listener?.port else { return nil }
            let parameters = NWParameters.udp
            parameters.allowLocalEndpointReuse = true
            parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: local)
            let connection = NWConnection(host: .ipv4(.loopback), port: NWEndpoint.Port(rawValue: port)!, using: parameters)
            connection.start(queue: queue)
            lock.withLock { connections.append(connection) }
            return connection
        }

        func send(_ data: Data, toPort port: UInt16) {
            connection(toPort: port)?.send(content: data, completion: .contentProcessed { _ in })
        }

        func stop() {
            listener?.cancel()
            lock.withLock { connections.forEach { $0.cancel() }; connections.removeAll() }
        }
    }

    /// A BSD datagram client on 127.0.0.1:<ephemeral>.
    final class SpikeUdpClient {
        let fd: Int32
        let port: UInt16
        init() {
            let sock = socket(AF_INET, SOCK_DGRAM, 0)
            fd = sock
            var addr = sockaddr_in()
            addr.sin_family = sa_family_t(AF_INET); addr.sin_addr.s_addr = inet_addr("127.0.0.1"); addr.sin_port = 0
            _ = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
            var bound = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            _ = withUnsafeMutablePointer(to: &bound) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(sock, $0, &len) } }
            port = UInt16(bigEndian: bound.sin_port)
            var tv = timeval(tv_sec: 0, tv_usec: 100_000)
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
            var size: Int32 = 1 << 20
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, socklen_t(MemoryLayout<Int32>.size))
        }
        @discardableResult
        func send(_ data: Data, to port: UInt16) -> Int {
            var to = sockaddr_in()
            to.sin_family = sa_family_t(AF_INET); to.sin_addr.s_addr = inet_addr("127.0.0.1"); to.sin_port = port.bigEndian
            return data.withUnsafeBytes { raw in
                withUnsafePointer(to: &to) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, raw.baseAddress, data.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
            }
        }
        /// Every datagram that arrives within `seconds`, with its source port.
        func drain(_ seconds: Double) -> [(Data, UInt16)] {
            var out: [(Data, UInt16)] = []
            let end = Date().addingTimeInterval(seconds)
            while Date() < end {
                var buf = [UInt8](repeating: 0, count: 65_536); var from = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
                let n = withUnsafeMutablePointer(to: &from) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buf, buf.count, 0, $0, &len) } }
                if n > 0 { out.append((Data(buf[0..<n]), UInt16(bigEndian: from.sin_port))) }
            }
            return out
        }
        private var closed = false
        func close() { if !closed { closed = true; Darwin.close(fd) } }
        deinit { close() }
    }

    @MainActor
    static func testControlUdpSpike() {
        let started = Date()
        let server = NWUdpEchoSpike()
        guard let port = server.start() else { expect(false, "spike listener starts"); return }
        defer { server.stop() }
        let other = NWUdpEchoSpike()   // mutation hook target only
        _ = other.start(); defer { other.stop() }

        // 1 + 2: sixteen senders, one datagram each; each gets exactly one reply, from the listener's port.
        let clients = (0..<16).map { _ in SpikeUdpClient() }
        for (i, c) in clients.enumerated() { c.send(Data("hello-\(i)".utf8), to: port) }
        Thread.sleep(forTimeInterval: 1)   // every reply must have landed within 1 s
        let replies = clients.map { $0.drain(0.12) }
        let ports = replies.flatMap { $0.map(\.1) }
        expect(!ports.isEmpty && ports.allSatisfy { $0 == port }, "1: reply source port == listener port (got \(Set(ports)) vs \(port))")
        let counts = replies.map(\.count)
        expect(counts.allSatisfy { $0 == 1 }, "2: 16 senders each receive exactly 1 reply within 1 s (counts \(counts))")

        // 3: server-initiated sends on an existing sender's connection.
        let target = clients[0]
        Thread.sleep(forTimeInterval: 0.2)
        for i in 0..<3 { server.send(Data("push-\(i)".utf8), toPort: target.port) }
        let pushed = target.drain(0.5)
        expect(pushed.count == 3 && pushed.allSatisfy { $0.1 == port }, "3: 3 unsolicited datagrams arrive from the listener port (got \(pushed.count))")

        // 4: a dead sender (ICMP unreachable) does not wedge the listener.
        let a = SpikeUdpClient(), b = SpikeUdpClient()
        a.send(Data("a".utf8), to: port); _ = a.drain(0.2)
        let deadPort = a.port
        a.close()
        server.send(Data("to-dead".utf8), toPort: deadPort)
        Thread.sleep(forTimeInterval: 0.2)
        b.send(Data("b".utf8), to: port)
        let bReply = b.drain(0.5)
        expect(bReply.count == 1 && server.isReady, "4: after a dead sender, B still gets its reply and the listener stays ready (\(bReply.count), ready \(server.isReady))")

        // 5: a burst of 1000 x 64 bytes from one client.
        let burst = SpikeUdpClient()
        let before = server.received
        for _ in 0..<1000 { burst.send(Data(count: 64), to: port) }
        let burstEnd = Date().addingTimeInterval(2)
        while server.received - before < 1000 && Date() < burstEnd { Thread.sleep(forTimeInterval: 0.02) }
        let got = server.received - before
        expect(got >= 990, "5: >= 990 of 1000 burst datagrams received (got \(got))")

        // 6: a 65,000-byte datagram is received intact or refused; a normal one still echoes after.
        let big = SpikeUdpClient()
        let sent = big.send(Data(count: 65_000), to: port)
        Thread.sleep(forTimeInterval: 0.2)
        big.send(Data("after".utf8), to: port)
        let after = big.drain(0.5)
        expect((server.largest == 65_000 || sent < 0) && after.count == 1 && server.isReady,
               "6: oversize received intact (\(server.largest) bytes, send \(sent)) or refused, and a normal datagram still echoes (\(after.count))")
        ShellLog.write("control: udp spike run took \(Int(Date().timeIntervalSince(started) * 1000)) ms")
        FileHandle.standardError.write("control: udp spike largest=\(server.largest) burst=\(got) took \(Int(Date().timeIntervalSince(started) * 1000)) ms\n".data(using: .utf8)!)
    }
}
