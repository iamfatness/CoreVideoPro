// The support bundle's KNOWN LITERAL layer: which secret values the shell holds,
// and how a held value is found again after something has escaped or URL-encoded
// it. Local only; nothing here sends a byte anywhere.

import Foundation

/// The ONE list of live secret values scrubbed by identity from a support bundle.
/// A new stored secret belongs here (and in ShellTestsBundleSecrets).
enum SupportBundleSecrets {
    /// The secrets that live in operator state (the model, or on a headless
    /// export the Keychain and prefs it is restored from).
    struct Held {
        /// The editable destinations and every copy frozen at a Start this
        /// session: one of those is, or was, on air after the fields changed.
        var destinations: [(config: StreamDestinationsConfig, secrets: StreamDestinationSecrets)] = []
        var joinPasscode = ""
        /// Meeting fields as typed: a join link carries its passcode in ?pwd=.
        var meetingTargets: [String] = []
    }

    /// The core's own floor (FfmpegSenderDiagnostics isPlausibleSecret). A Stream
    /// ID is often a plain word; a shorter one would shred ordinary text.
    static let minimumStreamIdLength = 6
    /// Nor is a Stream ID that is one of the bundle's own words.
    private static let ordinaryWords: Set<String> = [
        "camera", "default", "output", "program", "publish", "record", "recording",
        "request", "session", "stream", "streaming", "studio",
    ]
    /// Secrets the media core reads from the environment it inherits from the
    /// shell (ZoomEngineRuntime.cpp), plus the control API token.
    private static let environmentNames = [
        "COREVIDEO_CONTROL_TOKEN", "COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN",
        "COREVIDEO_ZOOM_MEETING_PASSCODE", "COREVIDEO_ZOOM_ON_BEHALF_TOKEN",
        "COREVIDEO_ZOOM_SDK_JWT", "COREVIDEO_ZOOM_USER_ZAK",
    ]

    /// Every ?pwd= in a field, whatever the host and whatever surrounds the
    /// link (a pasted invite, "&amp;", a wrapped link that encodes the whole URL).
    /// Decoded; the matcher re-finds encoded copies. Prose around a link can
    /// stick to the value, so each value is offered in three cuts.
    private static func linkPasscodes(_ field: String) -> [String] {
        var texts = [field]
        if let decoded = field.removingPercentEncoding, decoded != field { texts.append(decoded) }
        var found: [String] = []
        for text in texts {
            let spaced = text.replacingOccurrences(of: "&amp;", with: "&")
            for part in spaced.components(separatedBy: CharacterSet(charactersIn: "?&")).dropFirst()
            where part.lowercased().hasPrefix("pwd=") {
                // As written, up to what ends a query value.
                let whole = part.dropFirst(4).prefix { !$0.isWhitespace && $0 != "#" }
                // The characters Zoom's own passcode tokens use, and that
                // without a sentence's full stop.
                let token = String(whole.prefix { $0.isLetter || $0.isNumber || "._-%~+=".contains($0) })
                let bare = token.trimmingCharacters(in: CharacterSet(charactersIn: "."))
                found += [String(whole), token, bare].map { $0.removingPercentEncoding ?? $0 }
            }
        }
        return found
    }

    /// The HLS ingest URL and its path/stem, which can be the credential (the
    /// core scrubs the same two: HlsOutputPolicy.h). Only a VALID playlist URL:
    /// a half-typed "https://" as a literal would shred every URL, and switch
    /// off the pattern rule that removes a user:password.
    private static func hlsLiterals(_ field: String) -> [String] {
        let url = field.trimmingCharacters(in: .whitespacesAndNewlines)
        guard StreamDestinationPolicy.validateHls(url) == nil,
              let scheme = url.range(of: "://"),
              let slash = url[scheme.upperBound...].firstIndex(of: "/") else { return [] }
        let path = String(url[slash...].dropLast(".m3u8".count))
        // A one-part path ("/live") is an ordinary word, not an origin id.
        return path.dropFirst().contains("/") ? [url, path] : [url]
    }

    /// Held values plus the stored sign-ins and environment secrets. Trimmed,
    /// because the core is sent (and logs) the trimmed value.
    static func literals(_ held: Held,
                         environment: [String: String] = ProcessInfo.processInfo.environment) -> [String] {
        var values = [held.joinPasscode]
        values += held.meetingTargets.flatMap(linkPasscodes)
        for destination in held.destinations {
            values += [destination.secrets.rtmpKey, destination.secrets.srtPassphrase]
            values += hlsLiterals(destination.config.hlsPlaylistUrl)
            let streamId = destination.config.srtStreamId.trimmingCharacters(in: .whitespacesAndNewlines)
            if streamId.count >= minimumStreamIdLength, !ordinaryWords.contains(streamId.lowercased()) {
                values.append(streamId)
            }
        }
        values += SocialRedaction.secretLiterals()
        if let data = ZoomOAuth.keychain.read(),
           let tokens = try? JSONDecoder().decode(ZoomOAuth.Tokens.self, from: data) {
            values += [tokens.accessToken, tokens.refreshToken]
        }
        values += environmentNames.compactMap { environment[$0] }
        return values.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
    }

    /// What a headless export (no model) scrubs: the same values, read from where
    /// the model restores them.
    static func storedHeld(prefs: ShellPrefs) -> Held {
        Held(destinations: [(prefs.streamDestinations ?? StreamDestinationsConfig(), StreamDestinationSecrets(
                 rtmpKey: StreamKeychain.load(),
                 srtPassphrase: StreamKeychain.load(account: StreamKeychain.srtPassphraseAccount)))],
             meetingTargets: [prefs.joinMeetingId] + prefs.recentMeetings)
    }
}

/// Finds known secrets by identity in text that may have escaped them. Each
/// character of a literal may independently appear:
///   - as itself;
///   - percent-encoded, in either hex case, any number of times ("%2F", "%252F"),
///     and a space also as "+";
///   - backslash-escaped as JSON writes it, nested up to three times
///     (\" \\ \/ \n \uXXXX).
/// So it does not matter which characters a given encoder chose to escape.
///
/// A character may also be in any canonically equivalent scalar spelling (NFC,
/// NFD, partly composed, marks reordered), chosen per character and combined
/// with the above. Only the LITERAL is expanded; the scanned text is never
/// normalized, so nothing but a match is altered.
///
/// A hand-written matcher, not a regex: it tracks the set of positions reached
/// instead of backtracking, so its cost is linear in the text.
struct LiteralScrubber {
    static let minimumLength = 4
    private static let maxBackslashes = 8
    private static let maxStates = 32

    /// Beyond this many combining marks on one character, only the held, NFC and
    /// NFD spellings are matched (the orderings grow factorially).
    private static let maxReorderedMarks = 4

    /// One scalar of one spelling.
    private struct Unit {
        var raw: [UInt8]
        var utf16: [UInt16]
        /// The letter that follows the backslashes in this character's short escape.
        var escape: UInt8?
    }

    /// One character (grapheme cluster) of a literal: its equivalent spellings.
    private typealias Spellings = [[Unit]]

    private let literals: [[Spellings]]
    /// The values as held, longest first, for the Foundation search below.
    private let held: [String]
    /// Per literal, the bytes that can follow a run of backslashes its match
    /// STARTS with: lets a position in a long run be refused without matching.
    private let afterBackslashes: [[Bool]]
    /// Bytes a match can start with: the cheap test every position takes first.
    private let starts: [Bool]

    var isEmpty: Bool { literals.isEmpty }

    init(_ values: [String]) {
        held = Set(values).filter { $0.count >= Self.minimumLength }.sorted { $0.utf8.count > $1.utf8.count }
        var parsed: [[Spellings]] = []
        var starts = [Bool](repeating: false, count: 256)
        for value in Set(values) where value.count >= Self.minimumLength {
            let characters: [Spellings] = value.map { character in
                Self.spellings(of: character).map { $0.map(Self.unit) }
            }
            for spelling in characters[0] {
                starts[Int(spelling[0].raw[0])] = true
                if spelling[0].raw == [0x20] { starts[Int(UInt8(ascii: "+"))] = true }
            }
            starts[Int(UInt8(ascii: "%"))] = true
            starts[Int(UInt8(ascii: "\\"))] = true
            parsed.append(characters)
        }
        literals = parsed
        afterBackslashes = parsed.map { literal in
            var allowed = [Bool](repeating: false, count: 256)
            allowed[Int(UInt8(ascii: "u"))] = true
            allowed[Int(UInt8(ascii: "U"))] = true
            // A run longer than the cap: only a literal that begins with a backslash.
            for spelling in literal[0] {
                if let escape = spelling[0].escape { allowed[Int(escape)] = true }
                if spelling[0].raw == [UInt8(ascii: "\\")] { allowed = [Bool](repeating: true, count: 256) }
            }
            return allowed
        }
        self.starts = starts
    }

    private static func unit(_ scalar: Unicode.Scalar) -> Unit {
        let text = String(scalar)
        let escape: UInt8?
        switch scalar {
        case "\"", "/", "'": escape = UInt8(ascii: scalar)
        case "\n": escape = UInt8(ascii: "n")
        case "\r": escape = UInt8(ascii: "r")
        case "\t": escape = UInt8(ascii: "t")
        case "\u{8}": escape = UInt8(ascii: "b")
        case "\u{C}": escape = UInt8(ascii: "f")
        default: escape = nil
        }
        return Unit(raw: Array(text.utf8), utf16: Array(text.utf16), escape: escape)
    }

    /// The scalar sequences canonically equivalent to `character`: as held, NFC,
    /// NFD, and every ordering of its marks composed up to each point. (Singleton
    /// code points such as U+212B are matched when held, but are not invented:
    /// no normalization form produces one.)
    private static func spellings(of character: Character) -> [[Unicode.Scalar]] {
        let text = String(character)
        func scalars(_ s: String) -> [Unicode.Scalar] { Array(s.unicodeScalars) }
        func string(_ s: ArraySlice<Unicode.Scalar>) -> String {
            var out = String.UnicodeScalarView()
            out.append(contentsOf: s)
            return String(out)
        }
        let decomposed = scalars(text.decomposedStringWithCanonicalMapping)
        var found = [scalars(text)]
        func offer(_ candidate: [Unicode.Scalar]) {
            // Swift's String equality IS canonical equivalence.
            if !found.contains(candidate), string(candidate[...]) == text { found.append(candidate) }
        }
        offer(scalars(text.precomposedStringWithCanonicalMapping))
        offer(decomposed)
        if decomposed.count > 1 {
            var orders = [decomposed]
            if decomposed.count - 1 <= maxReorderedMarks {
                orders = permutations(Array(decomposed.dropFirst())).map { [decomposed[0]] + $0 }
            }
            for order in orders {
                offer(order)
                for length in 2...order.count {
                    offer(scalars(string(order[..<length]).precomposedStringWithCanonicalMapping) + order[length...])
                }
            }
        }
        return found
    }

    private static func permutations(_ items: [Unicode.Scalar]) -> [[Unicode.Scalar]] {
        if items.count < 2 { return [items] }
        return items.indices.flatMap { index -> [[Unicode.Scalar]] in
            var rest = items
            let head = rest.remove(at: index)
            return permutations(rest).map { [head] + $0 }
        }
    }

    /// `text` with every occurrence replaced. Occurrences are all found in the
    /// ORIGINAL text and overlapping ones merged, so a secret contained in (or
    /// overlapping) another never leaves the other's remainder behind.
    func scrub(_ text: String) -> (text: String, hits: Int) {
        if literals.isEmpty || text.isEmpty { return (text, 0) }
        var (result, hits) = scrubSpellings(text)
        // Foundation's search equates more than the spellings above (a singleton
        // code point in the text, such as the Kelvin sign for "K"). Run it too,
        // so this layer never matches less than a plain replace would.
        for value in held where result.range(of: value) != nil {
            hits += result.components(separatedBy: value).count - 1
            result = result.replacingOccurrences(of: value, with: Redactor.placeholder)
        }
        return (result, hits)
    }

    private func scrubSpellings(_ text: String) -> (text: String, hits: Int) {
        let bytes = Array(text.utf8)
        var found: [Range<Int>] = []
        bytes.withUnsafeBufferPointer { buffer in
            for start in 0..<buffer.count where starts[Int(buffer[start])] {
                var end = 0
                let follower = buffer[start] == UInt8(ascii: "\\") ? byteAfterBackslashes(buffer, start) : nil
                for (index, literal) in literals.enumerated() {
                    if let follower, !afterBackslashes[index][Int(follower)] { continue }
                    if let candidate = matchEnd(literal, buffer, start) { end = max(end, candidate) }
                }
                guard end > start else { continue }
                if let last = found.last, start < last.upperBound {
                    found[found.count - 1] = last.lowerBound..<max(last.upperBound, end)
                } else {
                    found.append(start..<end)
                }
            }
        }
        if found.isEmpty { return (text, 0) }
        var out: [UInt8] = []
        out.reserveCapacity(bytes.count)
        var copied = 0
        for range in found {
            out += bytes[copied..<range.lowerBound]
            out += Array(Redactor.placeholder.utf8)
            copied = range.upperBound
        }
        out += bytes[copied...]
        return (String(decoding: out, as: UTF8.self), found.count)
    }

    /// The byte after the (capped) run of backslashes at `start`; 0 at the end.
    private func byteAfterBackslashes(_ text: UnsafeBufferPointer<UInt8>, _ start: Int) -> UInt8 {
        var position = start + 1
        while position - start < Self.maxBackslashes, position < text.count, text[position] == text[start] { position += 1 }
        return position < text.count ? text[position] : 0
    }

    /// The furthest end of `literal` starting at `start`, or nil.
    private func matchEnd(_ literal: [Spellings], _ text: UnsafeBufferPointer<UInt8>, _ start: Int) -> Int? {
        var first: [Int] = []
        advance(literal[0], text, start, &first)
        if first.isEmpty { return nil }
        var states = first
        for character in literal.dropFirst() {
            var next: [Int] = []
            for state in states { advance(character, text, state, &next) }
            if next.isEmpty { return nil }
            states = next
        }
        return states.max()
    }

    /// Appends every position just past any spelling of `character` at `at`.
    private func advance(_ character: Spellings, _ text: UnsafeBufferPointer<UInt8>, _ at: Int, _ out: inout [Int]) {
        // Nearly every character: one spelling of one scalar.
        if character.count == 1, character[0].count == 1 {
            advance(character[0][0], text, at, &out)
            return
        }
        for spelling in character {
            var states = [at]
            for unit in spelling {
                var next: [Int] = []
                for state in states { advance(unit, text, state, &next) }
                states = next
                if states.isEmpty { break }
            }
            for state in states where out.count < Self.maxStates && !out.contains(state) { out.append(state) }
        }
    }

    /// Appends every position just past one spelling of `unit` at `at`.
    private func advance(_ unit: Unit, _ text: UnsafeBufferPointer<UInt8>, _ at: Int, _ out: inout [Int]) {
        guard at < text.count else { return }
        func add(_ position: Int) {
            if out.count < Self.maxStates, !out.contains(position) { out.append(position) }
        }
        let lead = text[at]
        // As itself.
        if lead == unit.raw[0], at + unit.raw.count <= text.count,
           unit.raw.indices.allSatisfy({ text[at + $0] == unit.raw[$0] }) {
            add(at + unit.raw.count)
        }
        if lead == UInt8(ascii: "+"), unit.raw == [0x20] { add(at + 1) }
        if lead == UInt8(ascii: "%") {
            var positions = [at]
            for byte in unit.raw {
                var next: [Int] = []
                for position in positions where position < text.count && text[position] == UInt8(ascii: "%") {
                    var q = position + 1
                    // "%25" repeated is an already-encoded "%" being encoded again.
                    while let value = hex(text, q, 2) {
                        if value == UInt32(byte), !next.contains(q + 2) { next.append(q + 2) }
                        if value != 0x25 || next.count >= Self.maxStates { break }
                        q += 2
                    }
                }
                positions = next
                if positions.isEmpty { break }
            }
            positions.forEach(add)
        }
        if lead == UInt8(ascii: "\\") {
            var run = 1
            while run < Self.maxBackslashes, at + run < text.count, text[at + run] == lead { run += 1 }
            let after = at + run
            // A literal backslash, doubled once per level of escaping.
            if unit.raw == [lead], run > 1 { (2...run).forEach { add(at + $0) } }
            if let escape = unit.escape, after < text.count, text[after] == escape { add(after + 1) }
            if after < text.count, text[after] | 0x20 == UInt8(ascii: "u"),
               let end = unicodeEscapeEnd(unit.utf16, text, at) { add(end) }
        }
    }

    /// The end of \uXXXX (one per UTF-16 unit, so a surrogate pair is two).
    private func unicodeEscapeEnd(_ units: [UInt16], _ text: UnsafeBufferPointer<UInt8>, _ at: Int) -> Int? {
        var position = at
        for unit in units {
            var run = 0
            while run < Self.maxBackslashes, position < text.count, text[position] == UInt8(ascii: "\\") {
                run += 1
                position += 1
            }
            // \uXXXX, or \UXXXX as an NSDictionary description writes it.
            guard run > 0, position < text.count, text[position] | 0x20 == UInt8(ascii: "u"),
                  hex(text, position + 1, 4) == UInt32(unit) else { return nil }
            position += 5
        }
        return position
    }

    private func hex(_ text: UnsafeBufferPointer<UInt8>, _ at: Int, _ digits: Int) -> UInt32? {
        guard at + digits <= text.count else { return nil }
        var value: UInt32 = 0
        for index in at..<(at + digits) {
            let byte = text[index]
            switch byte {
            case 0x30...0x39: value = value * 16 + UInt32(byte - 0x30)
            case 0x41...0x46: value = value * 16 + UInt32(byte - 0x37)
            case 0x61...0x66: value = value * 16 + UInt32(byte - 0x57)
            default: return nil
            }
        }
        return value
    }
}
