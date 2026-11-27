// Support bundle secret scrubbing. Every value below is FAKE, and each is short or
// shaped so that the pattern rules (layer 2) cannot catch it as free text: only
// the known-literal layer can. Keychain reads hit the harness's in-memory backend.

import Foundation

extension ShellTests {
    static var bundleSecretCases: [(String, () -> Void)] {
        [
            ("bundle-secrets/every-held-secret", testBundleScrubsEveryHeldSecret),
            ("bundle-secrets/encoded-forms", testBundleScrubsEncodedForms),
            ("bundle-secrets/unicode-normalization", testBundleScrubsCanonicallyEquivalentText),
            ("bundle-secrets/dictionary-keys", testBundleScrubsDictionaryKeys),
            ("bundle-secrets/overlapping-literals", testBundleScrubsOverlappingLiterals),
            ("bundle-secrets/manifest", testBundleManifestCarriesNoSecret),
            ("bundle-secrets/headless-export", testBundleHeadlessExportScrubsStoredSecrets),
            ("bundle-secrets/large-input", testBundleLiteralScrubStaysLinear),
            ("bundle-secrets/frozen-live-destinations", testBundleScrubsTheSecretsStillOnAir),
            ("bundle-secrets/more-sources", testBundleScrubsEnvironmentAndOtherLinks),
            ("bundle-secrets/no-over-redaction", testBundleKeepsDiagnosticKeysAndWords),
            ("bundle-secrets/long-run", testBundleKeyValueRuleStaysLinear),
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

    /// A non-ASCII secret held in one normalization and logged in another (NFC,
    /// NFD, a mix, reordered marks), raw, JSON-escaped, \u-escaped or URL-encoded.
    static func testBundleScrubsCanonicallyEquivalentText() {
        // é, ñ, Hangul, and a letter with two marks (partly composable, reorderable).
        let nfc = "fk\u{E9}-\u{F1}\u{D55C}-\u{1EAD}q/z"
        let nfd = "fke\u{301}-n\u{303}\u{1112}\u{1161}\u{11AB}-a\u{323}\u{302}q/z"
        let spellings: [(String, String)] = [
            ("nfc", nfc), ("nfd", nfd),
            ("mixed", "fk\u{E9}-n\u{303}\u{D55C}-a\u{302}\u{323}q/z"),
            ("partly composed", "fke\u{301}-\u{F1}\u{1112}\u{1161}\u{11AB}-\u{E2}\u{323}q/z"),
            ("partly composed, other mark", "fk\u{E9}-\u{F1}\u{D558}\u{11AB}-\u{1EA1}\u{302}q/z"),
        ]
        let unreserved = CharacterSet(charactersIn:
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
        func uEscaped(_ s: String) -> String {
            s.utf16.map { $0 < 128 && $0 != 47 ? String(UnicodeScalar(UInt8($0))) : String(format: "\\u%04x", $0) }.joined()
        }
        for (name, text) in spellings {
            expect(text == nfc && (name == "nfc" || !text.utf8.elementsEqual(nfc.utf8)), "\(name): equivalent, other bytes")
        }
        // Unrelated text in either normalization must come through byte for byte.
        let bystander = "cafe\u{301} caf\u{E9} \u{212B} \u{1112}\u{1161}"
        for (heldName, held) in [("held nfc", nfc), ("held nfd", nfd)] {
            for (name, spelling) in spellings {
                let forms = [("raw", spelling), ("json", spelling.replacingOccurrences(of: "/", with: "\\/")),
                             ("unicode escape", uEscaped(spelling)),
                             ("percent", spelling.addingPercentEncoding(withAllowedCharacters: unreserved) ?? "")]
                for (formName, form) in forms {
                    let label = "\(heldName), \(name) \(formName)"
                    let out = Redactor.scrub("\(bystander) <\(form)> end", literals: [held])
                    expect(out.utf8.elementsEqual("\(bystander) <[redacted]> end".utf8), "LEAK or damage: \(label) -> \(out)")
                }
            }
            // And through a whole bundle: log tail, warnings, a JSON value and a key.
            var input = SupportBundleInput()
            input.knownSecretLiterals = [held]
            let other = held.utf8.elementsEqual(nfc.utf8) ? nfd : nfc
            input.warnings = ["saw <\(other)>"]
            input.coreSnapshotJson = (try? JSONSerialization.data(withJSONObject: ["m": "saw \(other)", other: 1])) ?? Data()
            let files = writeSecretBundle(input, shellLog: "saw <\(uEscaped(other))>", coreLog: "saw <\(other)>")
            for (name, spelling) in spellings { expectAbsent(spelling, "\(heldName), \(name)", in: files) }
            expectAbsent(uEscaped(other), "\(heldName), escaped other form", in: files)
        }
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
            expect(seconds < 60, "\(label) took \(seconds) s")
        }
        // A non-ASCII literal has several spellings per character.
        let accented = LiteralScrubber((0..<12).map { "\u{E9}\u{E9}\u{E9}\u{E9}\u{1EAD}\u{D55C}-\($0)" })
        for (label, text) in [("composed near misses", String(repeating: "\u{E9}", count: size / 2)),
                              ("decomposed near misses", String(repeating: "e\u{301}", count: size / 3))] {
            let start = Date()
            expectEqual(accented.scrub(text).hits, 0, "\(label): no hit")
            let seconds = Date().timeIntervalSince(start)
            FileHandle.standardError.write(Data("    large-input \(label): \(String(format: "%.2f", seconds)) s\n".utf8))
            expect(seconds < 60, "\(label) took \(seconds) s")
        }
        // And it still finds a secret at the far end of a large line.
        let hit = scrubber.scrub(String(repeating: "\\ %25 ", count: 100_000) + "fklarge-secret%20%22%5C%2F%257")
        expect(hit.text.hasSuffix(" [redacted]") && hit.hits == 1, "found after 600 KB")
    }

    /// Start freezes the destinations; the footer and Settings key fields stay
    /// editable, so the key ON AIR may no longer be the one in the field.
    @MainActor static func testBundleScrubsTheSecretsStillOnAir() {
        let model = AppModel()
        model.streamKey = "fkonairkeyq"
        model.srtPassphrase = "fkonairsrtq"
        model.streamDestinations.srtStreamId = "fkonairstreamid"
        model.streamingDesired = true
        model.streamKey = "fkeditedkeyq"
        model.srtPassphrase = ""
        model.streamDestinations.srtStreamId = ""
        let secrets = ["fkonairkeyq", "fkonairsrtq", "fkonairstreamid", "fkeditedkeyq"]
        let log = secrets.map { "peer refused <\($0)>" }.joined(separator: "\n")
        let files = writeSecretBundle(model.supportBundleInput(), shellLog: log, coreLog: log)
        for secret in secrets { expectAbsent(secret, "on air: " + secret, in: files) }
        // Stop is the usual moment before an export, and a second take must not
        // forget the first.
        model.streamingDesired = false
        model.streamKey = "fksecondkeyq"
        model.streamingDesired = true
        model.streamingDesired = false
        model.streamKey = ""
        let later = secrets + ["fksecondkeyq"]
        let laterLog = later.map { "peer refused <\($0)>" }.joined(separator: "\n")
        let after = writeSecretBundle(model.supportBundleInput(), shellLog: laterLog, coreLog: laterLog)
        // (fkeditedkeyq was typed but never streamed, then replaced: not held.)
        for secret in later where secret != "fkeditedkeyq" { expectAbsent(secret, "after Stop: " + secret, in: after) }
    }

    /// Secrets the core reads from the environment it inherits from the shell,
    /// passcodes in non-zoom.us links or pasted invites, and the HLS ingest URL.
    @MainActor static func testBundleScrubsEnvironmentAndOtherLinks() {
        let names = ["COREVIDEO_ZOOM_MEETING_PASSCODE", "COREVIDEO_ZOOM_USER_ZAK", "COREVIDEO_ZOOM_SDK_JWT",
                     "COREVIDEO_ZOOM_ON_BEHALF_TOKEN", "COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN"]
        var environment: [String: String] = [:]
        for (index, name) in names.enumerated() { environment[name] = "fkenvsecret\(index)q" }
        var held = SupportBundleSecrets.Held()
        held.meetingTargets = ["https://example.zoomgov.com/j/1?pwd=fkgovpwdq",
                               "Join: https://zoom.com/j/2?pwd=fk%2Dinvitepwdq#success - see you"]
        held.destinations = [(StreamDestinationsConfig(hlsPlaylistUrl: "https://h.example/up/fkhlspathq/live.m3u8"),
                              StreamDestinationSecrets())]
        let literals = SupportBundleSecrets.literals(held, environment: environment)
        for value in environment.values.sorted() + ["fkgovpwdq", "fk-invitepwdq",
                                                    "https://h.example/up/fkhlspathq/live.m3u8"] {
            expect(literals.contains(value), "held: \(value)")
        }
        // The core's FFmpeg errors name numbered segments, not the playlist.
        let segment = "PUT https://h.example/up/fkhlspathq/live_segment_00000000000000000001.ts failed"
        expect(!Redactor.scrub(segment, literals: literals).contains("fkhlspathq"), "the HLS path in a segment URL")
        // A half-typed or invalid HLS field is not a literal: it would switch
        // off the user:password rule and shred ordinary URLs.
        for field in ["https://", "http", "https://cdn.example", "https://u:p@cdn.example/a/b.m3u8"] {
            var typing = SupportBundleSecrets.Held()
            typing.destinations = [(StreamDestinationsConfig(hlsPlaylistUrl: field), StreamDestinationSecrets())]
            let partial = SupportBundleSecrets.literals(typing, environment: [:])
            expect(!partial.contains(field), "not held: \(field)")
            expect(!Redactor.scrub("pull https://user:fkpw99@cdn.example/x", literals: partial).contains("fkpw99"),
                   "the pattern rules still work with the field at \(field)")
        }
        var plain = SupportBundleSecrets.Held()
        plain.destinations = [(StreamDestinationsConfig(hlsPlaylistUrl: "https://h.example/live.m3u8"),
                               StreamDestinationSecrets())]
        expect(!SupportBundleSecrets.literals(plain, environment: [:]).contains("/live"), "a one-part path is a word")
        // Punctuation around a pasted link, HTML "&amp;", and a wrapped link.
        for target in ["see <https://zoom.us/j/1?pwd=fkTail.9q>", "(https://zoom.us/j/1?pwd=fkTail.9q).",
                       "go to https://zoom.us/j/1?pwd=fkTail.9q.", "\"https://zoom.us/j/1?a=1&amp;pwd=fkTail.9q\";x",
                       "https://wrap.example/?url=https%3A%2F%2Fzoom.us%2Fj%2F1%3Fpwd%3DfkTail.9q&data=1"] {
            var linked = SupportBundleSecrets.Held()
            linked.meetingTargets = [target]
            let found = SupportBundleSecrets.literals(linked, environment: [:])
            // (The as-written cut may take one neighbouring character with it.)
            let bare = Redactor.scrub("bare <fkTail.9q> end", literals: found)
            expect(!bare.contains("fkTail") && bare.hasSuffix(" end"), "\(target) -> \(bare)")
        }
        // A secret whose FIRST character is written as an escape.
        for (literal, written) in [("\u{E9}fkfirstq", "\\u00e9fkfirstq"), ("/fkfirstq", "\\/fkfirstq"),
                                   ("/fkfirstq", "\\\\\\/fkfirstq"), ("fkfirstq", "\\U0066kfirstq"),
                                   ("\nfkfirstq", "\\nfkfirstq")] {
            expectEqual(Redactor.scrub("<\(written)>", literals: [literal]), "<[redacted]>", "leading escape \(written)")
        }
        // NSDictionary descriptions write \U00e9.
        expectEqual(Redactor.scrub("<fk\\U00e9secret>", literals: ["fk\u{E9}secret"]), "<[redacted]>", "\\U escape")
    }

    /// Scrubbing keys and Stream IDs must not shred the diagnostics.
    static func testBundleKeepsDiagnosticKeysAndWords() {
        let longKey = "programHardwareH264EncoderSession2Count"
        let redacted = Redactor.redact([longKey: 2]) as? JSONObject
        expect(redacted?[longKey] != nil, "a long mixed-case diagnostic KEY survives")
        let urlKey = Redactor.redact(["rtmp://h.example/app/fkurlkeyq": 1]) as? JSONObject
        expect(urlKey?.keys.contains { $0.contains("fkurlkeyq") } == false, "a URL used as a key is still filtered")
        for word in ["stream", "streaming", "publish"] {
            var held = SupportBundleSecrets.Held()
            held.destinations = [(StreamDestinationsConfig(srtStreamId: word), StreamDestinationSecrets())]
            expect(!SupportBundleSecrets.literals(held, environment: [:]).contains(word),
                   "the bundle's own word \(word) is not a literal")
        }
    }

    /// A pre-existing quadratic: the `someKey=value` rule rescanned an unbroken
    /// run from every position, so one long line hung the export.
    static func testBundleKeyValueRuleStaysLinear() {
        // Same matches as before the rule was anchored to the start of a run.
        let cases = [("a streamKey=fkv1 b", "a streamKey=[redacted] b"),
                     ("x-my_token : fkv2;y", "x-my_token : [redacted];y"),
                     ("userZak=fkv3,sdkJwt=fkv4", "userZak=[redacted],sdkJwt=[redacted]"),
                     ("keyphase=ok monkey=fkv5", "keyphase=ok monkey=[redacted]"),
                     ("[pwd=fkv6]", "[pwd=[redacted]]")]
        for (input, output) in cases { expectEqual(Redactor.scrub(input), output, input) }
        // 24 KB is minutes under the quadratic rule.
        // The JWT rule, likewise: same matches, one pass per run.
        let token = "eyJabcdefgh.ijklmnopqr"
        for (input, output) in [("t \(token) u", "t [redacted] u"), ("t \(token).sig-_9 u", "t [redacted] u"),
                                ("id-\(token) u", "id-[redacted] u"),
                                ("eyJ-eyJabcdefgh.ijklmnopqr", "[redacted]"),
                                ("a/\(token);\(token)", "a/[redacted];[redacted]"),
                                ("xeyJabcdefgh.ijklmnopqr", "xeyJabcdefgh.ijklmnopqr"),
                                ("eyJshort.ijklmnopqr", "eyJshort.ijklmnopqr")] {
            expectEqual(Redactor.scrub(input), output, input)
        }
        for (label, text) in [("letters", String(repeating: "a", count: 24_000)),
                              ("jwt starts", String(repeating: "eyJ-", count: 32_000)),
                              ("hex", String(repeating: "0f3a", count: 6_000))] {
            let start = Date()
            _ = Redactor.scrub(text)
            let seconds = Date().timeIntervalSince(start)
            FileHandle.standardError.write(Data("    long-run \(label): \(String(format: "%.2f", seconds)) s\n".utf8))
            expect(seconds < 5, "\(label) took \(seconds) s")
        }
    }
}
