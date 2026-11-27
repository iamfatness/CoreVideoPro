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
            ("bundle-secrets/never-less-than-main", testBundleStillMatchesWhatMainMatched),
            ("bundle-secrets/matcher-gives-up", testBundleDropsTextTheMatcherGivesUpOn),
            ("bundle-secrets/multi-line-short-lines", testBundleScrubsShortLinesOfAMultiLineSecret),
            ("bundle-secrets/multi-line-blank-lines", testBundleBlankLinesAreNotLiterals),
            ("bundle-secrets/multi-line-differential", testBundleMultiLineNeverLessThanMain),
            ("bundle-secrets/nul-literal", testBundleSurvivesANulInALiteral),
            ("bundle-secrets/rules-after-every-spelling", testBundleRulesSeeTextWithEverySpellingRemoved),
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
        // The same overlap between two ESCAPED spellings.
        expectEqual(Redactor.scrub("e fkleft%4DIDright%71 f", literals: [left, right]), "e [redacted] f",
                    "overlap of encoded spellings")
        // And between two matches only Foundation's search finds (Kelvin sign).
        expectEqual(Redactor.scrub("g fkleft\u{212A}rightq h", literals: ["fkleftK", "Krightq"]), "g [redacted] h",
                    "overlap of singleton matches")
        expectEqual(LiteralScrubber(["fkleftK", "Krightq"]).scrub("g fkleft\u{212A}rightq h").text,
                    "g [redacted] h", "and in the final pass")
        expectEqual(LiteralScrubber([left, right]).scrub("e fkleft%4DIDright%71 f").text, "e [redacted] f",
                    "and in the final pass")
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
        // The union path (a line WITH hits) tracks every character: one 2 MB
        // line with 60k hits and rule matches must stay quick too.
        let busy = String(repeating: "x fkunionhit {\"streamKey\": \"v\"} y ", count: 60_000)
        let began = Date()
        let busyOut = Redactor.scrub(busy, literals: (0..<18).map { "fkunused\($0)" } + ["fkunionhit"])
        let took = Date().timeIntervalSince(began)
        FileHandle.standardError.write(Data("    large-input union path: \(String(format: "%.2f", took)) s\n".utf8))
        expect(took < 60, "union path took \(took) s")
        expect(!busyOut.contains("fkunionhit") && !busyOut.contains("\"v\""), "union path filtered")
        // A multi-line value adds one whole-text search before the split.
        let tail = String(repeating: "plain log words fkml1abc\nxy9 more\n", count: 60_000)
        let from = Date()
        let tailOut = Redactor.redactLines(tail, literals: ["fkml1abc\nxy9"] + (0..<18).map { "fkunused\($0)" })
        let spent = Date().timeIntervalSince(from)
        FileHandle.standardError.write(Data("    large-input multi-line tail: \(String(format: "%.2f", spent)) s\n".utf8))
        expect(spent < 60, "multi-line tail took \(spent) s")
        expect(!tailOut.contains("xy9") && !tailOut.contains("fkml1abc"), "multi-line tail filtered")
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
        for (url, path, kept) in [("https://h.example/live/stream.m3u8", "/live/stream", false),
                                  ("https://h.example/k1abcdefgh.m3u8", "/k1abcdefgh", true)] {
            plain.destinations = [(StreamDestinationsConfig(hlsPlaylistUrl: url), StreamDestinationSecrets())]
            expectEqual(SupportBundleSecrets.literals(plain, environment: [:]).contains(path), kept, "path of \(url)")
        }
        // A short key with spaces was a literal as typed; trimming must not drop it.
        plain.destinations = [(StreamDestinationsConfig(), StreamDestinationSecrets(rtmpKey: "ab  "))]
        expect(SupportBundleSecrets.literals(plain, environment: [:]).contains("ab  "), "the as-held form is kept")
        // One stray "%" in the prose must not hide a wrapped link.
        plain.meetingTargets = ["100% join https://wrap.example/?url=https%3A%2F%2Fzoom.us%2Fj%2F1%3Fpwd%3DfkStray9q&d=1"]
        expect(SupportBundleSecrets.literals(plain, environment: [:]).contains("fkStray9q"), "wrapped link after a %")
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
        // The longest key names the core and shell really publish.
        let keys = ["recordingStartupDroppedAudioPackets", "recordingStartupDroppedVideoFrames",
                    "resourceBudgetAtStatePublication", "recordingAudioPrerollSampleCount",
                    "zoomEngineForbiddenAcquisitions", "recordingVideoPrerollFrameCount",
                    "encoderQueueDroppedAudioPackets", "completionMaximumObservedAgeNs",
                    "senderAcquisitionTimeVerified", "destinationCompletionVerified",
                    "programPixelSignature", "renderPlanSignature", "keyframeIntervalSeconds"]
        let redacted = Redactor.redact(Dictionary(uniqueKeysWithValues: keys.map { ($0, 2) })) as? JSONObject
        expectEqual(Set((redacted ?? [:]).keys), Set(keys), "diagnostic KEY names survive")
        // A secret nobody told the shell about, used as a key: the shape rule
        // applies to keys exactly as it does to values.
        let opaque = "FkUnheldOpaque0123456789abcdefGHIJ"
        var input = SupportBundleInput()
        input.coreSnapshotJson = (try? JSONSerialization.data(withJSONObject: [
            "bySession": [opaque: 1, "nested": [opaque: ["a": 2]]],
            "byUrl": ["rtmp://h.example/app/fkurlkeyq": 1]] as JSONObject)) ?? Data()
        let files = writeSecretBundle(input, shellLog: "x", coreLog: "x")
        expectAbsent(opaque, "unheld opaque token used as a key", in: files)
        expectAbsent("fkurlkeyq", "a URL used as a key", in: files)
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

    /// For the same held literals, whatever main removed from a text is removed.
    static func testBundleStillMatchesWhatMainMatched() {
        // Singletons in the text, which Foundation's search equates.
        for (held, written) in [("fkKelvinq", "fk\u{212A}elvinq"), ("fk\u{915}\u{93C}secret", "fk\u{958}secret"),
                                ("fk;semiq", "fk\u{37E}semiq")] {
            let out = Redactor.scrub("saw <\(written)> end", literals: [held])
            expect(out.utf8.elementsEqual("saw <[redacted]> end".utf8), "as main did: \(held) -> \(out)")
            var input = SupportBundleInput()
            input.knownSecretLiterals = [held]
            let files = writeSecretBundle(input, shellLog: "saw <\(written)>", coreLog: "x")
            expect(files["shell-log-tail.txt"]?.contains("saw <[redacted]>") == true, "in a bundle: \(held)")
        }
        // A literal that removes the anchor a pattern rule needs must not
        // leave the secret that only the rule knew about.
        let split = "Ab1xxxxxxxxxxxxx7788yyyyyyyyyyyyyyyy"
        let anchors: [(String, [String], [String])] = [
            ("push rtmp://ingest.example/live/stream_OLDKEY99 ok", ["/live/stream"], ["OLDKEY99"]),
            ("srt://ingest.example/live/stream?mode=caller&streamid=OLDSID99", ["/live/stream"], ["OLDSID99"]),
            ("token \(split) end", ["7788"], ["Ab1x", "yyyy"]),
            ("Authorization: Bearer abc.def.ghi", ["Bearer"], ["abc.def.ghi"]),
            ("rtmp://h.example/app/OLDKEY-unheld-123", ["rtmp"], ["OLDKEY-unheld-123"]),
            ("url http://yy:fkpw99@cdn.example/x", ["http://y"], ["fkpw99"]),
        ]
        for (text, literals, gone) in anchors {
            let out = Redactor.scrub(text, literals: literals)
            for secret in gone { expect(!out.contains(secret), "unheld \(secret) survived \(literals): \(out)") }
            let tree = Redactor.redact([text: text], literals: literals) as? JSONObject ?? [:]
            for secret in gone {
                expect(!tree.contains { "\($0.key) \($0.value)".contains(secret) }, "\(secret) in a key or value: \(tree)")
            }
        }
        // Literals that overlap, or contain rule syntax, or start with a mark.
        let exact: [(String, [String], [String])] = [
            ("&:/key:/key::/k:", [":/key:"], ["/k:"]),
            ("a fk\u{212A}ey-ABCD-tail z", ["fkKey-ABCD", "ABCD-tail"], ["tail", "ey-"]),
            ("e\u{301}eb\u{212A} z", ["\u{301}ebK"], ["eb\u{212A}"]),
        ]
        for (text, literals, probes) in exact {
            let main = MainRedactor.scrub(text, literals: literals)
            let out = Redactor.scrub(text, literals: literals)
            for probe in probes where !main.contains(probe) {
                expect(!out.contains(probe), "main removed \(probe) from \(text): main \(main), now \(out)")
            }
        }
        // Replacing a floor literal CREATES a match; an extra literal sits on
        // that match's anchor.
        let created = Redactor.scrub("tokenBearer abcdefgh", literals: ["token", "Bearer"], floor: ["token"])
        expect(!created.contains("abcdefgh"), "the floor view: \(created)")
        floorIsWiredFromTheModel()
        multiLineLiteralsThroughAWholeBundle()
        randomDifferentialAgainstMain()
    }

    /// A literal with a newline: main matched it only in its whole-file pass. If
    /// the per-line pass takes part of it first, that pass no longer finds it.
    private static func multiLineLiteralsThroughAWholeBundle() {
        let cases: [([String], [String], String)] = [
            (["Bearer", "abc fkml1abc\nfkml2def"], ["Bearer", "abc fkml1abc\nfkml2def"],
             "Bearer abc fkml1abc\nfkml2def end"),
            (["fkml1abc\nfkml2def", "fkml1abc"], ["fkml1abc\nfkml2def"], "pass fkml1abc\nfkml2def end"),
            (["token", "fkml1abc\r\nfkml2def"], ["fkml1abc\r\nfkml2def"], "token: fkml1abc\r\nfkml2def end"),
        ]
        for (literals, floor, text) in cases {
            var input = SupportBundleInput()
            input.knownSecretLiterals = literals
            input.floorSecretLiterals = floor
            input.warnings = [text]
            input.coreSnapshotJson = (try? JSONSerialization.data(withJSONObject: ["m": text])) ?? Data()
            let files = writeSecretBundle(input, shellLog: text, coreLog: text)
            for (name, contents) in files where name != "manifest.txt" {
                for secret in ["fkml1abc", "fkml2def"] {
                    expect(!has(contents, secret), "\(secret) in \(name) for \(text.debugDescription)")
                }
            }
        }
    }

    /// The app passes main's own list as the floor, and a bundle honours it.
    private static func floorIsWiredFromTheModel() {
        MainActor.assumeIsolated {
            let model = AppModel()
            model.streamKey = "fkfloorkeyq"
            model.joinPasscode = "token"
            model.srtPassphrase = "Bearer"
            let input = model.supportBundleInput()
            expectEqual(input.floorSecretLiterals ?? [], ["fkfloorkeyq", "token"], "main's list, in main's order")
            let files = writeSecretBundle(input, shellLog: "tokenBearer abcdefgh", coreLog: "x")
            expect(files["shell-log-tail.txt"]?.contains("[redacted]") == true
                   && files["shell-log-tail.txt"]?.contains("abcdefgh") == false,
                   "honoured by the export")
        }
    }

    /// Code-unit containment. String.contains compares whole characters, so a
    /// marker followed by a combining mark would read as absent.
    fileprivate static func has(_ text: String, _ needle: String) -> Bool {
        text.range(of: needle, options: .literal) != nil
    }

    /// Seeded: random lines built from rule anchors, with a fresh marker between
    /// them. A marker main removed must not appear. With no literal, identical.
    private static func randomDifferentialAgainstMain() {
        let pieces = [
            "rtmp://h.example/", "srt://i.example:9/live/stream", "://user:", "@host.example/", "?pwd=", "&sig=",
            "streamKey=", "token: ", "\"userZak\" : \"", "\"", "Bearer ", "Authorization: ", "eyJabcdefgh.", ".", " ",
            "/live/stream", "7788", ",", ";", "}", "]", "-", "_", "\\/", "%2F", "\u{212A}", "\u{301}", "\n",
            "Ab1xxxxxxxxxxxxx", "yyyyyyyyyyyyyyyy", "[redacted]", "key",
        ]
        var state: UInt64 = 0x9E3779B97F4A7C15
        func random(_ bound: Int) -> Int {
            state = state &* 6364136223846793005 &+ 1442695040888963407
            return Int((state >> 33) % UInt64(bound))
        }
        var lessThanMain: [String] = []
        var different: [String] = []
        var marker = 0
        for _ in 0..<4_000 {
            var text = ""
            var markers: [String] = []
            for _ in 0..<(3 + random(8)) {
                text += pieces[random(pieces.count)]
                if random(2) == 0 {
                    marker += 1
                    markers.append("Zq\(marker)mk")
                    text += markers.last!
                }
            }
            if Redactor.scrub(text) != MainRedactor.scrub(text) { different.append(text) }
            var literals: [String] = []
            let scalars = Array(text.unicodeScalars)
            for _ in 0..<(1 + random(3)) where scalars.count > 4 {
                if random(3) == 0 {
                    literals.append(["/live/stream", "7788", "Bearer", "rtmp", "streamKey", ":/key:", "pwd="][random(7)])
                } else {
                    let low = random(scalars.count - 4)
                    var view = String.UnicodeScalarView()
                    view.append(contentsOf: scalars[low..<min(scalars.count, low + 4 + random(14))])
                    literals.append(String(view))
                }
            }
            // Against main with its list, scrubbing with the same list and with
            // a LONGER one whose extra literals sit on rule anchors.
            let main = MainRedactor.scrub(text, literals: literals)
            let extras = ["token", "Bearer", "rtmp", "://", "streamKey", "pwd=", "eyJa", "userZak", "/live/stream"]
            let longer = literals + (0..<(1 + random(4))).map { _ in extras[random(extras.count)] }
            for out in [Redactor.scrub(text, literals: literals),
                        Redactor.scrub(text, literals: longer, floor: literals)] {
                for name in markers where !has(main, name) && has(out, name) {
                    lessThanMain.append("\(name) in \(text.debugDescription) with \(longer) floor \(literals)")
                    break
                }
            }
        }
        expectEqual(different.count, 0, "no literal: identical to main, e.g. \(different.prefix(1))")
        expectEqual(lessThanMain.count, 0, "never less than main, e.g. \(lessThanMain.prefix(2))")
    }

    /// ICU gives up on a very long segment and, unchecked, leaves the rest of
    /// the string unfiltered. The string must then be dropped, never passed.
    static func testBundleDropsTextTheMatcherGivesUpOn() {
        let later = "eyJlaterabcd.fkLaterJwtSig"
        for (label, long) in [
            ("long third segment", "eyJabcdefghij.aaaaaaaaaa." + String(repeating: "b", count: 300_000)),
            ("long second segment", "eyJabcdefghij." + String(repeating: "b", count: 400_000) + ".c"),
        ] {
            for literals in [[], ["fkheldliteral"]] {
                let out = Redactor.scrub("\(long) fkheldliteral \(later)", literals: literals)
                expect(!out.contains("fkLaterJwtSig"), "\(label): a later token survived")
                expect(!out.contains(String(repeating: "b", count: 64)), "\(label): the long token survived")
            }
        }
    }
}

extension ShellTests {
    /// A rule can only match once a held secret's ESCAPED spelling is out of the
    /// way (its "/", space or quote stops the rule). The neighbour it then
    /// removes must be removed. A plain replace of the held value cannot do it.
    static func testBundleRulesSeeTextWithEverySpellingRemoved() {
        let held = "fkpa/ss word\"q"
        let spellings = ["fkpa\\/ss word\\\"q", "fkpa%2Fss%20word%22q", "fkpa\\u002fss word\\u0022q",
                         "fkpa/ss+word%22q"]
        for spelling in spellings {
            let text = "pull ftp://fkNeighbourUser:\(spelling)@host.example/x"
            let out = Redactor.scrub(text, literals: [held])
            expect(!out.contains("fkNeighbourUser"), "the user beside \(spelling): \(out)")
        }
        // Seeded: every spelling of held secrets beside rule anchors and fresh
        // markers. Whatever a rules-after-every-spelling-removed pass takes (and
        // whatever main takes) must be gone.
        let secrets = [held, "fk\u{E9} p&w=d", "fkplainq7788"]
        var forms: [String] = []
        for secret in secrets {
            let json = String(String(decoding: (try? JSONSerialization.data(withJSONObject: [secret])) ?? Data(),
                                     as: UTF8.self).dropFirst(2).dropLast(2))
            forms += [secret, json, secret.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? "",
                      secret.utf16.map { String(format: "\\u%04x", $0) }.joined(),
                      secret.decomposedStringWithCanonicalMapping]
        }
        let anchors = ["ftp://", "://", ":", "@host.example/", "rtmp://h.example/", "?pwd=", "&sig=", "streamKey=",
                       "token: ", "\"userZak\":\"", "\"", "Bearer ", "eyJabcdefgh.", ".", " ", "/", ",", "}",
                       "Ab1xxxxxxxxxxxxx", "yyyyyyyyyyyyyyyy"]
        var state: UInt64 = 0x1234_5678_9ABC_DEF1
        func random(_ bound: Int) -> Int {
            state = state &* 6364136223846793005 &+ 1442695040888963407
            return Int((state >> 33) % UInt64(bound))
        }
        let known = LiteralScrubber(secrets)
        var misses: [String] = []
        for round in 0..<6_000 {
            var text = round % 2 == 0 ? "" : ["ftp://", "rtmp://h.example/", "token=", "Bearer "][random(4)]
            var markers: [String] = []
            for index in 0..<(3 + random(7)) {
                switch random(3) {
                case 0: text += forms[random(forms.count)]
                case 1: text += anchors[random(anchors.count)]
                default: markers.append("Zr\(round)n\(index)mk"); text += markers.last!
                }
            }
            let out = Redactor.scrub(text, literals: secrets)
            let afterSpellings = MainRedactor.scrub(known.scrub(text).text)
            let main = MainRedactor.scrub(text, literals: secrets)
            for marker in markers where has(out, marker) && (!has(afterSpellings, marker) || !has(main, marker)) {
                misses.append(text.debugDescription)
                break
            }
        }
        expectEqual(misses.count, 0, "left visible, e.g. \(misses.prefix(2))")
    }
}

extension ShellTests {
    /// A pasted "?pwd=abcd%00" yields a literal with a NUL. Foundation's search
    /// then answers with a range PAST the end, and the export took the app down.
    @MainActor static func testBundleSurvivesANulInALiteral() {
        expectEqual(Redactor.scrub("peer said abcd", literals: ["abcd\0"]), "peer said [redacted]", "text ends mid-match")
        expectEqual(LiteralScrubber(["abcd\0tailq"]).scrub("x abcd tailq abcd").text,
                    "x [redacted] [redacted] [redacted]", "each side of the NUL is matched")
        let model = AppModel()
        model.joinMeetingId = "https://zoom.us/j/1?pwd=abcd%00"
        model.warnings = ["peer said abcd"]
        let files = writeSecretBundle(model.supportBundleInput(), shellLog: "peer said abcd", coreLog: "abcd")
        expect(files["manifest.txt"] != nil, "the export finished")
        expectAbsent("abcd", "the passcode beside the NUL", in: ["warnings.txt": files["warnings.txt"] ?? "",
                                                               "core-log-tail.txt": files["core-log-tail.txt"]?
                                                                .components(separatedBy: "\n\n").last ?? ""])
    }
}

extension ShellTests {
    /// A line under 4 characters is not a literal of its own, so only the INTACT
    /// secret covers it, and it must be found before the log is split into lines.
    static func testBundleScrubsShortLinesOfAMultiLineSecret() {
        for (held, label) in [("fkml1abc\nxy9", "short last line"), ("ab1\nfkml2def", "short first line"),
                              ("q7\nfkml3ghi\nz8", "short outer lines"), ("fkml1abc\r\nxy9", "CRLF")] {
            for floor in [[held], []] {
                var input = SupportBundleInput()
                input.knownSecretLiterals = [held]
                input.floorSecretLiterals = floor
                // Twice, once beside a rule anchor that takes part of the value.
                let text = "pass \(held) end\nkeep me\ntoken: \(held) end"
                input.warnings = [text]
                let files = writeSecretBundle(input, shellLog: text, coreLog: text)
                for name in ["warnings.txt", "shell-log-tail.txt", "core-log-tail.txt"] {
                    let body = files[name]?.components(separatedBy: "\n\n").last ?? ""
                    for line in held.split(whereSeparator: \.isNewline) {
                        expect(!has(body, String(line)), "\(label) floor \(floor.count): \(line) in \(name): \(body.debugDescription)")
                    }
                    expect(has(body, "pass [redacted]") && has(body, "keep me") && has(body, " end"),
                           "\(label): the text around it survives in \(name): \(body.debugDescription)")
                }
            }
        }
        // An encoded first line, and a rule that needs its own anchor on a line
        // a cross-line match touches.
        let out = Redactor.redactLines("v fkml%31abc\nxy9 streamKey=fkneighbourq end", literals: ["fkml1abc\nxy9"])
        expect(!has(out, "xy9") && !has(out, "fkneighbourq") && has(out, " end"), "encoded and anchored: \(out)")
    }

    /// A blank line of a multi-line value is not a literal: four spaces would
    /// shred every indented line and the pretty-printed JSON.
    static func testBundleBlankLinesAreNotLiterals() {
        for held in ["fkkey1234\n    ", "    \nfkkey1234", "fkkey1234\n\t\t\t\t\nfktail567", "fkkey1234\n  ab  ",
                     "fkkey1234\u{0}    "] {
            let indented = "    indented line\n\t\t\t\ttabbed   ab   spaced"
            expectEqual(Redactor.redactLines(indented, literals: [held], floor: []), indented,
                        "ordinary whitespace survives \(held.debugDescription)")
            var input = SupportBundleInput()
            input.knownSecretLiterals = [held]
            input.floorSecretLiterals = []
            input.coreSnapshotJson = (try? JSONSerialization.data(withJSONObject: ["a": ["b": ["c": 1]]])) ?? Data()
            input.shellStateJson = input.coreSnapshotJson
            let files = writeSecretBundle(input, shellLog: indented, coreLog: "saw fkkey1234 once")
            for name in ["core-snapshot.json", "shell-state.json"] {
                let json = files[name] ?? ""
                expect((try? JSONSerialization.jsonObject(with: Data(json.utf8))) != nil && !has(json, "[redacted]"),
                       "\(name) intact for \(held.debugDescription): \(json.debugDescription)")
            }
            expect(has(files["shell-log-tail.txt"] ?? "", indented), "log indentation survives")
            expect(!has(files["core-log-tail.txt"] ?? "", "fkkey1234"), "the real line is still a literal")
        }
        // The intact value is still removed, indentation and all, where it occurs.
        let whole = Redactor.redactLines("a fkkey1234\n    b", literals: ["fkkey1234\n    "])
        expect(!has(whole, "fkkey1234") && has(whole, "b"), "intact: \(whole.debugDescription)")
    }

    /// Seeded: multi-line secrets with short and blank lines, in log text, each
    /// path as the bundle runs it (line pass, then the whole-file pass), against
    /// main's. No secret line may appear more often than in main's output.
    static func testBundleMultiLineNeverLessThanMain() {
        var state: UInt64 = 0x0BAD_5EED_1234_5678
        func random(_ bound: Int) -> Int {
            state = state &* 6364136223846793005 &+ 1442695040888963407
            return Int((state >> 33) % UInt64(bound))
        }
        func mainFile(_ text: String, _ literals: [String]) -> String {
            var out = text.split(separator: "\n", omittingEmptySubsequences: false)
                .map { MainRedactor.scrub(String($0), literals: literals) }.joined(separator: "\n")
            for literal in literals.filter({ $0.count >= 4 }) {
                out = out.replacingOccurrences(of: literal, with: MainRedactor.placeholder)
            }
            return out
        }
        func count(_ needle: String, _ text: String) -> Int {
            var total = 0
            var from = text.startIndex
            while let range = text.range(of: needle, options: .literal, range: from..<text.endIndex) {
                total += 1
                from = range.upperBound
            }
            return total
        }
        let long = ["fkml1abc", "fkml2def", "Bearer", "token", "pwd=fkq1"]
        let short = ["xy9", "ab1", "q7", "Z", "k:"]
        let blank = ["", "    ", "\t\t\t\t", "  "]
        let anchors = ["token: ", "Bearer ", "streamKey=", "rtmp://h.example/", "://u:", "@h.example/", "?pwd=",
                       " ", " end", "\"userZak\":\"", "\"", "    ", "\t\t\t\t", "plain words "]
        var misses: [String] = []
        for _ in 0..<3_000 {
            var secrets: [String] = []
            for _ in 0..<(1 + random(2)) {
                var lines = (0..<(1 + random(3))).map { _ -> String in
                    switch random(4) {
                    case 0, 1: return long[random(long.count)]
                    case 2: return short[random(short.count)]
                    default: return blank[random(blank.count)]
                    }
                }
                lines.insert(long[random(2)], at: random(lines.count + 1))
                secrets.append(lines.joined(separator: random(5) == 0 ? "\r\n" : "\n"))
            }
            var text = ""
            for _ in 0..<(3 + random(8)) {
                switch random(4) {
                case 0, 1: text += secrets[random(secrets.count)]
                case 2: text += anchors[random(anchors.count)]
                default: text += "\n"
                }
            }
            let extras = random(2) == 0 ? [] : [long[random(long.count)]]
            let main = mainFile(text, secrets)
            let known = LiteralScrubber(secrets + extras, floor: secrets)
            let head = known.scrub(Redactor.redactLines(text, literals: secrets + extras, floor: secrets)).text
            let fragments = Set(secrets.flatMap { $0.split(whereSeparator: \.isNewline).map(String.init) })
                .filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty && !MainRedactor.placeholder.contains($0) }
            for fragment in fragments where count(fragment, head) > count(fragment, main) {
                misses.append("\(fragment.debugDescription) text \(text.debugDescription) held \(secrets) + \(extras)")
                break
            }
        }
        expectEqual(misses.count, 0, "less than main, e.g. \(misses.prefix(2))")
    }
}

/// origin/main's Redactor.scrub (18904684), copied verbatim: the floor the
/// bundle scrubber must never fall below.
private enum MainRedactor {
    static let placeholder = "[redacted]"

    private struct Rule {
        let regex: NSRegularExpression
        let template: String

        init(_ pattern: String, _ template: String,
             _ options: NSRegularExpression.Options = [.caseInsensitive]) {
            // A malformed literal here is a programming error; crashing at first
            // use beats silently shipping an unfiltered bundle.
            // swiftlint:disable:next force_try
            regex = try! NSRegularExpression(pattern: pattern, options: options)
            self.template = template
        }
    }

    private static let rules: [Rule] = [
        // URL userinfo — rtmp://user:password@host/…
        Rule("(://)[^/\\s:@\"']+:[^/\\s@\"']+@", "$1" + placeholder + "@"),
        // Any rtmp/rtmps/srt URL: scheme + host survive (they name the service,
        // which IS diagnostic); everything after the authority is treated as a
        // stream key. Same rule as the Windows RtmpUrlPath.
        Rule("((?:rtmps?|srt)://[^/\\s\"']+/)[^\\s\"',}\\]]+", "$1" + placeholder),
        // Secret query parameters on any URL.
        Rule("([?&][A-Za-z0-9_-]*(?:key|token|passphrase|password|passcode|secret|pwd|zak|jwt|sig|auth)=)[^&\\s\"'<>]+",
             "$1" + placeholder),
        // "someKey": "value" in JSON-ish text.
        Rule("\"([A-Za-z0-9_-]*(?:key|token|passphrase|password|passcode|secret|pwd|zak|jwt))\"\\s*:\\s*\"[^\"]*\"",
             "\"$1\":\"" + placeholder + "\""),
        // someKey=value / someKey: value in log lines.
        Rule("([A-Za-z0-9_-]*(?:key|token|passphrase|password|passcode|secret|pwd|zak|jwt)\\s*[:=]\\s*)[^&\\s\"',;}\\]]+",
             "$1" + placeholder),
        // Authorization headers.
        Rule("\\bBearer\\s+[A-Za-z0-9\\-_.=+/]+", "Bearer " + placeholder, []),
        // JWT-shaped blobs (Zoom ZAKs and sdkJwts arrive in this shape).
        Rule("\\beyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}(?:\\.[A-Za-z0-9_-]*)?",
             placeholder, []),
        // Catch-all for opaque high-entropy tokens: a >=28 char run of
        // [A-Za-z0-9_-] carrying lower AND upper AND digits. The lookbehind
        // excludes runs preceded by / or . so FILE PATHS are never touched, and
        // the mixed-case requirement spares UUIDs, lowercase ids and
        // SCREAMING_SNAKE env-var names.
        Rule("(?<![A-Za-z0-9_\\-/.])(?=[A-Za-z0-9_-]*[a-z])(?=[A-Za-z0-9_-]*[A-Z])(?=[A-Za-z0-9_-]*[0-9])[A-Za-z0-9_-]{28,}",
             placeholder, []),
    ]

    static func scrub(_ text: String, literals: [String] = []) -> String {
        var result = text
        // Layer 3 — KNOWN LITERALS. The shell holds the operator's live stream
        // key and meeting passcode in memory; an identity match cannot be fooled
        // by a shape the pattern rules did not anticipate.
        for literal in literals where literal.count >= 4 {
            result = result.replacingOccurrences(of: literal, with: placeholder)
        }
        guard !result.isEmpty else { return result }
        for rule in rules {
            let range = NSRange(result.startIndex..., in: result)
            result = rule.regex.stringByReplacingMatches(
                in: result, options: [], range: range, withTemplate: rule.template)
        }
        return result
    }
}
