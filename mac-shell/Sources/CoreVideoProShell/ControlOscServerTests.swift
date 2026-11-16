// ControlOscServerTests.swift — the OSC UDP transport over real loopback sockets
// (twin of CT OscControlServerTests, extended). Every server binds an EPHEMERAL
// port: nothing here may touch 8010/8011. Plan Task 19.

import Foundation

extension ShellTests {
    /// A BSD datagram client on 127.0.0.1:<ephemeral> (sees a reply's true source port).
    final class UdpClient {
        let fd: Int32
        let port: UInt16
        init() {
            let sock = socket(AF_INET, SOCK_DGRAM, 0)
            fd = sock
            var addr = sockaddr_in(); addr.sin_family = sa_family_t(AF_INET); addr.sin_addr.s_addr = inet_addr("127.0.0.1"); addr.sin_port = 0
            _ = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
            var bound = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            _ = withUnsafeMutablePointer(to: &bound) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(sock, $0, &len) } }
            port = UInt16(bigEndian: bound.sin_port)
            var tv = timeval(tv_sec: 0, tv_usec: 100_000); setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
            var size: Int32 = 1 << 20; setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &size, socklen_t(MemoryLayout<Int32>.size))
        }
        func send(_ data: Data, to port: UInt16) {
            var to = sockaddr_in(); to.sin_family = sa_family_t(AF_INET); to.sin_addr.s_addr = inet_addr("127.0.0.1"); to.sin_port = port.bigEndian
            data.withUnsafeBytes { raw in _ = withUnsafePointer(to: &to) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(fd, raw.baseAddress, data.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } } }
        }
        /// Every datagram that arrives within `seconds`, with its source port.
        func drain(_ seconds: Double) -> [(Data, UInt16)] {
            var out: [(Data, UInt16)] = []; let end = Date().addingTimeInterval(seconds)
            while Date() < end {
                var buf = [UInt8](repeating: 0, count: 65_536); var from = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
                let n = withUnsafeMutablePointer(to: &from) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buf, buf.count, 0, $0, &len) } }
                if n > 0 { out.append((Data(buf[0..<n]), UInt16(bigEndian: from.sin_port))) }
            }
            return out
        }
        deinit { close(fd) }
    }

    @MainActor
    static var controlOscServerCases: [(String, () -> Void)] {
        [
            ("control/osc-server-end-to-end", testControlOscServerEndToEnd),
            ("control/osc-targets-cap-and-expiry", testControlOscTargetsCapAndExpiry),
            ("control/osc-port-in-use-never-blocks-http", testControlOscPortInUseNeverBlocksHttp),
        ]
    }

    @MainActor
    static func testControlOscServerEndToEnd() {
        let surface = FakeSurface()
        let server = ControlOscServer(router: ControlOscRouter(surface: surface), options: ControlOscOptions(port: 0))
        let box = Box<Result<UInt16, ControlServerError>>()
        server.start { box.value = $0 }
        spin(until: { box.value != nil }, limit: 3)
        defer { server.stop() }
        guard case .success(let port)? = box.value, port != 0 else { expect(false, "OSC server starts on an ephemeral loopback port"); return }
        let client = UdpClient()
        client.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/transport/take", args: [])), to: port)
        spin(until: { surface.invocations.count == 1 }, limit: 3)
        expectEqual(surface.invocations.first?.0, "transport.take", "a datagram invokes the action")
        expect(client.drain(0.3).isEmpty, "a command gets no reply (Windows sends none)")
        server.sendFeedback(ControlOscFeedback.encode(["recording": true]))
        let got = client.drain(0.5)
        let decoded = got.flatMap { ControlOscCodec.decode($0.0) }
        expect(decoded.contains(ControlOscMessage(address: "/cvp/state/recording", args: [.int(1)])), "feedback reaches the sender")
        expect(!got.isEmpty && got.allSatisfy { $0.1 == port }, "feedback comes FROM the OSC port (Companion matches on it)")
        // Junk is ignored, and its sender still becomes a target (Windows records every sender).
        let junk = UdpClient()
        junk.send(Data([1, 2, 3]), to: port)
        spin(until: { server.feedbackTargets.count == 2 }, limit: 2)
        expectEqual(server.feedbackTargets.count, 2, "both senders are targets")
        // An unavailable action is refused, logged, never answered.
        client.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/showEngine/restart", args: [])), to: port)
        spin(until: { server.lastRefusal != nil }, limit: 2)
        expectEqual(server.lastRefusal?.actionId, "showEngine.restart", "refusal recorded")
        expectEqual(surface.invocations.count, 1, "the refused action never reached the surface")
        // Oversize is dropped without harm.
        client.send(Data(count: 65_000), to: port)
        client.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/transport/take", args: [])), to: port)
        spin(until: { surface.invocations.count == 2 }, limit: 3)
        expectEqual(surface.invocations.count, 2, "still serving after an oversize datagram")
    }

    @MainActor
    static func testControlOscTargetsCapAndExpiry() {
        var clock = Date(timeIntervalSince1970: 1_000)
        var options = ControlOscOptions(port: 0); options.feedbackTargetCap = 3; options.senderTtl = 300
        let server = ControlOscServer(router: ControlOscRouter(surface: FakeSurface()), options: options, now: { clock })
        let box = Box<Result<UInt16, ControlServerError>>()
        server.start { box.value = $0 }
        spin(until: { box.value != nil }, limit: 3)
        defer { server.stop() }
        guard case .success(let port)? = box.value else { expect(false, "starts"); return }
        let clients = (0..<4).map { _ in UdpClient() }
        for (i, c) in clients.enumerated() {
            clock = clock.addingTimeInterval(1)
            c.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/nope", args: [])), to: port)
            spin(until: { server.feedbackTargets.count == min(i + 1, 3) && (i < 3 || !server.feedbackTargets.contains("127.0.0.1:\(clients[0].port)")) }, limit: 2)
        }
        expectEqual(server.feedbackTargets.count, 3, "capped at 3")
        expect(!server.feedbackTargets.contains("127.0.0.1:\(clients[0].port)"), "the least recently seen sender was evicted")
        clock = clock.addingTimeInterval(301)
        server.sendFeedback([ControlOscMessage(address: "/cvp/state/recording", args: [.int(0)])])
        expectEqual(server.feedbackTargets.count, 0, "senders older than 5 minutes are pruned on send")
        let (o1, n1) = ControlOscOptions.fromEnvironment(["COREVIDEO_OSC_LAN": "1"])
        expect(!o1.lan, "LAN needs both variables (D2)")
        expectEqual(n1, "control: OSC remains on loopback. Unauthenticated LAN OSC requires COREVIDEO_OSC_TRUSTED_NETWORK=1 on a trusted network.", "Windows log line")
        let (o2, n2) = ControlOscOptions.fromEnvironment(["COREVIDEO_OSC_LAN": "1", "COREVIDEO_OSC_TRUSTED_NETWORK": "1", "COREVIDEO_OSC_PORT": "9010"])
        expect(o2.lan && o2.port == 9010 && n2 == nil, "LAN opt-in and port")
        expectEqual(ControlOscOptions.fromEnvironment(["COREVIDEO_OSC_PORT": "70000"]).options.port, 8010, "an out-of-range port keeps 8010")
    }

    @MainActor
    static func testControlOscPortInUseNeverBlocksHttp() {
        let first = ControlOscServer(router: ControlOscRouter(surface: FakeSurface()), options: ControlOscOptions(port: 0))
        let a = Box<Result<UInt16, ControlServerError>>(); first.start { a.value = $0 }; spin(until: { a.value != nil }, limit: 3)
        defer { first.stop() }
        guard case .success(let port)? = a.value else { expect(false, "first starts"); return }
        let second = ControlOscServer(router: ControlOscRouter(surface: FakeSurface()), options: ControlOscOptions(port: port))
        let b = Box<Result<UInt16, ControlServerError>>(); second.start { b.value = $0 }; spin(until: { b.value != nil }, limit: 3)
        second.stop()
        if case .failure(.portInUse)? = b.value { expect(true, "port in use reported") } else { expect(false, "a taken OSC port is port-in-use: \(String(describing: b.value))") }
        // The host keeps HTTP up when OSC fails: drive ControlApiHost with a taken OSC port.
        let model = sceneEditModel()
        let host = ControlApiHost()
        host.startForTesting(model: model, http: ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil), osc: ControlOscOptions(port: port))
        defer { host.stop() }
        spin(until: { host.httpPortForTesting != nil && host.oscFailedForTesting }, limit: 3)
        expect(host.oscFailedForTesting, "OSC failure observed")
        expectEqual(http("GET", "http://127.0.0.1:\(host.httpPortForTesting ?? 0)/state").status, 200, "HTTP still serving")
        expect(model.warnings.contains { $0.contains("OSC control unavailable") }, "the operator is told, once")
    }
}
