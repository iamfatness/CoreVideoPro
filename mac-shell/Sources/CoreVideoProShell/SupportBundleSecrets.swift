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
        var streamKey = ""
        var srtPassphrase = ""
        var srtStreamId = ""
        var joinPasscode = ""
        /// Meeting fields as typed: a Zoom join link carries its passcode in ?pwd=.
        var meetingTargets: [String] = []
    }

    /// The core's own floor (FfmpegSenderDiagnostics isPlausibleSecret). A Stream
    /// ID is often a plain word; a shorter one would shred ordinary text.
    static let minimumStreamIdLength = 6

    /// Held values plus the stored sign-ins and the control API token. Trimmed,
    /// because the core is sent (and logs) the trimmed value.
    static func literals(_ held: Held,
                         environment: [String: String] = ProcessInfo.processInfo.environment) -> [String] {
        var values = [held.streamKey, held.srtPassphrase, held.joinPasscode]
        values += held.meetingTargets.map { AppModel.parseZoomTarget($0, fallbackPasscode: "").passcode }
        let streamId = held.srtStreamId.trimmingCharacters(in: .whitespacesAndNewlines)
        if streamId.count >= minimumStreamIdLength { values.append(streamId) }
        values += SocialRedaction.secretLiterals()
        if let data = ZoomOAuth.keychain.read(),
           let tokens = try? JSONDecoder().decode(ZoomOAuth.Tokens.self, from: data) {
            values += [tokens.accessToken, tokens.refreshToken]
        }
        values.append(environment["COREVIDEO_CONTROL_TOKEN"] ?? "")
        return values.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
    }

    /// What a headless export (no model) scrubs: the same values, read from where
    /// the model restores them.
    static func storedHeld(prefs: ShellPrefs) -> Held {
        Held(streamKey: StreamKeychain.load(),
             srtPassphrase: StreamKeychain.load(account: StreamKeychain.srtPassphraseAccount),
             srtStreamId: prefs.streamDestinations?.srtStreamId ?? "",
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
/// A hand-written matcher, not a regex: it tracks the set of positions reached
/// instead of backtracking, so its cost is linear in the text.
struct LiteralScrubber {
    static let minimumLength = 4
    private static let maxBackslashes = 8
    private static let maxStates = 32

    private struct Unit {
        var raw: [UInt8]
        var utf16: [UInt16]
        /// The letter that follows the backslashes in this character's short escape.
        var escape: UInt8?
    }

    private let literals: [[Unit]]
    /// Bytes a match can start with: the cheap test every position takes first.
    private let starts: [Bool]

    var isEmpty: Bool { literals.isEmpty }

    init(_ values: [String]) {
        var parsed: [[Unit]] = []
        var starts = [Bool](repeating: false, count: 256)
        for value in Set(values) where value.count >= Self.minimumLength {
            let units = value.unicodeScalars.map { scalar -> Unit in
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
            starts[Int(units[0].raw[0])] = true
            if units[0].raw == [0x20] { starts[Int(UInt8(ascii: "+"))] = true }
            starts[Int(UInt8(ascii: "%"))] = true
            starts[Int(UInt8(ascii: "\\"))] = true
            parsed.append(units)
        }
        literals = parsed
        self.starts = starts
    }

    /// `text` with every occurrence replaced. Occurrences are all found in the
    /// ORIGINAL text and overlapping ones merged, so a secret contained in (or
    /// overlapping) another never leaves the other's remainder behind.
    func scrub(_ text: String) -> (text: String, hits: Int) {
        if literals.isEmpty || text.isEmpty { return (text, 0) }
        let bytes = Array(text.utf8)
        var found: [Range<Int>] = []
        bytes.withUnsafeBufferPointer { buffer in
            for start in 0..<buffer.count where starts[Int(buffer[start])] {
                var end = 0
                for literal in literals {
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

    /// The furthest end of `literal` starting at `start`, or nil.
    private func matchEnd(_ literal: [Unit], _ text: UnsafeBufferPointer<UInt8>, _ start: Int) -> Int? {
        var first: [Int] = []
        advance(literal[0], text, start, &first)
        if first.isEmpty { return nil }
        var states = first
        for unit in literal.dropFirst() {
            var next: [Int] = []
            for state in states { advance(unit, text, state, &next) }
            if next.isEmpty { return nil }
            states = next
        }
        return states.max()
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
            if after < text.count, text[after] == UInt8(ascii: "u"),
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
            guard run > 0, position < text.count, text[position] == UInt8(ascii: "u"),
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
