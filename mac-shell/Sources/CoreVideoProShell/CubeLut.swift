// The `.cube` 3D LUT parser (grading Phase 2, Task 1; spec §3.2). A line-for-line twin of the
// Windows CubeLutParser (native-shell/CoreVideoPro.MediaCore/Models/CubeLutParser.cs): the same
// rules, order and messages. contracts/grade.fixtures.json `cubes` pins the verdict the core
// (compositor/CubeLut.h) and C# give on every row, so the three cannot drift.
//
// Rules: TITLE lines and `#` comments are ignored; exactly one LUT_3D_SIZE 2…33; optional single
// DOMAIN_MIN / DOMAIN_MAX with three values; exactly size³ RGB triples, red fastest, each within
// ±16; domain max above min (compared as Float, like C#) and both within ±16; at most 2 MB of UTF-8.

import Foundation

struct CubeLut: Equatable {
    let size: Int
    let domainMin: [Double]
    let domainMax: [Double]
    let samples: [[Double]]
}

enum CubeLutParser {
    static let maxBytes = 2_000_000

    static func parse(_ text: String) throws -> CubeLut {
        func fail(_ message: String) -> GradeValidationError { GradeValidationError(message: message) }
        guard text.utf8.count <= maxBytes else { throw fail("LUT files are limited to 2 MB.") }
        var size = 0
        var minimum = [0.0, 0.0, 0.0], maximum = [1.0, 1.0, 1.0]
        var minSeen = false, maxSeen = false
        var samples: [[Double]] = []
        // components(separatedBy:) splits a CRLF file on its "\n" (a Swift Character split would
        // keep "\r\n" as one grapheme); C# Split('\n') then Trim() removes the "\r".
        for raw in text.components(separatedBy: "\n") {
            let line = (raw.components(separatedBy: "#").first ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if line.isEmpty { continue }
            let tokens = line.split(whereSeparator: { $0.isWhitespace }).map(String.init)
            if tokens[0] == "TITLE" { continue }
            if tokens[0] == "LUT_3D_SIZE" {
                guard size == 0, tokens.count == 2, let parsed = Int(tokens[1]), (2...33).contains(parsed) else {
                    throw fail("Supported 3D LUT dimensions are 2 through 33; duplicate size declarations are invalid.")
                }
                size = parsed
                continue
            }
            if tokens[0] == "DOMAIN_MIN" || tokens[0] == "DOMAIN_MAX" {
                let isMinimum = tokens[0] == "DOMAIN_MIN"
                guard !(isMinimum && minSeen), !(!isMinimum && maxSeen), tokens.count == 4 else {
                    throw fail("Invalid LUT domain.")
                }
                let values = try tokens.dropFirst().map(number)
                if isMinimum { minimum = values; minSeen = true } else { maximum = values; maxSeen = true }
                continue
            }
            guard size != 0, tokens.count == 3, samples.count < size * size * size else {
                throw fail("Unsupported or malformed .cube content.")
            }
            let sample = try tokens.map(number)
            guard sample.allSatisfy({ $0 >= -16 && $0 <= 16 }) else { throw fail("LUT outputs must be within -16 to 16.") }
            samples.append(sample)
        }
        guard size != 0, samples.count == size * size * size,
              (0..<3).allSatisfy({ Float(maximum[$0]) > Float(minimum[$0]) && abs(minimum[$0]) <= 16 && abs(maximum[$0]) <= 16 })
        else { throw fail("LUT sample count or domain is invalid.") }
        return CubeLut(size: size, domainMin: minimum, domainMax: maximum, samples: samples)
    }

    /// C# double.TryParse(NumberStyles.Float, InvariantCulture) + IsFinite: an optional sign,
    /// digits with an optional decimal point, an optional exponent. Never hex, never inf/nan.
    private static func number(_ token: String) throws -> Double {
        let scalars = Array(token.unicodeScalars)
        var index = 0
        func digits() -> Int {
            let start = index
            while index < scalars.count, ("0"..."9").contains(scalars[index]) { index += 1 }
            return index - start
        }
        if index < scalars.count, scalars[index] == "+" || scalars[index] == "-" { index += 1 }
        var mantissa = digits()
        if index < scalars.count, scalars[index] == "." { index += 1; mantissa += digits() }
        var ok = mantissa > 0
        if ok, index < scalars.count, scalars[index] == "e" || scalars[index] == "E" {
            index += 1
            if index < scalars.count, scalars[index] == "+" || scalars[index] == "-" { index += 1 }
            ok = digits() > 0
        }
        guard ok, index == scalars.count, let value = Double(token), value.isFinite else {
            throw GradeValidationError(message: "LUT contains a non-finite or invalid number.")
        }
        return value
    }
}
