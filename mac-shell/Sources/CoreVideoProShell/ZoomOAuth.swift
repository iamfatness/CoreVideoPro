// ZoomOAuth — the mac port of ZoomOAuthService (broker mode only). The
// broker (corevideo.iamfatness.us, site-worker.js) owns PKCE and the token
// exchange; the shell opens the browser, receives corevideo://oauth/callback,
// redeems the broker_token, and keeps Zoom tokens in the KEYCHAIN under this
// app's OWN service string. HARD RULES: never touch the OBS plugin's
// "CoreVideo OBS Plugin" Keychain item (Zoom rotates refresh tokens — a
// second reader signs the plugin out); never clobber a live refresh token
// with an empty one; refresh is single-flight (the actor) because a rotated
// token issued twice invalidates the first.

import AppKit
import Foundation
import Security

actor ZoomOAuth {
    static let shared = ZoomOAuth()

    static let brokerStart = "https://corevideo.iamfatness.us/oauth/start"
    static let brokerBase = "https://corevideo.iamfatness.us"
    static let appReturnUri = "corevideo://oauth/callback"

    struct Tokens: Codable {
        var accessToken = ""
        var refreshToken = ""
        var expiresAt: Double = 0
    }

    struct JoinCredentials {
        var userZak: String?
        var displayName: String?
    }

    private var pendingState: String?
    private var tokens: Tokens?
    private var refreshInFlight: Task<Tokens?, Never>?

    // ── sign-in flow ─────────────────────────────────────────────────────────

    func beginSignIn() {
        var stateBytes = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, stateBytes.count, &stateBytes)
        let state = Data(stateBytes).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
        pendingState = state
        var components = URLComponents(string: Self.brokerStart)!
        components.queryItems = [
            URLQueryItem(name: "state", value: state),
            URLQueryItem(name: "redirect_uri", value: Self.brokerBase + "/oauth/callback"),
            URLQueryItem(name: "return_uri", value: Self.appReturnUri),
        ]
        NSWorkspace.shared.open(components.url!)
    }

    // Returns a status string for the UI ("" = success).
    func handleCallback(_ url: URL) async -> String {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false)
        else { return "malformed callback URL" }
        let query = { (name: String) in
            components.queryItems?.first { $0.name == name }?.value
        }
        if let error = query("error"), !error.isEmpty {
            pendingState = nil
            return "Zoom sign-in failed: \(error)"
        }
        guard let state = query("state"), state == pendingState else {
            return "sign-in state mismatch — start again"
        }
        pendingState = nil
        guard let brokerToken = query("broker_token"), !brokerToken.isEmpty else {
            return "callback carried no broker token"
        }
        do {
            let response = try await postJson(
                Self.brokerBase + "/oauth/redeem", body: ["broker_token": brokerToken])
            saveTokenResponse(response)
            return ""
        } catch {
            return "token redeem failed: \(error.localizedDescription)"
        }
    }

    func signOut() {
        tokens = nil
        keychainDelete()
    }

    var signedIn: Bool {
        loadTokens() != nil
    }

    // ── join credentials (fresh ZAK per join, never cached) ──────────────────

    func ensureJoinCredentials() async -> JoinCredentials? {
        guard var current = loadTokens() else { return nil }
        if current.expiresAt <= Date().timeIntervalSince1970 {
            guard let refreshed = await refresh(current) else { return nil }
            current = refreshed
        }
        // ZAK: direct to Zoom with the access token; field name is "token".
        var request = URLRequest(
            url: URL(string: "https://api.zoom.us/v2/users/me/token?type=zak")!)
        request.setValue("Bearer \(current.accessToken)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200,
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let zak = object["token"] as? String, !zak.isEmpty
        else {
            // One refresh retry: the access token may have been revoked early.
            if let refreshed = await refresh(current) {
                return await zakWith(refreshed)
            }
            return nil
        }
        return JoinCredentials(userZak: zak, displayName: nil)
    }

    private func zakWith(_ tokens: Tokens) async -> JoinCredentials? {
        var request = URLRequest(
            url: URL(string: "https://api.zoom.us/v2/users/me/token?type=zak")!)
        request.setValue("Bearer \(tokens.accessToken)", forHTTPHeaderField: "Authorization")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200,
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let zak = object["token"] as? String, !zak.isEmpty
        else { return nil }
        return JoinCredentials(userZak: zak, displayName: nil)
    }

    // Single-flight: concurrent callers await the SAME refresh — Zoom rotates
    // the refresh token on use, so two parallel refreshes sign the user out.
    private func refresh(_ current: Tokens) async -> Tokens? {
        if let inFlight = refreshInFlight {
            return await inFlight.value
        }
        let task = Task<Tokens?, Never> {
            do {
                let response = try await postJson(
                    Self.brokerBase + "/oauth/refresh",
                    body: ["refresh_token": current.refreshToken])
                return saveTokenResponse(response)
            } catch {
                return nil  // NEVER delete tokens on a failed refresh
            }
        }
        refreshInFlight = task
        let result = await task.value
        refreshInFlight = nil
        return result
    }

    @discardableResult
    /// PURE token merge, extracted so the rule that protects the owner's OBS
    /// plugin can be tested without a network or a Keychain.
    ///
    /// Zoom ROTATES refresh tokens on use. If this ever writes an empty refresh
    /// token over a stored one, the next refresh has nothing to present and the
    /// account is signed out — and because the OBS plugin authenticates the same
    /// Zoom account, a bad write here logs the owner out of a tool they use in
    /// production. A response that omits `refresh_token` means "no rotation,
    /// keep what you have", NOT "you have none".
    static func mergeTokenResponse(_ response: [String: Any],
                                   existingRefreshToken: String,
                                   now: Double) -> Tokens {
        var next = Tokens()
        next.accessToken = response["access_token"] as? String ?? ""
        next.refreshToken = response["refresh_token"] as? String ?? ""
        let expiresIn = (response["expires_in"] as? NSNumber)?.doubleValue ?? 3600
        // 60s of slack: a token that expires mid-join is a failed show.
        next.expiresAt = now + expiresIn - 60
        if next.refreshToken.isEmpty {
            next.refreshToken = existingRefreshToken
        }
        return next
    }

    private func saveTokenResponse(_ response: [String: Any]) -> Tokens {
        let next = Self.mergeTokenResponse(
            response,
            existingRefreshToken: loadTokens()?.refreshToken ?? "",
            now: Date().timeIntervalSince1970)
        tokens = next
        keychainSave(next)
        return next
    }

    private func postJson(_ url: String, body: [String: String]) async throws
        -> [String: Any] {
        var request = URLRequest(url: URL(string: url)!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            let detail = String(data: data, encoding: .utf8) ?? ""
            throw NSError(domain: "ZoomOAuth", code: 1, userInfo: [
                NSLocalizedDescriptionKey: detail.isEmpty ? "broker error" : detail,
            ])
        }
        return (try JSONSerialization.jsonObject(with: data) as? [String: Any]) ?? [:]
    }

    // ── Keychain (this app's OWN service — never the OBS plugin's) ───────────

    // Internal (not private) so a test can assert this app NEVER reads or
    // writes the OBS plugin's item ("CoreVideo OBS Plugin").
    static let service = "us.iamfatness.corevideopro.zoom-oauth"
    private static let account = "tokens"

    private func loadTokens() -> Tokens? {
        if let tokens { return tokens }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: Self.account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data,
              let decoded = try? JSONDecoder().decode(Tokens.self, from: data),
              !decoded.refreshToken.isEmpty || !decoded.accessToken.isEmpty
        else { return nil }
        tokens = decoded
        return decoded
    }

    private func keychainSave(_ value: Tokens) {
        guard let data = try? JSONEncoder().encode(value) else { return }
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: Self.account,
        ]
        let update: [String: Any] = [kSecValueData as String: data]
        let status = SecItemUpdate(base as CFDictionary, update as CFDictionary)
        if status == errSecItemNotFound {
            var add = base
            add[kSecValueData as String] = data
            SecItemAdd(add as CFDictionary, nil)
        }
    }

    private func keychainDelete() {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: Self.account,
        ]
        SecItemDelete(base as CFDictionary)
    }
}
