// RemoteControlStatus.swift — the read-only Settings "Remote control" row (D4):
// what the control servers are doing, never an editor (ports and token stay
// environment-only, as on Windows). Plan Task 20.

import Foundation

struct RemoteControlStatus: Equatable {
    var httpEndpoint: String?          // "127.0.0.1:8011", nil = not running
    var httpError: String?
    var oscEndpoint: String?           // "127.0.0.1:8010"
    var oscError: String?
    var lan: Bool                      // either transport bound beyond loopback
    var tokenRequired: Bool
    var webSocketClients: Int
    var oscTargets: Int
    var lastRefusal: String?           // "<actionId>: <error>" (HTTP 400/422 or OSC), most recent

    static let idle = RemoteControlStatus(httpEndpoint: nil, httpError: nil, oscEndpoint: nil, oscError: nil,
                                          lan: false, tokenRequired: false, webSocketClients: 0, oscTargets: 0,
                                          lastRefusal: nil)

    /// The formatter the Settings view renders, in order.
    var lines: [(label: String, value: String)] {
        func endpoint(_ value: String?, _ error: String?) -> String { value ?? "Off — \(error ?? "not running")" }
        let network = !lan ? "Loopback only" : (tokenRequired ? "LAN (token required)" : "LAN (OSC, no token)")
        return [
            ("HTTP", endpoint(httpEndpoint, httpError)),
            ("OSC", endpoint(oscEndpoint, oscError)),
            ("Network", network),
            ("Clients", "\(webSocketClients) WebSocket, \(oscTargets) OSC"),
            ("Last refused", lastRefusal ?? "None"),
        ]
    }
}

/// The last refused `/invoke` (status 400 or 422), readable from any thread.
final class ControlRefusalBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value: (actionId: String, error: String, at: Date)?

    func record(_ actionId: String, _ error: String) { lock.withLock { value = (actionId, error, Date()) } }
    func read() -> (actionId: String, error: String, at: Date)? { lock.withLock { value } }
}
