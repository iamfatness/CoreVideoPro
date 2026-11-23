// Twitch sign-in for social review: ports of
// native-shell/CoreVideoPro.MediaCore/Services/TwitchHttpTransport.cs and
// TwitchAuthorization.cs. Device-code grant only (no client secret, no
// redirect URI), scope user:read:chat only, validation at startup and then
// hourly, single-use refresh tokens rotated and stored atomically BEFORE use,
// a changed user id treated as an authorization failure that clears the
// store. The client id is user-entered (blank reuses the stored one). The
// whole credential set is one JSON blob in the login Keychain
// (SocialCredentials.twitch); nothing touches disk.

import Foundation

enum TwitchFailure: Error, Equatable, CustomStringConvertible {
    case invalid, authorization, retry, revoked

    /// TwitchChatException messages, verbatim.
    var message: String {
        switch self {
        case .authorization: return "Twitch authorization is unavailable. Sign in again."
        case .revoked: return "Twitch removed chat access. Review decisions cleared."
        case .retry: return "Twitch chat disconnected. Review decisions cleared."
        case .invalid: return "Twitch returned an unexpected response. Review decisions cleared."
        }
    }
    var description: String { "\(self == .invalid ? "invalid" : self == .authorization ? "authorization" : self == .retry ? "retry" : "revoked")" }
}

/// One user's Twitch sign-in. `description` never prints a token.
struct TwitchCredentials: Codable, Equatable, CustomStringConvertible, CustomDebugStringConvertible {
    var clientId: String
    var accessToken: String
    var refreshToken: String
    var expiresAt: Date
    var userId: String = ""
    var login: String = ""
    var description: String { "Twitch credentials" }
    var debugDescription: String { "Twitch credentials" }
}

/// The JSON helpers Twitch shares (TwitchJson).
enum TwitchJSON {
    static func identifier(_ s: String) -> Bool { (1...256).contains(s.utf16.count) && s.utf16.allSatisfy { (0x21...0x7E).contains($0) } }
    static func userId(_ s: String) -> Bool { (1...32).contains(s.utf16.count) && s.unicodeScalars.allSatisfy { ("0"..."9").contains($0) } }
    static func text(_ o: JSONObject?, _ key: String) -> String { o?[key] as? String ?? "" }

    /// Parse with the 65536-byte cap and a depth bound (TwitchJson.Parse).
    static func parse(_ data: Data) throws -> JSONObject {
        guard data.count <= 65536, let root = (try? JSONSerialization.jsonObject(with: data)) as? JSONObject,
              depth(root) <= 24 else { throw TwitchFailure.invalid }
        return root
    }

    static func depth(_ value: Any, _ level: Int = 1) -> Int {
        if let o = value as? JSONObject { return o.values.map { depth($0, level + 1) }.max() ?? level }
        if let a = value as? [Any] { return a.map { depth($0, level + 1) }.max() ?? level }
        return level
    }
}

enum TwitchHttpTransport {
    static let readScope = "user:read:chat"
    static let deadline: TimeInterval = 12

    struct TokenPair: Equatable { var accessToken: String; var refreshToken: String; var expiresIn: Int }
    struct DeviceGrant { var deviceCode: String; var prompt: TwitchDevicePrompt; var expiresAt: Date; var interval: Int }
    struct Identity: Equatable { var userId: String; var login: String; var expiresIn: Int }
    struct Pending: Error { let slowDown: Bool }

    static func token(_ s: String) -> Bool { (10...4096).contains(s.utf16.count) && s.utf16.allSatisfy { (0x21...0x7E).contains($0) } }
    static func login(_ s: String) -> Bool { SocialFeedRunners.isChannel(s) }
    static func clientId(_ s: String) -> Bool { SocialFeedRunners.isClientId(s) }

    /// verification_uri must be exactly https://www.twitch.tv/activate on 443,
    /// with no user info and no fragment.
    static func verificationUri(_ text: String) -> URL? {
        guard text.utf16.count <= 2048, let c = URLComponents(string: text), c.scheme == "https",
              c.host?.lowercased() == "www.twitch.tv", c.port == nil || c.port == 443, c.path == "/activate",
              c.user == nil, c.password == nil, c.fragment == nil, let url = c.url else { return nil }
        return url
    }

    static func begin(clientId: String, now: Date, http: SocialHTTP) async throws -> DeviceGrant {
        let root = try await request(form("device", [("client_id", clientId), ("scopes", readScope)]), http: http)
        let device = TwitchJSON.text(root, "device_code"), code = TwitchJSON.text(root, "user_code")
        guard token(device), TwitchJSON.identifier(code), code.utf16.count <= 32,
              let uri = verificationUri(TwitchJSON.text(root, "verification_uri")) else { throw TwitchFailure.invalid }
        let expires = try positive(root, "expires_in"), interval = try positive(root, "interval")
        guard expires <= 3600, interval <= 60 else { throw TwitchFailure.invalid }
        return DeviceGrant(deviceCode: device, prompt: TwitchDevicePrompt(userCode: code, verificationUri: uri),
                           expiresAt: now.addingTimeInterval(TimeInterval(expires)), interval: interval)
    }

    static func poll(clientId: String, deviceCode: String, http: SocialHTTP) async throws -> TokenPair {
        let root = try await request(form("token", [("client_id", clientId), ("scopes", readScope), ("device_code", deviceCode),
                                                    ("grant_type", "urn:ietf:params:oauth:grant-type:device_code")]),
                                     http: http, pending: true)
        return try tokens(root)
    }

    /// No client secret: the device-flow public client refreshes with its id only.
    static func refresh(_ c: TwitchCredentials, http: SocialHTTP) async throws -> TokenPair {
        try tokens(try await request(form("token", [("client_id", c.clientId), ("refresh_token", c.refreshToken),
                                                    ("grant_type", "refresh_token")]), http: http))
    }

    static func validate(_ c: TwitchCredentials, http: SocialHTTP) async throws -> Identity {
        guard clientId(c.clientId), token(c.accessToken), token(c.refreshToken),
              let url = URL(string: "https://id.twitch.tv/oauth2/validate") else { throw TwitchFailure.authorization }
        let root = try await request(SocialHTTPRequest(method: "GET", url: url, headers: ["Authorization": "OAuth " + c.accessToken]),
                                     http: http)
        let user = TwitchJSON.text(root, "user_id"), name = TwitchJSON.text(root, "login")
        guard TwitchJSON.text(root, "client_id") == c.clientId, TwitchJSON.userId(user), login(name), hasReadScope(root, "scopes")
        else { throw TwitchFailure.authorization }
        return Identity(userId: user, login: name, expiresIn: try positive(root, "expires_in"))
    }

    static func channel(_ name: String, credentials c: TwitchCredentials, http: SocialHTTP) async throws -> String {
        guard login(name) else { throw TwitchFailure.invalid }
        let root = try await request(helix("GET", "users?login=" + YouTubeChatTransport.escape(name.lowercased()), c), http: http)
        guard let rows = root["data"] as? [Any], rows.count == 1, let row = rows[0] as? JSONObject,
              TwitchJSON.text(row, "login").lowercased() == name.lowercased(), TwitchJSON.userId(TwitchJSON.text(row, "id"))
        else { throw TwitchFailure.invalid }
        return TwitchJSON.text(row, "id")
    }

    // ── internals ────────────────────────────────────────────────────────

    /// application/x-www-form-urlencoded as FormUrlEncodedContent writes it.
    static func formBody(_ fields: [(String, String)]) -> Data {
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~ ")
        func enc(_ s: String) -> String {
            (s.addingPercentEncoding(withAllowedCharacters: allowed) ?? s).replacingOccurrences(of: " ", with: "+")
        }
        return Data(fields.map { enc($0.0) + "=" + enc($0.1) }.joined(separator: "&").utf8)
    }

    private static func form(_ path: String, _ fields: [(String, String)]) -> SocialHTTPRequest {
        SocialHTTPRequest(method: "POST", url: URL(string: "https://id.twitch.tv/oauth2/" + path)!,
                          headers: ["Content-Type": "application/x-www-form-urlencoded"], body: formBody(fields))
    }

    static func helix(_ method: String, _ path: String, _ c: TwitchCredentials, body: Data? = nil) -> SocialHTTPRequest {
        var headers = ["Authorization": "Bearer " + c.accessToken, "Client-Id": c.clientId]
        if body != nil { headers["Content-Type"] = "application/json; charset=utf-8" }
        return SocialHTTPRequest(method: method, url: URL(string: "https://api.twitch.tv/helix/" + path)!, headers: headers, body: body)
    }

    static func request(_ r: SocialHTTPRequest, http: SocialHTTP, pending: Bool = false) async throws -> JSONObject {
        let response: SocialHTTPResponse
        do { response = try await http.send(r, deadline: deadline) }
        catch SocialFailure.invalid { throw TwitchFailure.invalid }
        catch { throw TwitchFailure.retry }
        if (300..<400).contains(response.status) { throw TwitchFailure.authorization }
        guard response.body.count <= URLSessionSocialHTTP.bodyLimit else { throw TwitchFailure.invalid }
        let root = try TwitchJSON.parse(response.body)
        if (200..<300).contains(response.status) { return root }
        let message = TwitchJSON.text(root, "message")
        if pending, response.status == 400, message == "authorization_pending" || message == "slow_down" {
            throw Pending(slowDown: message == "slow_down")
        }
        throw response.status >= 500 || response.status == 429 ? TwitchFailure.retry : TwitchFailure.authorization
    }

    private static func tokens(_ root: JSONObject) throws -> TokenPair {
        let access = TwitchJSON.text(root, "access_token"), refresh = TwitchJSON.text(root, "refresh_token")
        guard token(access), token(refresh), TwitchJSON.text(root, "token_type") == "bearer", hasReadScope(root, "scope")
        else { throw TwitchFailure.authorization }
        return TokenPair(accessToken: access, refreshToken: refresh, expiresIn: try positive(root, "expires_in"))
    }

    private static func hasReadScope(_ root: JSONObject, _ key: String) -> Bool {
        guard let scopes = root[key] as? [Any], scopes.count <= 32 else { return false }
        return scopes.contains { $0 as? String == readScope }
    }

    private static func positive(_ root: JSONObject, _ key: String) throws -> Int {
        guard let n = root[key] as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID(), n.doubleValue == n.doubleValue.rounded(),
              n.doubleValue > 0, n.doubleValue <= Double(Int32.max) else { throw TwitchFailure.invalid }
        return n.intValue
    }
}

/// One review owner's serialised authorization and rotating token pair.
@MainActor
final class TwitchAuthorization {
    typealias Read = () -> TwitchCredentials?
    typealias Save = (TwitchCredentials) -> Bool
    typealias Clear = () -> Void

    private let http: SocialHTTP
    private let read: Read
    private let save: Save
    private let clearStore: Clear
    private let now: () -> Date
    private let sleep: (UInt64) async -> Void
    private let rotation = SocialAsyncLock()
    private var current: TwitchCredentials?
    private var validatedAt: Date = .distantPast

    init(http: SocialHTTP, read: @escaping Read, save: @escaping Save, clear: @escaping Clear,
         now: @escaping () -> Date = Date.init, sleep: @escaping (UInt64) async -> Void = { try? await Task.sleep(nanoseconds: $0) }) {
        self.http = http; self.read = read; self.save = save; self.clearStore = clear; self.now = now; self.sleep = sleep
    }

    /// Production: the Keychain blob (SocialCredentials.twitch).
    static func keychain(http: SocialHTTP = URLSessionSocialHTTP()) -> TwitchAuthorization {
        TwitchAuthorization(http: http, read: {
            SocialCredentials.twitch.read().flatMap { try? Self.decoder.decode(TwitchCredentials.self, from: $0) }
        }, save: { credentials in
            guard let data = try? Self.encoder.encode(credentials) else { return false }
            return SocialCredentials.twitch.write(data)   // one item: access + refresh together
        }, clear: { SocialCredentials.twitch.delete() })
    }

    nonisolated static let encoder: JSONEncoder = { let e = JSONEncoder(); e.dateEncodingStrategy = .secondsSince1970; return e }()
    nonisolated static let decoder: JSONDecoder = { let d = JSONDecoder(); d.dateDecodingStrategy = .secondsSince1970; return d }()

    static func isChannel(_ s: String) -> Bool { TwitchHttpTransport.login(s) }
    static func isClientId(_ s: String) -> Bool { TwitchHttpTransport.clientId(s) }

    func authorize(clientId requested: String, prompt: @escaping (TwitchDevicePrompt) -> Void) async throws -> TwitchCredentials {
        await rotation.acquire()
        defer { rotation.release() }
        let stored = read()
        let clientId = requested.isEmpty ? stored?.clientId ?? "" : requested
        guard TwitchHttpTransport.clientId(clientId) else { throw TwitchFailure.authorization }
        if let stored, stored.clientId == clientId {
            current = stored
            do { return try await maintainLocked(force: true) }
            catch TwitchFailure.authorization { current = nil; clearStore() }
        }
        let grant = try await TwitchHttpTransport.begin(clientId: clientId, now: now(), http: http)
        prompt(grant.prompt)
        var interval = grant.interval
        while now() < grant.expiresAt {
            await sleep(UInt64(interval) * 1_000_000_000)
            try Task.checkCancellation()
            if now() >= grant.expiresAt { break }
            do {
                let pair = try await TwitchHttpTransport.poll(clientId: clientId, deviceCode: grant.deviceCode, http: http)
                let fresh = TwitchCredentials(clientId: clientId, accessToken: pair.accessToken, refreshToken: pair.refreshToken,
                                              expiresAt: now().addingTimeInterval(TimeInterval(pair.expiresIn)))
                return try await validateAndSave(fresh)
            } catch let pending as TwitchHttpTransport.Pending {
                if pending.slowDown { interval = min(60, interval + 5) }
            }
        }
        throw TwitchFailure.authorization
    }

    /// Refresh at <= 60 s left, validate hourly (or when forced). Serialised:
    /// concurrent callers share one single-use refresh.
    func maintain(force: Bool = false) async throws -> TwitchCredentials {
        await rotation.acquire()
        defer { rotation.release() }
        do { return try await maintainLocked(force: force) }
        catch TwitchFailure.authorization {
            current = nil
            clearStore()
            throw TwitchFailure.authorization
        }
    }

    func clear() {
        current = nil
        clearStore()
    }

    private func maintainLocked(force: Bool) async throws -> TwitchCredentials {
        guard var credentials = current else { throw TwitchFailure.authorization }
        var force = force
        let t = now()
        if credentials.expiresAt.timeIntervalSince(t) <= 60 {
            let pair = try await TwitchHttpTransport.refresh(credentials, http: http)
            credentials.accessToken = pair.accessToken
            credentials.refreshToken = pair.refreshToken
            credentials.expiresAt = t.addingTimeInterval(TimeInterval(pair.expiresIn))
            // Device-flow refresh tokens are single-use: commit the complete rotated
            // pair before any later request can try the previous refresh token.
            guard save(credentials) else { throw TwitchFailure.authorization }
            current = credentials
            force = true
        }
        if force || now().timeIntervalSince(validatedAt) >= 3600 { return try await validateAndSave(credentials) }
        return credentials
    }

    private func validateAndSave(_ credentials: TwitchCredentials) async throws -> TwitchCredentials {
        let identity = try await TwitchHttpTransport.validate(credentials, http: http)
        if !credentials.userId.isEmpty, credentials.userId != identity.userId { throw TwitchFailure.authorization }
        var validated = credentials
        validated.userId = identity.userId
        validated.login = identity.login
        validated.expiresAt = now().addingTimeInterval(TimeInterval(identity.expiresIn))
        guard save(validated) else { throw TwitchFailure.authorization }
        current = validated
        validatedAt = now()
        return validated
    }
}
