// ZoomOAuth — the mac port of ZoomOAuthService (broker mode only). The
// broker (corevideo.iamfatness.us, site-worker.js) owns PKCE and the token
// exchange; the shell opens the browser, receives the callback on a one-shot
// loopback listener (ZoomOAuthLoopback.swift — corevideo:// is shared with the
// OBS plugin and LaunchServices may hand it to that app instead; the scheme is
// kept only as a fallback), redeems the broker_token, and keeps Zoom tokens in the KEYCHAIN under this
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
    private var loopback: OAuthLoopbackListener?
    private var tokens: Tokens?
    private var refreshInFlight: Task<Tokens?, Never>?

    // ── sign-in flow ─────────────────────────────────────────────────────────

    /// Status for a sign-in that never came back (the loopback timed out).
    static let signInTimedOut = "Zoom sign-in timed out. Choose Sign in with Zoom to try again."

    /// The broker start URL. `returnUri` is the loopback callback when the
    /// listener bound, else the corevideo:// fallback.
    static func brokerStartURL(state: String, returnUri: String) -> URL {
        var components = URLComponents(string: brokerStart)!
        components.queryItems = [
            URLQueryItem(name: "state", value: state),
            URLQueryItem(name: "redirect_uri", value: brokerBase + "/oauth/callback"),
            URLQueryItem(name: "return_uri", value: returnUri),
        ]
        return components.url!
    }

    /// `onLoopbackResult` gets the final status of a loopback sign-in ("" =
    /// signed in), or `signInTimedOut`. A scheme callback still arrives through
    /// `.onOpenURL` -> handleCallback, exactly as before.
    func beginSignIn(onLoopbackResult: @escaping @Sendable (String) -> Void) async {
        loopback?.stop()
        loopback = nil
        let state = Self.newState()
        pendingState = state
        let listener = OAuthLoopbackListener()
        let bound: UInt16? = await withCheckedContinuation { continuation in
            listener.start(
                expectedState: state,
                onCallback: { url in
                    let status = await ZoomOAuth.shared.handleCallback(url)
                    onLoopbackResult(status)
                    return status
                },
                onTimeout: { onLoopbackResult(ZoomOAuth.signInTimedOut) },
                completion: { result in
                    switch result {
                    case .success(let port): continuation.resume(returning: port)
                    case .failure: continuation.resume(returning: nil)
                    }
                })
        }
        // A newer beginSignIn may have run while we waited for the bind.
        guard pendingState == state else { listener.stop(); return }
        let returnUri: String
        if let bound, bound != 0 {
            loopback = listener
            returnUri = OAuthLoopbackHttp.returnUri(port: bound)
            ShellLog.write("zoom sign-in: loopback callback on 127.0.0.1:\(bound)")
        } else {
            listener.stop()
            returnUri = Self.appReturnUri
            ShellLog.write("zoom sign-in: loopback listener failed; using corevideo:// callback")
        }
        NSWorkspace.shared.open(Self.brokerStartURL(state: state, returnUri: returnUri))
    }

    private static func newState() -> String {
        var stateBytes = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, stateBytes.count, &stateBytes)
        return Data(stateBytes).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    private func stopLoopback() {
        loopback?.stop()
        loopback = nil
    }

    // Returns a status string for the UI ("" = success).
    func handleCallback(_ url: URL) async -> String {
        // A scheme (fallback) callback finishes this sign-in: release the
        // loopback port. A loopback callback must NOT stop its own listener
        // here — that would drop the browser connection before its page.
        if url.scheme == "corevideo" { stopLoopback() }
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false)
        else { return "malformed callback URL" }
        let query = { (name: String) in
            components.queryItems?.first { $0.name == name }?.value
        }
        // State FIRST (fix round 1, review M1): an `error` from a request that
        // does not carry this sign-in's state is not Zoom's answer — acting on
        // it let any local sender end a pending sign-in. The broker always
        // sets `state` next to `error` (site-worker.js handleOauthCallback), so
        // the corevideo:// fallback is unaffected.
        guard let pendingState, let state = query("state"), state == pendingState else {
            return "sign-in state mismatch — start again"
        }
        self.pendingState = nil
        if let error = query("error"), !error.isEmpty {
            return "Zoom sign-in failed: \(error)"
        }
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
        stopLoopback()
        pendingState = nil
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
    /// The shared KeychainStore item (same service and account as before).
    static var keychain: KeychainStore { KeychainStore(service: service, account: "tokens") }

    private func loadTokens() -> Tokens? {
        if let tokens { return tokens }
        guard let data = Self.keychain.read(),
              let decoded = try? JSONDecoder().decode(Tokens.self, from: data),
              !decoded.refreshToken.isEmpty || !decoded.accessToken.isEmpty
        else { return nil }
        tokens = decoded
        return decoded
    }

    private func keychainSave(_ value: Tokens) {
        guard let data = try? JSONEncoder().encode(value) else { return }
        Self.keychain.write(data)
    }

    private func keychainDelete() {
        Self.keychain.delete()
    }
}
