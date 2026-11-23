// GraphicsLocalData — a local CSV or JSON dataset read exactly as Windows reads it
// (G4-G7 Task 5). Port of MC/Services/GraphicsLocalData.cs: ReadAsync (1 MiB, strict
// UTF-8, leading BOMs stripped), Parse, Valid, WithKey, Project. CSV follows
// Microsoft.VisualBasic.FileIO.TextFieldParser with HasFieldsEnclosedInQuotes and its
// default whitespace trim, quirks included (blank lines skipped, also inside a quoted
// field; a quoted field followed by trailing white space on the LAST line yields an extra
// empty field). JSON uses a purpose-built scanner, not JSONSerialization: raw number text
// is kept ("1.50" stays "1.50"), property order is kept, depth is capped at 4 as
// JsonDocumentOptions.MaxDepth, and names differing only in case are refused. Pinned by
// contracts/graphics-shell/data.vectors.json.

import Foundation

struct GraphicsDataError: Error, Equatable {
    let message: String
    static let malformed = GraphicsDataError(message: "The data file is malformed.")
}

struct GraphicsDataColumn: Equatable {
    let id: String
    let kind: String   // text | number | boolean
}

struct GraphicsDataRow: Equatable {
    let key: String
    let cells: [String]
}

struct GraphicsLocalData: Equatable {
    static let byteLimit = 1024 * 1024, maxRows = 1000, maxColumns = 32, maxVisibleRows = 12, maxVisibleColumns = 8
    static let tooLarge = GraphicsDataError(message: "Data files can contain at most 1 MiB.")

    let columns: [GraphicsDataColumn]
    let rows: [GraphicsDataRow]

    // ── reading ──────────────────────────────────────────────────────────────

    static func read(url: URL, key: String?) throws -> GraphicsLocalData {
        let size = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int) ?? 0
        if size > byteLimit { throw tooLarge }
        guard let data = try? Data(contentsOf: url) else { throw GraphicsDataError(message: "The data file could not be read.") }
        if data.count > byteLimit { throw tooLarge }
        guard var text = strictUTF8(data) else { throw GraphicsDataError(message: "Use a UTF-8 CSV or JSON file.") }
        while text.unicodeScalars.first == "\u{FEFF}" { text.unicodeScalars.removeFirst() }
        return try parse(text, format: url.pathExtension, key: key)
    }

    /// UTF8Encoding(false, throwOnInvalid: true).GetString: nil on any invalid sequence.
    static func strictUTF8(_ data: Data) -> String? {
        var scalars = String.UnicodeScalarView()
        var parser = Unicode.UTF8.ForwardParser()
        var iterator = data.makeIterator()
        while true {
            switch parser.parseScalar(from: &iterator) {
            case .valid(let encoded): scalars.append(Unicode.UTF8.decode(encoded))
            case .emptyInput: return String(scalars)
            case .error: return nil
            }
        }
    }

    static func parse(_ text: String, format: String, key: String?) throws -> GraphicsLocalData {
        if text.utf8.count > byteLimit { throw tooLarge }
        let (headings, cells, kinds): ([String], [[String]], [String])
        switch format.lowercased() {
        case "csv": (headings, cells, kinds) = try readCsv(text)
        case "json": (headings, cells, kinds) = try readJson(text)
        default: throw GraphicsDataError(message: "Choose a CSV or JSON file.")
        }
        let keyGiven = key.map { !isNullOrWhiteSpace($0) } ?? false
        let keyIndex = keyGiven ? headings.firstIndex(of: trim(key!)) ?? -1 : -1
        if keyGiven && keyIndex < 0 { throw GraphicsDataError(message: "The selected row key is missing.") }
        let rows = cells.enumerated().map { index, row in
            GraphicsDataRow(key: keyIndex < 0 ? String(format: "%04d", index + 1) : row[keyIndex], cells: row)
        }
        let data = GraphicsLocalData(columns: zip(headings, kinds).map { GraphicsDataColumn(id: $0, kind: $1) }, rows: rows)
        guard data.isValid else { throw GraphicsDataError(message: "Rows need unique nonempty keys, consistent columns, and bounded values.") }
        return data
    }

    func withKey(_ column: String?) throws -> GraphicsLocalData {
        guard let column else { return self }
        let index = try indexOf(column)
        let keyed = GraphicsLocalData(columns: columns, rows: rows.map { GraphicsDataRow(key: $0.cells[index], cells: $0.cells) })
        guard keyed.isValid else { throw GraphicsDataError(message: "The selected key needs a unique nonempty value in each row.") }
        return keyed
    }

    var isValid: Bool {
        guard (1...Self.maxColumns).contains(columns.count),
              columns.allSatisfy({ (1...128).contains($0.id.utf16.count) && ["text", "number", "boolean"].contains($0.kind) }),
              Set(columns.map { $0.id.uppercased() }).count == columns.count,
              rows.count <= Self.maxRows else { return false }
        for row in rows {
            guard (1...256).contains(row.key.utf16.count), !Self.isNullOrWhiteSpace(row.key), row.cells.count == columns.count else { return false }
            for (index, value) in row.cells.enumerated() {
                guard value.utf16.count <= 4096 else { return false }
                if value.isEmpty { continue }
                switch columns[index].kind {
                case "number": if !Self.isNumber(value) { return false }
                case "boolean": if Self.parseBool(value) == nil { return false }
                default: break
                }
            }
        }
        guard Set(rows.map(\.key)).count == rows.count else { return false }
        return rows.reduce(0) { $0 + $1.cells.reduce(0) { $0 + $1.utf8.count } } <= Self.byteLimit
    }

    /// The visible cells of up to 12 rows: file order, or descending by a numeric column (blank last,
    /// ties in ordinal key order).
    func project(columns selected: [String], orderBy: String?, limit: Int) throws -> [[String]] {
        guard (1...Self.maxVisibleColumns).contains(selected.count), (1...Self.maxVisibleRows).contains(limit),
              Set(selected).count == selected.count else {
            throw GraphicsDataError(message: "Select up to eight distinct columns and twelve rows.")
        }
        let indices = try selected.map(indexOf)
        var ordered = rows
        if let orderBy, !Self.isNullOrWhiteSpace(orderBy) {
            let sort = try indexOf(orderBy)
            guard columns[sort].kind == "number" else { throw GraphicsDataError(message: "Leaderboard sorting needs a numeric column.") }
            func value(_ row: GraphicsDataRow) -> Double { Self.number(row.cells[sort]) ?? -.infinity }
            ordered.sort { a, b in
                let x = value(a), y = value(b)
                if x != y { return x > y }
                return Array(a.key.utf16).lexicographicallyPrecedes(Array(b.key.utf16))
            }
        }
        return ordered.prefix(limit).map { row in indices.map { row.cells[$0] } }
    }

    func indexOf(_ id: String) throws -> Int {
        guard let index = columns.firstIndex(where: { $0.id == id }) else { throw GraphicsDataError(message: "Data column '\(id)' is missing.") }
        return index
    }

    // ── document shape (GraphicsLocalData record, Web JSON) ────────────────

    var json: JSONObject {
        ["columns": columns.map { ["id": $0.id, "kind": $0.kind] as JSONObject },
         "rows": rows.map { ["key": $0.key, "cells": $0.cells] as JSONObject }]
    }

    init(columns: [GraphicsDataColumn], rows: [GraphicsDataRow]) { self.columns = columns; self.rows = rows }

    init?(json: JSONObject) {
        guard let cols = json["columns"] as? [JSONObject], let rows = json["rows"] as? [JSONObject] else { return nil }
        var columns: [GraphicsDataColumn] = []
        for c in cols {
            guard let id = c["id"] as? String, let kind = c["kind"] as? String else { return nil }
            columns.append(GraphicsDataColumn(id: id, kind: kind))
        }
        var out: [GraphicsDataRow] = []
        for r in rows {
            guard let key = r["key"] as? String, let cells = r["cells"] as? [String] else { return nil }
            out.append(GraphicsDataRow(key: key, cells: cells))
        }
        self.columns = columns; self.rows = out
    }

    // ── value rules (.NET) ─────────────────────────────────────────────────

    /// double.TryParse(value, NumberStyles.Float, InvariantCulture) && double.IsFinite.
    static func isNumber(_ value: String) -> Bool { number(value) != nil }

    static func number(_ value: String) -> Double? {
        let ws: Set<UInt32> = [9, 10, 11, 12, 13, 32]
        var s = Array(value.unicodeScalars)
        while let f = s.first, ws.contains(f.value) { s.removeFirst() }
        while let l = s.last, ws.contains(l.value) || l.value == 0 { s.removeLast() }
        var i = 0
        func digit(_ k: Int) -> Bool { k < s.count && s[k].value >= 48 && s[k].value <= 57 }
        if i < s.count, s[i] == "+" || s[i] == "-" { i += 1 }
        var mantissaDigits = 0
        while digit(i) { i += 1; mantissaDigits += 1 }
        if i < s.count, s[i] == "." { i += 1; while digit(i) { i += 1; mantissaDigits += 1 } }
        guard mantissaDigits > 0 else { return nil }
        if i < s.count, s[i] == "e" || s[i] == "E" {
            i += 1
            if i < s.count, s[i] == "+" || s[i] == "-" { i += 1 }
            guard digit(i) else { return nil }
            while digit(i) { i += 1 }
        }
        guard i == s.count, let parsed = Double(String(String.UnicodeScalarView(s))), parsed.isFinite else { return nil }
        return parsed
    }

    /// bool.TryParse: "True" / "False" in any case, white space and NULs around it ignored.
    static func parseBool(_ value: String) -> Bool? {
        var s = Array(value.unicodeScalars)
        while let f = s.first, f.properties.isWhitespace || f.value == 0 { s.removeFirst() }
        while let l = s.last, l.properties.isWhitespace || l.value == 0 { s.removeLast() }
        switch String(String.UnicodeScalarView(s)).lowercased() {
        case "true": return true
        case "false": return false
        default: return nil
        }
    }

    /// string.Trim(): char.IsWhiteSpace at both ends.
    static func trim(_ s: String) -> String {
        var scalars = Array(s.unicodeScalars)
        while let f = scalars.first, f.properties.isWhitespace { scalars.removeFirst() }
        while let l = scalars.last, l.properties.isWhitespace { scalars.removeLast() }
        return String(String.UnicodeScalarView(scalars))
    }

    static func isNullOrWhiteSpace(_ s: String) -> Bool { s.unicodeScalars.allSatisfy { $0.properties.isWhitespace } }

    // ── CSV (TextFieldParser) ──────────────────────────────────────────────

    static func readCsv(_ text: String) throws -> ([String], [[String]], [String]) {
        var parser = GraphicsTextFieldParser(text)
        guard let headings = try parser.readFields() else { throw GraphicsDataError(message: "CSV needs a header row.") }
        if !(1...maxColumns).contains(headings.count) { throw GraphicsDataError(message: "Use at most 32 data columns.") }
        var rows: [[String]] = []
        while !parser.endOfData {
            if rows.count == maxRows { throw GraphicsDataError(message: "Use at most 1,000 data rows.") }
            let row = try parser.readFields() ?? []
            if row.count != headings.count { throw GraphicsDataError(message: "Every CSV row must match the header.") }
            rows.append(row)
        }
        let kinds = headings.indices.map { i -> String in
            let values = rows.map { $0[i] }.filter { !$0.isEmpty }
            if values.isEmpty { return "text" }
            if values.allSatisfy(isNumber) { return "number" }
            return values.allSatisfy { parseBool($0) != nil } ? "boolean" : "text"
        }
        return (headings, rows, kinds)
    }

    // ── JSON (JsonDocument, MaxDepth 4) ────────────────────────────────────

    static func readJson(_ text: String) throws -> ([String], [[String]], [String]) {
        var scanner = GraphicsJSONScanner(text)
        let root = try scanner.document()
        guard case .array(let elements) = root, (1...maxRows).contains(elements.count) else {
            throw GraphicsDataError(message: "JSON needs an array of 1–1,000 row objects.")
        }
        var headings: [String]?
        var kinds: [String?] = []
        var rows: [[String]] = []
        for element in elements {
            guard case .object(let properties) = element else { throw GraphicsDataError(message: "Every JSON row must be an object.") }
            guard (1...maxColumns).contains(properties.count), Set(properties.map { $0.name.uppercased() }).count == properties.count else {
                throw GraphicsDataError(message: "Use distinct JSON columns and at most 32 per row.")
            }
            if headings == nil { headings = properties.map(\.name); kinds = Array(repeating: nil, count: properties.count) }
            let names = headings!
            guard properties.count == names.count else { throw GraphicsDataError(message: "Every JSON row must have the same columns.") }
            var cells = Array(repeating: "", count: names.count)
            for property in properties {
                guard let index = names.firstIndex(of: property.name) else { throw GraphicsDataError(message: "Every JSON row must have the same columns.") }
                let value: String, kind: String?
                switch property.value {
                case .null: (value, kind) = ("", nil)
                case .string(let s): (value, kind) = (s, "text")
                case .number(let raw) where isNumber(raw): (value, kind) = (raw, "number")
                case .bool(let raw): (value, kind) = (raw, "boolean")
                default: throw GraphicsDataError(message: "JSON cells must be text, numbers, booleans or null.")
                }
                if let kind, let existing = kinds[index], existing != kind {
                    throw GraphicsDataError(message: "Each JSON column must have a consistent type.")
                }
                if kinds[index] == nil { kinds[index] = kind }
                cells[index] = value
            }
            rows.append(cells)
        }
        return (headings!, rows, kinds.map { $0 ?? "text" })
    }
}

/// TextFieldParser (HasFieldsEnclosedInQuotes, delimiter ",", TrimWhiteSpace) over physical lines that
/// keep their terminators, as Microsoft.VisualBasic.FileIO reads them.
struct GraphicsTextFieldParser {
    /// TextFieldParser's white-space characters (begin-quote and after-closing-quote checks).
    static let spaces: Set<UInt32> = [0x09, 0x0B, 0x0C, 0x20, 0x85, 0xA0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
                                      0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x200B, 0x2028, 0x2029, 0x3000, 0xFEFF]
    private var lines: [[Unicode.Scalar]] = []
    private var next = 0

    init(_ text: String) {
        var current: [Unicode.Scalar] = []
        let scalars = Array(text.unicodeScalars)
        var i = 0
        while i < scalars.count {
            let c = scalars[i]
            current.append(c)
            if c == "\r" || c == "\n" {
                if c == "\r", i + 1 < scalars.count, scalars[i + 1] == "\n" { current.append("\n"); i += 1 }
                lines.append(current); current = []
            }
            i += 1
        }
        if !current.isEmpty { lines.append(current) }
    }

    private static func ignorable(_ line: [Unicode.Scalar]) -> Bool { line.allSatisfy { $0.properties.isWhitespace } }

    /// The next line that is not blank (blank lines are skipped everywhere, even inside a quoted field).
    private mutating func nextDataLine() -> [Unicode.Scalar]? {
        while next < lines.count {
            let line = lines[next]; next += 1
            if !Self.ignorable(line) { return line }
        }
        return nil
    }

    var endOfData: Bool { !lines[next...].contains { !Self.ignorable($0) } }

    private static func endOfLineIndex(_ line: [Unicode.Scalar]) -> Int {
        let n = line.count
        if n == 1 { return n }
        if line[n - 2] == "\r" || line[n - 2] == "\n" { return n - 2 }
        if line[n - 1] == "\r" || line[n - 1] == "\n" { return n - 1 }
        return n
    }

    private static func trimField(_ scalars: ArraySlice<Unicode.Scalar>) -> String {
        GraphicsLocalData.trim(String(String.UnicodeScalarView(scalars)))
    }

    mutating func readFields() throws -> [String]? {
        guard var line = nextDataLine() else { return nil }
        var fields: [String] = []
        var index = 0
        var lineEnd = Self.endOfLineIndex(line)
        while index <= lineEnd {
            // BeginQuotesRegex: \G[white space]*"
            var probe = index
            while probe < line.count, Self.spaces.contains(line[probe].value) { probe += 1 }
            if probe < line.count, line[probe] == "\"" {
                var builder = QuotedField()
                builder.build(line, from: probe + 1)
                if builder.malformed { throw GraphicsDataError.malformed }
                while !builder.finished {
                    let end = line.count
                    guard let more = nextDataLine() else { throw GraphicsDataError.malformed }
                    line += more
                    lineEnd = Self.endOfLineIndex(line)
                    builder.build(line, from: end)
                    if builder.malformed { throw GraphicsDataError.malformed }
                }
                fields.append(GraphicsLocalData.trim(String(String.UnicodeScalarView(builder.field))))
                index = builder.index + builder.delimiterLength
            } else if let comma = line[index...].firstIndex(of: ",") {
                fields.append(Self.trimField(line[index..<comma]))
                index = comma + 1
            } else {
                var rest = Array(line[min(index, line.count)...])
                while let l = rest.last, l == "\r" || l == "\n" { rest.removeLast() }
                fields.append(Self.trimField(rest[...]))
                break
            }
        }
        return fields
    }

    /// QuoteDelimitedFieldBuilder: "" is a quote; after the closing quote only white space may precede
    /// the delimiter or the line end.
    struct QuotedField {
        var field: [Unicode.Scalar] = []
        var index = 0
        var delimiterLength = 0
        var finished = false
        var malformed = false

        mutating func build(_ line: [Unicode.Scalar], from start: Int) {
            index = start
            let length = line.count
            while index < length {
                if line[index] == "\"" {
                    if index + 1 == length { finished = true; delimiterLength = 1; index += 1; return }
                    if line[index + 1] == "\"" { field.append("\""); index += 2; continue }
                    // the delimiter-with-end-chars regex: , | \r\n | \r | \n
                    var match: (at: Int, length: Int)?
                    var k = index + 1
                    while k < length {
                        let c = line[k]
                        if c == "," || c == "\n" { match = (k, 1); break }
                        if c == "\r" { match = (k, k + 1 < length && line[k + 1] == "\n" ? 2 : 1); break }
                        k += 1
                    }
                    // Measured against TextFieldParser (data vectors): with no delimiter the scan runs to the
                    // line's end, and the next field starts after the delimiter. On the LAST line (no
                    // terminator) trailing white space therefore leaves one more, empty, field.
                    let end = match?.at ?? length
                    for i in (index + 1)..<max(index + 1, end) where !GraphicsTextFieldParser.spaces.contains(line[i].value) {
                        malformed = true; return
                    }
                    delimiterLength = end - index + (match?.length ?? 0)
                    finished = true
                    return
                }
                field.append(line[index]); index += 1
            }
        }
    }
}

/// A strict RFC 8259 scanner with System.Text.Json's defaults (no comments, no trailing commas,
/// JsonDocumentOptions.MaxDepth 4). Numbers keep their raw text; object properties keep their order
/// (duplicates are kept for the caller to refuse).
struct GraphicsJSONScanner {
    indirect enum Value {
        case object([(name: String, value: Value)])
        case array([Value])
        case string(String)
        case number(String)
        case bool(String)
        case null
    }

    static let maxDepth = 4
    private let s: [Unicode.Scalar]
    private var i = 0

    init(_ text: String) { s = Array(text.unicodeScalars) }

    mutating func document() throws -> Value {
        skip()
        let value = try parseValue(depth: 0)
        skip()
        guard i == s.count else { throw GraphicsDataError.malformed }
        return value
    }

    private mutating func skip() { while i < s.count, s[i] == " " || s[i] == "\t" || s[i] == "\n" || s[i] == "\r" { i += 1 } }

    private mutating func parseValue(depth: Int) throws -> Value {
        guard i < s.count else { throw GraphicsDataError.malformed }
        switch s[i] {
        case "{":
            guard depth < Self.maxDepth else { throw GraphicsDataError.malformed }
            i += 1; skip()
            var properties: [(name: String, value: Value)] = []
            if i < s.count, s[i] == "}" { i += 1; return .object(properties) }
            while true {
                skip()
                guard i < s.count, s[i] == "\"" else { throw GraphicsDataError.malformed }
                let name = try parseString()
                skip()
                guard i < s.count, s[i] == ":" else { throw GraphicsDataError.malformed }
                i += 1; skip()
                properties.append((name, try parseValue(depth: depth + 1)))
                skip()
                guard i < s.count else { throw GraphicsDataError.malformed }
                if s[i] == "," { i += 1; continue }
                if s[i] == "}" { i += 1; return .object(properties) }
                throw GraphicsDataError.malformed
            }
        case "[":
            guard depth < Self.maxDepth else { throw GraphicsDataError.malformed }
            i += 1; skip()
            var items: [Value] = []
            if i < s.count, s[i] == "]" { i += 1; return .array(items) }
            while true {
                skip()
                items.append(try parseValue(depth: depth + 1))
                skip()
                guard i < s.count else { throw GraphicsDataError.malformed }
                if s[i] == "," { i += 1; continue }
                if s[i] == "]" { i += 1; return .array(items) }
                throw GraphicsDataError.malformed
            }
        case "\"": return .string(try parseString())
        case "t": try literal("true"); return .bool("true")
        case "f": try literal("false"); return .bool("false")
        case "n": try literal("null"); return .null
        default: return .number(try parseNumber())
        }
    }

    private mutating func literal(_ word: String) throws {
        for c in word.unicodeScalars {
            guard i < s.count, s[i] == c else { throw GraphicsDataError.malformed }
            i += 1
        }
    }

    private func isDigit(_ k: Int) -> Bool { k < s.count && s[k].value >= 48 && s[k].value <= 57 }

    private mutating func parseNumber() throws -> String {
        let start = i
        if i < s.count, s[i] == "-" { i += 1 }
        guard isDigit(i) else { throw GraphicsDataError.malformed }
        if s[i] == "0" { i += 1 } else { while isDigit(i) { i += 1 } }
        if i < s.count, s[i] == "." {
            i += 1
            guard isDigit(i) else { throw GraphicsDataError.malformed }
            while isDigit(i) { i += 1 }
        }
        if i < s.count, s[i] == "e" || s[i] == "E" {
            i += 1
            if i < s.count, s[i] == "+" || s[i] == "-" { i += 1 }
            guard isDigit(i) else { throw GraphicsDataError.malformed }
            while isDigit(i) { i += 1 }
        }
        return String(String.UnicodeScalarView(s[start..<i]))
    }

    private mutating func hex4() throws -> UInt32 {
        guard i + 4 <= s.count else { throw GraphicsDataError.malformed }
        var v: UInt32 = 0
        for _ in 0..<4 {
            let c = s[i].value
            let d: UInt32
            switch c {
            case 48...57: d = c - 48
            case 65...70: d = c - 55
            case 97...102: d = c - 87
            default: throw GraphicsDataError.malformed
            }
            v = v * 16 + d; i += 1
        }
        return v
    }

    private mutating func parseString() throws -> String {
        i += 1   // opening quote
        var out = String.UnicodeScalarView()
        while true {
            guard i < s.count else { throw GraphicsDataError.malformed }
            let c = s[i]
            if c == "\"" { i += 1; return String(out) }
            if c.value < 0x20 { throw GraphicsDataError.malformed }
            if c != "\\" { out.append(c); i += 1; continue }
            i += 1
            guard i < s.count else { throw GraphicsDataError.malformed }
            let e = s[i]; i += 1
            switch e {
            case "\"": out.append("\"")
            case "\\": out.append("\\")
            case "/": out.append("/")
            case "b": out.append("\u{08}")
            case "f": out.append("\u{0C}")
            case "n": out.append("\n")
            case "r": out.append("\r")
            case "t": out.append("\t")
            case "u":
                var code = try hex4()
                if (0xD800...0xDBFF).contains(code) {
                    // A high surrogate must pair with an escaped low surrogate (a lone one cannot be text).
                    guard i + 1 < s.count, s[i] == "\\", s[i + 1] == "u" else { throw GraphicsDataError.malformed }
                    i += 2
                    let low = try hex4()
                    guard (0xDC00...0xDFFF).contains(low) else { throw GraphicsDataError.malformed }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
                } else if (0xDC00...0xDFFF).contains(code) {
                    throw GraphicsDataError.malformed
                }
                guard let scalar = Unicode.Scalar(code) else { throw GraphicsDataError.malformed }
                out.append(scalar)
            default: throw GraphicsDataError.malformed
            }
        }
    }
}
