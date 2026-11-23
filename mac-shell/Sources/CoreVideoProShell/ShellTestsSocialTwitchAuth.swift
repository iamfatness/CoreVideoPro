// Twitch authorization, plan 2026-11-22 Task 16: ports of
// native-shell/CoreVideoPro.MediaCore.Tests/TwitchAuthorizationTests.cs with
// the C# literals and clock (2026-11-05T06:00:00Z), plus the Keychain blob.

import Foundation

extension ShellTests {
    static var socialTwitchAuthCases: [(String, () -> Void)] {
        [
            ("social/twitch-oversized-response-fails-closed", testTwitchOversizedResponse),
            ("social/twitch-device-flow-read-chat-only-stores-only-validated-tokens", testTwitchDeviceFlow),
            ("social/twitch-denied-or-expired-consent-never-stores", testTwitchDeniedConsent),
            ("social/twitch-device-deadline-never-repeats-grant", testTwitchDeviceDeadline),
            ("social/twitch-hourly-validation-single-use-refresh-serialized-identity-fixed", testTwitchRefreshSerialized),
            ("social/twitch-rotated-pair-stored-atomically", testTwitchRotatedPairAtomic),
            ("social/twitch-verification-uri-must-be-twitch-activate", testTwitchVerificationUri),
            ("social/redaction-sentinel-twitch-tokens", testTwitchSentinel),
        ]
    }

    static let twClient = "fixtureclient12345678"

    final class TwitchClock: @unchecked Sendable {
        var now = ISO8601DateFormatter().date(from: "2026-11-05T06:00:00Z")!
        func sleep(_ ns: UInt64) async { now += Double(ns) / 1e9 }
    }

    /// A closure-driven SocialHTTP.
    final class FnHTTP: SocialHTTP, @unchecked Sendable {
        let handle: (SocialHTTPRequest) async throws -> (Int, String)
        init(_ handle: @escaping (SocialHTTPRequest) async throws -> (Int, String)) { self.handle = handle }
        func send(_ request: SocialHTTPRequest, deadline: TimeInterval) async throws -> SocialHTTPResponse {
            let (status, body) = try await handle(request)
            return SocialHTTPResponse(status: status, body: Data(body.utf8))
        }
    }

    static func twTokens(_ clock: TwitchClock) -> TwitchCredentials {
        TwitchCredentials(clientId: twClient, accessToken: "fixture-access-token", refreshToken: "fixture-refresh-token",
                          expiresAt: clock.now.addingTimeInterval(4 * 3600), userId: "5678", login: "fixtureviewer")
    }

    static func twValidation(user: String = "5678", client: String = twClient, expires: Int = 14400) -> String {
        #"{"client_id":"\#(client)","user_id":"\#(user)","login":"fixtureviewer","scopes":["user:read:chat"],"expires_in":\#(expires)}"#
    }

    static func body(_ r: SocialHTTPRequest) -> String { String(decoding: r.body ?? Data(), as: UTF8.self) }

    @MainActor static func testTwitchOversizedResponse() {
        let http = FnHTTP { _ in (200, String(repeating: "x", count: 1024 * 1024 + 1)) }
        var caught: Error?
        _ = awaitMain {
            do { _ = try await TwitchHttpTransport.validate(twTokens(TwitchClock()), http: http) } catch { caught = error }
            return true
        }
        expectEqual(caught as? TwitchFailure, .invalid, "1 MiB + 1 fails closed")
    }

    @MainActor static func testTwitchDeviceFlow() {
        let clock = TwitchClock()
        var stored: TwitchCredentials?
        var prompts: [TwitchDevicePrompt] = []
        var polls = 0
        let http = FnHTTP { request in
            expectEqual(request.url.host, "id.twitch.tv", "id host")
            expect(!body(request).contains("client_secret"), "no client secret, ever")
            switch request.url.path {
            case "/oauth2/device":
                expect(body(request).contains("scopes=user%3Aread%3Achat"), "read-chat scope only")
                return (200, #"{"device_code":"private-fixture-device","user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate","expires_in":1800,"interval":5}"#)
            case "/oauth2/token":
                expect(body(request).contains("device_code=private-fixture-device"), "device code polled")
                polls += 1
                if polls == 1 { return (400, #"{"message":"authorization_pending"}"#) }
                return (200, #"{"access_token":"fixture-access-token","refresh_token":"fixture-refresh-token","expires_in":14400,"token_type":"bearer","scope":["user:read:chat"]}"#)
            default:
                expectEqual(request.url.path, "/oauth2/validate", "validate")
                expect(stored == nil, "nothing stored before validation")
                return (200, twValidation())
            }
        }
        let auth = TwitchAuthorization(http: http, read: { stored }, save: { stored = $0; return true }, clear: { stored = nil },
                                       now: { clock.now }, sleep: clock.sleep)
        let result = awaitMain { try? await auth.authorize(clientId: twClient) { prompts.append($0) } } ?? nil
        expectEqual(result?.userId, "5678", "validated identity")
        expectEqual(result, stored, "stored what was validated")
        expectEqual(prompts.map(\.userCode), ["ABCDEFGH"], "one prompt")
        expect(!String(describing: stored as Any).contains("fixture-access-token"), "description has no token")
        expect(!String(reflecting: stored as Any).contains("fixture-access-token"), "debugDescription has no token")
        expectEqual(polls, 2, "two polls")
    }

    @MainActor static func testTwitchDeniedConsent() {
        for failure in ["access_denied", "expired_token"] {
            let clock = TwitchClock()
            let http = FnHTTP { request in
                request.url.path.hasSuffix("device")
                    ? (200, #"{"device_code":"fixture-device-code","user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate","expires_in":20,"interval":5}"#)
                    : (400, #"{"message":"\#(failure)"}"#)
            }
            let auth = TwitchAuthorization(http: http, read: { nil }, save: { _ in expect(false, "stored unapproved credentials"); return true },
                                           clear: {}, now: { clock.now }, sleep: clock.sleep)
            var threw = false
            _ = awaitMain { do { _ = try await auth.authorize(clientId: twClient) { _ in } } catch { threw = true }; return true }
            expect(threw, "\(failure): refused")
        }
    }

    @MainActor static func testTwitchDeviceDeadline() {
        let clock = TwitchClock()
        var starts = 0
        let http = FnHTTP { request in
            if request.url.path.hasSuffix("device") {
                starts += 1
                return (200, #"{"device_code":"fixture-device-code","user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate","expires_in":9,"interval":5}"#)
            }
            return (400, #"{"message":"authorization_pending"}"#)
        }
        let auth = TwitchAuthorization(http: http, read: { nil }, save: { _ in expect(false, "stored expired credentials"); return true },
                                       clear: {}, now: { clock.now }, sleep: clock.sleep)
        var threw = false
        _ = awaitMain { do { _ = try await auth.authorize(clientId: twClient) { _ in } } catch { threw = true }; return true }
        expect(threw, "the deadline ends the grant")
        expectEqual(starts, 1, "the device grant is never repeated")
    }

    @MainActor static func testTwitchRefreshSerialized() {
        let clock = TwitchClock()
        var stored: TwitchCredentials? = twTokens(clock)
        var refreshes = 0, validations = 0
        var foreign = false
        var issued = clock.now
        let http = FnHTTP { request in
            if request.url.path == "/oauth2/token" {
                refreshes += 1
                issued = clock.now
                expect(body(request).contains("fixture-refresh-token"), "refresh uses the stored token")
                expect(!body(request).contains("client_secret"), "no secret on refresh")
                return (200, #"{"access_token":"rotated-access-token","refresh_token":"rotated-refresh-token","expires_in":14400,"token_type":"bearer","scope":["user:read:chat"]}"#)
            }
            validations += 1
            return (200, twValidation(user: foreign ? "9988" : "5678", expires: Int(issued.addingTimeInterval(4 * 3600).timeIntervalSince(clock.now))))
        }
        let auth = TwitchAuthorization(http: http, read: { stored }, save: { stored = $0; return true }, clear: { stored = nil },
                                       now: { clock.now }, sleep: clock.sleep)
        _ = awaitMain { try? await auth.authorize(clientId: twClient) { _ in expect(false, "unexpected device prompt") } }
        clock.now += 3600
        _ = awaitMain { try? await auth.maintain() }
        expectEqual(validations, 2, "validated at start and after an hour")
        clock.now += 3 * 3600
        _ = awaitMain {
            await withTaskGroup(of: Void.self) { group in
                for _ in 0..<8 { group.addTask { @MainActor in _ = try? await auth.maintain() } }
            }
            return true
        }
        expectEqual(refreshes, 1, "8 concurrent maintains share one single-use refresh")
        expectEqual(stored?.refreshToken, "rotated-refresh-token", "the rotated refresh token is stored")
        foreign = true
        clock.now += 3600
        var threw = false
        _ = awaitMain { do { _ = try await auth.maintain() } catch { threw = true }; return true }
        expect(threw, "a different user id is refused")
        expect(stored == nil, "and clears the store")
    }

    /// The Keychain holds ONE item for Twitch: the rotated access+refresh pair
    /// is written together, never a new access token beside an old refresh token.
    @MainActor static func testTwitchRotatedPairAtomic() {
        guard let memory = KeychainStore.defaultBackend as? MemoryKeychain else { expect(false, "harness keychain"); return }
        let clock = TwitchClock()
        var expiring = twTokens(clock)
        expiring.expiresAt = clock.now.addingTimeInterval(30)
        _ = SocialCredentials.twitch.write((try? TwitchAuthorization.encoder.encode(expiring)) ?? Data())
        defer { SocialCredentials.twitch.delete() }
        final class Writes: @unchecked Sendable { var blobs: [Data] = [] }
        let writes = Writes()
        let auth = TwitchAuthorization(http: FnHTTP { request in
            if request.url.path == "/oauth2/token" {
                return (200, #"{"access_token":"rotated-access-token","refresh_token":"rotated-refresh-token","expires_in":14400,"token_type":"bearer","scope":["user:read:chat"]}"#)
            }
            return (200, twValidation())
        }, read: {
            SocialCredentials.twitch.read().flatMap { try? TwitchAuthorization.decoder.decode(TwitchCredentials.self, from: $0) }
        }, save: { c in
            guard let data = try? TwitchAuthorization.encoder.encode(c) else { return false }
            writes.blobs.append(data)
            return SocialCredentials.twitch.write(data)
        }, clear: { SocialCredentials.twitch.delete() }, now: { clock.now }, sleep: clock.sleep)
        _ = awaitMain { try? await auth.authorize(clientId: "") { _ in expect(false, "blank client id reuses the stored sign-in") } }
        let items = memory.keys.filter { $0.service == SocialCredentials.service }
        expectEqual(items.map(\.account).sorted(), ["twitch-credentials"], "one Keychain item for Twitch")
        for blob in writes.blobs {
            let c = try? TwitchAuthorization.decoder.decode(TwitchCredentials.self, from: blob)
            expect(c?.accessToken == "rotated-access-token" && c?.refreshToken == "rotated-refresh-token",
                   "every write carries the rotated PAIR")
        }
        expect(!writes.blobs.isEmpty, "the rotation was written")
        let prefs = String(data: (try? JSONEncoder().encode(AppModel().currentPrefs())) ?? Data(), encoding: .utf8) ?? ""
        expect(!prefs.contains("rotated-access-token") && !prefs.contains("rotated-refresh-token"), "never in prefs")
    }

    static func testTwitchVerificationUri() {
        expect(TwitchHttpTransport.verificationUri("https://www.twitch.tv/activate") != nil, "the real one")
        expect(TwitchHttpTransport.verificationUri("https://www.twitch.tv:443/activate") != nil, "explicit 443")
        for bad in ["https://evil.example/activate", "http://www.twitch.tv/activate", "https://www.twitch.tv/activate#x",
                    "https://user@www.twitch.tv/activate", "https://www.twitch.tv:8443/activate", "https://www.twitch.tv/other"] {
            expect(TwitchHttpTransport.verificationUri(bad) == nil, "refused: \(bad)")
        }
    }

    /// Tokens held in the Keychain are scrubbed from a support bundle.
    @MainActor static func testTwitchSentinel() {
        let access = "SENTINELACCESS0123456789", refresh = "SENTINELREFRESH0123456789"
        let clock = TwitchClock()
        var c = twTokens(clock)
        c.accessToken = access; c.refreshToken = refresh
        _ = SocialCredentials.twitch.write((try? TwitchAuthorization.encoder.encode(c)) ?? Data())
        defer { SocialCredentials.twitch.delete() }
        let model = AppModel()
        model.pushWarning("twitch: refresh " + refresh + " access " + access)   // a worst-case leak into warnings
        var input = model.supportBundleInput()
        expect(input.knownSecretLiterals.contains(access) && input.knownSecretLiterals.contains(refresh), "tokens handed to the scrubber")
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("cvp-tw-sentinel-\(getpid())")
        try? FileManager.default.removeItem(at: dir)
        input.destinationRoot = dir
        input.shellLogPath = "/nonexistent"; input.coreLogPath = "/nonexistent"; input.crashReportDirectory = "/nonexistent"
        _ = try? SupportBundle.write(input)
        defer { try? FileManager.default.removeItem(at: dir) }
        var scanned = 0
        for file in FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil)?.compactMap({ $0 as? URL }) ?? []
        where !file.hasDirectoryPath && file.pathExtension != "zip" {
            scanned += 1
            let text = String(decoding: (try? Data(contentsOf: file)) ?? Data(), as: UTF8.self)
            expect(!text.contains(access) && !text.contains(refresh), "\(file.lastPathComponent) carries no Twitch token")
        }
        expect(scanned > 0, "files scanned")
    }
}
