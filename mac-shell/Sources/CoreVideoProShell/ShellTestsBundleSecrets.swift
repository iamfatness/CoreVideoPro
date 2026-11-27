// Support bundle secret scrubbing. Every value below is FAKE, and each is short or
// shaped so that the pattern rules (layer 2) cannot catch it as free text: only
// the known-literal layer can. Keychain reads hit the harness's in-memory backend.

import Foundation

extension ShellTests {
    static var bundleSecretCases: [(String, () -> Void)] {
        [
            ("bundle-secrets/every-held-secret", testBundleScrubsEveryHeldSecret),
            ("bundle-secrets/encoded-forms", testBundleScrubsEncodedForms),
            ("bundle-secrets/dictionary-keys", testBundleScrubsDictionaryKeys),
            ("bundle-secrets/overlapping-literals", testBundleScrubsOverlappingLiterals),
            ("bundle-secrets/manifest", testBundleManifestCarriesNoSecret),
            ("bundle-secrets/headless-export", testBundleHeadlessExportScrubsStoredSecrets),
            ("bundle-secrets/large-input", testBundleLiteralScrubStaysLinear),
        ]
    }

    /// Writes a bundle from `input` with seeded logs, returns file name -> text.
    private static func writeSecretBundle(_ input: SupportBundleInput, shellLog: String, coreLog: String,
                                          crashNames: [String] = []) -> [String: String] {
        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("cvp-bundle-secrets-\(UUID().uuidString)")
        let crashes = dir.appendingPathComponent("crashes")
        try? fm.createDirectory(at: crashes, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: dir) }
        for name in crashNames { try? Data().write(to: crashes.appendingPathComponent(name)) }
        let shell = dir.appendingPathComponent("shell.log"), core = dir.appendingPathComponent("core.log")
        try? shellLog.write(to: shell, atomically: true, encoding: .utf8)
        try? coreLog.write(to: core, atomically: true, encoding: .utf8)
        var input = input
        input.destinationRoot = dir.appendingPathComponent("out")
        input.shellLogPath = shell.path
        input.coreLogPath = core.path
        input.crashReportDirectory = crashes.path
        guard let result = try? SupportBundle.write(input) else { expect(false, "bundle written"); return [:] }
        var files: [String: String] = [:]
        for name in (try? fm.contentsOfDirectory(atPath: result.folder.path)) ?? [] {
            files[name] = (try? String(contentsOf: result.folder.appendingPathComponent(name), encoding: .utf8)) ?? ""
        }
        return files
    }

    /// Every string and every key of a JSON file, unescaped (what a reader sees).
    private static func jsonStrings(_ value: Any) -> [String] {
        if let object = value as? [String: Any] { return object.flatMap { [$0.key] + jsonStrings($0.value) } }
        if let array = value as? [Any] { return array.flatMap(jsonStrings) }
        if let text = value as? String { return [text] }
        return []
    }

    private static func expectAbsent(_ needle: String, _ label: String, in files: [String: String]) {
        for (name, text) in files.sorted(by: { $0.key < $1.key }) {
            var leaked = text.contains(needle)
            if !leaked, name.hasSuffix(".json"),
               let json = try? JSONSerialization.jsonObject(with: Data(text.utf8)) {
                leaked = jsonStrings(json).contains { $0.contains(needle) }
            }
            expect(!leaked, "LEAK \(label) in \(name)")
        }
    }

    @MainActor static func testBundleScrubsEveryHeldSecret() {
        let secrets: [(String, String)] = [
            ("stream key", "fkstreamkeyq"), ("srt passphrase", "fksrtphrase"),
            ("srt stream id", "fkstreamidzz"), ("meeting passcode", "fkpasscodeq"),
            ("join link passcode", "fklinkpwdq"), ("recent meeting passcode", "fkrecentpwdq"),
            ("youtube api key", "fkyoutubekeyq"), ("twitch access token", "fktwaccessq"),
            ("twitch refresh token", "fktwrefreshq"), ("zoom access token", "fkzmaccessq"),
            ("zoom refresh token", "fkzmrefreshq"), ("control token", "fkcontroltokq"),
        ]
        var v: [String: String] = [:]
        for (name, value) in secrets { v[name] = value }

        let model = AppModel()
        model.streamKey = v["stream key"]!
        // Pasted with stray whitespace: the core receives (and logs) the trimmed value.
        model.srtPassphrase = " " + v["srt passphrase"]! + "\n"
        model.streamDestinations.srtStreamId = v["srt stream id"]!
        model.joinPasscode = v["meeting passcode"]!
        model.joinMeetingId = "https://us02web.zoom.us/j/88800011122?pwd=\(v["join link passcode"]!)"
        model.recentMeetings = ["https://zoom.us/j/88800011133?pwd=\(v["recent meeting passcode"]!)"]
        _ = SocialCredentials.youTubeKey.writeString(v["youtube api key"]!)
        _ = SocialCredentials.twitch.write(Data(
            "{\"accessToken\":\"\(v["twitch access token"]!)\",\"refreshToken\":\"\(v["twitch refresh token"]!)\"}".utf8))
        var zoom = ZoomOAuth.Tokens()
        zoom.accessToken = v["zoom access token"]!
        zoom.refreshToken = v["zoom refresh token"]!
        _ = ZoomOAuth.keychain.write((try? JSONEncoder().encode(zoom)) ?? Data())
        let previousToken = ProcessInfo.processInfo.environment["COREVIDEO_CONTROL_TOKEN"]
        setenv("COREVIDEO_CONTROL_TOKEN", v["control token"]!, 1)
        defer {
            SocialCredentials.youTubeKey.delete(); SocialCredentials.twitch.delete(); ZoomOAuth.keychain.delete()
            if let previousToken { setenv("COREVIDEO_CONTROL_TOKEN", previousToken, 1) }
            else { unsetenv("COREVIDEO_CONTROL_TOKEN") }
        }

        // One free-text sentence per secret, with no shape a pattern rule knows.
        let prose = secrets.map { "peer refused <\($0.1)> end" }.joined(separator: "\n")
        let flat = secrets.map { "<\($0.1)>" }.joined(separator: " ")
        model.statusDetail = flat
        model.zoomOAuthStatus = flat
        model.warnings = [flat]
        model.lastSnapshot = ["health": ["messages": [flat]], "nested": [["lastError": flat]]]
        model.lastZoomSnapshot = ["lastError": flat]
        let files = writeSecretBundle(model.supportBundleInput(), shellLog: prose, coreLog: prose,
                                      crashNames: ["corevideo-\(v["stream key"]!).ips"])
        for file in ["core-snapshot.json", "zoom-snapshot.json", "shell-state.json", "warnings.txt",
                     "shell-log-tail.txt", "core-log-tail.txt", "crash-reports.txt", "manifest.txt"] {
            expect(files[file] != nil, "\(file) is in the bundle")
        }
        for (name, value) in secrets { expectAbsent(value, name, in: files) }
        expect(files["core-log-tail.txt"]?.contains("peer refused") == true, "the diagnostic text survives")
    }

    /// A literal as it looks after JSON escaping (once, twice) and URL encoding.
    static func testBundleScrubsEncodedForms() {
        let secret = "fk q\"a\\b/c&d=é%z"
        func json(_ s: String, _ options: JSONSerialization.WritingOptions = []) -> String {
            let data = (try? JSONSerialization.data(withJSONObject: [s], options: options)) ?? Data()
            return String(String(decoding: data, as: UTF8.self).dropFirst(2).dropLast(2))
        }
        func percent(_ s: String, _ allowed: CharacterSet) -> String {
            s.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        }
        // The core's percentEncodeSrtValue: everything but RFC 3986 unreserved, uppercase hex.
        let unreserved = CharacterSet(charactersIn:
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
        let srt = percent(secret, unreserved)
        var lower = srt
        for hex in ["%20", "%22", "%5C", "%2F", "%26", "%3D", "%C3", "%A9", "%25"] {
            lower = lower.replacingOccurrences(of: hex, with: hex.lowercased())
        }
        let forms: [(String, String)] = [
            ("raw", secret),
            ("json", json(secret)),
            ("json unescaped slash", json(secret, [.withoutEscapingSlashes])),
            ("json twice", json(json(secret))),
            ("json twice unescaped slash", json(json(secret, [.withoutEscapingSlashes]), [.withoutEscapingSlashes])),
            ("json unicode escapes", "fk q\\u0022a\\u005Cb\\u002fc\\u0026d\\u003d\\u00e9%z"),
            ("percent srt", srt),
            ("percent lowercase", lower),
            ("percent query set", percent(secret, .urlQueryAllowed)),
            ("percent path set", percent(secret, .urlPathAllowed)),
            ("percent twice", percent(srt, unreserved)),
            ("form encoded", srt.replacingOccurrences(of: "%20", with: "+")),
        ]
        for (label, form) in forms {
            let line = "info: payload={\"v\":\"\(form)\"} srt://h.example:9000?mode=caller&streamid=\(form)&x=1"
            var input = SupportBundleInput()
            input.knownSecretLiterals = [secret]
            input.warnings = ["saw <\(form)>"]
            input.coreSnapshotJson = (try? JSONSerialization.data(withJSONObject: ["m": "saw <\(form)>"])) ?? Data()
            let files = writeSecretBundle(input, shellLog: line, coreLog: "saw <\(form)> done")
            expectAbsent(form, label, in: files)
            expect(files["core-log-tail.txt"]?.contains("saw <[redacted]> done") == true,
                   "\(label): replaced whole, with no fragment left, text around it kept")
        }
        // Nothing that merely resembles it is touched.
        let benign = "path /tmp/a%20b \\\\server\\share {\"v\":\"fk q\"} 100% done"
        expectEqual(Redactor.scrub(benign, literals: [secret]), benign, "benign escapes survive")
    }

    static func testBundleScrubsDictionaryKeys() {
        let secret = "fk/dict\"secretq", other = "fkplainsecretq"
        var input = SupportBundleInput()
        input.knownSecretLiterals = [secret, other]
        let snapshot: JSONObject = [
            "bySource": [secret: "one", other: "two", "x \(other)": ["deeper": [secret: 3]]],
            // Two keys that redact to the same text must both survive.
            "collide": ["a \(secret)": "first", "a \(other)": "second"],
            "streamKey": "whatever",
        ]
        let data = (try? JSONSerialization.data(withJSONObject: snapshot)) ?? Data()
        input.coreSnapshotJson = data
        input.zoomSnapshotJson = data
        input.shellStateJson = data
        let files = writeSecretBundle(input, shellLog: "x", coreLog: "x")
        expectAbsent(secret, "dictionary key (needs escaping)", in: files)
        expectAbsent(other, "dictionary key", in: files)
        let decoded = (try? JSONSerialization.jsonObject(with: Data((files["core-snapshot.json"] ?? "").utf8))) as? JSONObject
        expect(decoded != nil, "the redacted file is still JSON")
        let collide = decoded?["collide"] as? JSONObject ?? [:]
        expectEqual(Set(collide.values.compactMap { $0 as? String }), ["first", "second"], "no value lost to a key collision")
        expectEqual((decoded?["bySource"] as? JSONObject)?.count, 3, "every key kept")
    }

    /// One held secret inside another must not leave the longer one's remainder.
    static func testBundleScrubsOverlappingLiterals() {
        let short = "fkabcd", long = "zz-fkabcd-tailq", left = "fkleftMID", right = "MIDrightq"
        var input = SupportBundleInput()
        input.knownSecretLiterals = [short, long, left, right]
        let files = writeSecretBundle(input, shellLog: "a \(long) b", coreLog: "c fkleftMIDrightq d")
        expectAbsent("-tailq", "remainder of the containing secret", in: files)
        expectAbsent("rightq", "remainder of an overlapping secret", in: files)
        expect(files["shell-log-tail.txt"]?.contains("a [redacted] b") == true, "one placeholder")
        expect(files["core-log-tail.txt"]?.contains("c [redacted] d") == true, "overlap merged")
    }

    static func testBundleManifestCarriesNoSecret() {
        let secret = "fkmanifestsecretq"
        var input = SupportBundleInput()
        input.knownSecretLiterals = [secret]
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("cvp-bundle-manifest-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        input.destinationRoot = dir
        // A missing log becomes a manifest note that quotes its path.
        input.shellLogPath = "/nonexistent/\(secret).log"
        input.coreLogPath = "/nonexistent/core.log"
        input.crashReportDirectory = "/nonexistent"
        guard let result = try? SupportBundle.write(input) else { expect(false, "bundle written"); return }
        let manifest = (try? String(contentsOf: result.folder.appendingPathComponent("manifest.txt"), encoding: .utf8)) ?? ""
        expect(manifest.contains("shell-log-tail.txt: not found"), "the skip note is there")
        expect(!manifest.contains(secret), "LEAK secret in manifest.txt")
    }

    /// COREVIDEO_SHELL_EXPORT_BUNDLE has no model, but exports the same log tails.
    static func testBundleHeadlessExportScrubsStoredSecrets() {
        _ = StreamKeychain.store().writeString("fkheadkeyq")
        _ = StreamKeychain.store(account: StreamKeychain.srtPassphraseAccount).writeString("fkheadsrtq")
        _ = SocialCredentials.youTubeKey.writeString("fkheadytq")
        let previous = FileManager.default.contents(atPath: ShellPrefs.path)
        var prefs = ShellPrefs()
        var destinations = StreamDestinationsConfig()
        destinations.srtStreamId = "fkheadstreamid"
        prefs.streamDestinations = destinations
        prefs.joinMeetingId = "https://zoom.us/j/88800011144?pwd=fkheadjoinq"
        prefs.recentMeetings = ["https://zoom.us/j/88800011155?pwd=fkheadrecentq"]
        let url = URL(fileURLWithPath: ShellPrefs.path)
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? JSONEncoder().encode(prefs).write(to: url)
        defer {
            StreamKeychain.store().delete()
            StreamKeychain.store(account: StreamKeychain.srtPassphraseAccount).delete()
            SocialCredentials.youTubeKey.delete()
            if let previous { try? previous.write(to: url) } else { try? FileManager.default.removeItem(at: url) }
        }
        let secrets = ["fkheadkeyq", "fkheadsrtq", "fkheadytq", "fkheadstreamid", "fkheadjoinq", "fkheadrecentq"]
        let log = secrets.map { "peer refused <\($0)>" }.joined(separator: "\n")
        let files = writeSecretBundle(DiagnosticsSelfCheck.headlessInput(destination: "1"), shellLog: log, coreLog: log)
        expect(files["core-log-tail.txt"]?.contains("peer refused") == true, "the tail is there")
        for secret in secrets { expectAbsent(secret, secret, in: files) }
    }

    /// The literal layer runs over every line of two 2 MB tails, so it must stay
    /// linear on the shapes that start a match: "%", backslashes, near-misses.
    static func testBundleLiteralScrubStaysLinear() {
        let literals = (0..<12).map { "fklarge-secret \"\\/%\($0)" } + ["\\\\\\\\\\\\fkslashes", "%%%%%%fkpercent",
                                                                       String(repeating: "a", count: 64) + "fk"]
        let scrubber = LiteralScrubber(literals)
        let size = 1 << 20
        let shapes: [(String, String)] = [
            ("backslash run", String(repeating: "\\", count: size)),
            ("percent run", String(repeating: "%", count: size)),
            ("double-encoding run", "%" + String(repeating: "25", count: size / 2)),
            ("encoded percents", String(repeating: "%25", count: size / 3)),
            ("near misses", String(repeating: "fklarge-secret \\\"", count: size / 18)),
            ("one letter", String(repeating: "a", count: size)),
        ]
        for (label, text) in shapes {
            let start = Date()
            _ = scrubber.scrub(text)
            let seconds = Date().timeIntervalSince(start)
            FileHandle.standardError.write(Data("    large-input \(label): \(String(format: "%.2f", seconds)) s\n".utf8))
            expect(seconds < 10, "\(label) took \(seconds) s")
        }
        // And it still finds a secret at the far end of a large line.
        let hit = scrubber.scrub(String(repeating: "\\ %25 ", count: 100_000) + "fklarge-secret%20%22%5C%2F%257")
        expect(hit.text.hasSuffix(" [redacted]") && hit.hits == 1, "found after 600 KB")
    }
}
