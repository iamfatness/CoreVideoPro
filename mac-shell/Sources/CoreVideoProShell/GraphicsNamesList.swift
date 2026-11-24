// GraphicsNamesList — the names-list editor's parser and writer (G4-G7 Task 2).
// Port of MC/Services/GraphicsBuilder.cs ParseNamesList / ClosingQuote / Unquote
// / NamesListText / NewRowId (:58-68,166-221) and GraphicsBuilder.NamesList.cs
// (NameListColumns, ReadNameListCells, WriteNameListCells). Pinned by
// contracts/graphics-shell/names-editor.vectors.json. Variants use the show
// document's JSON shape {id, name, title, cells}. Lengths count UTF-16 units as
// C# string.Length does. Line endings written are "\n" (plan D12).

import Foundation

enum GraphicsNamesList {
    static let maxLines = 64, maxName = 128, maxTitle = 256, maxCell = 4096, maxColumns = 32

    // ── parse ────────────────────────────────────────────────────────────────

    /// Names list text → variants. One line per name; the title follows the first comma, tab or
    /// semicolon. A first line "name,title" (any case) is a header and skipped; a header with more
    /// than two columns names variable columns (only `variableIds`, when given). `existing` keeps
    /// ids for names already in the list so an on-air line survives a re-save.
    static func parse(_ text: String, existing: [JSONObject], variableIds: [String]?) -> [JSONObject] {
        var result: [JSONObject] = []
        var used = Set<String>()
        let lines = text.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
        let columns = nameListColumns(lines, variableIds)
        var first = true
        for raw in lines {
            let line = trim(raw)
            if line.isEmpty { continue }
            if first && columns != nil { first = false; continue }
            var name: String, title: String
            var cells: JSONObject = [:]
            if let columns {
                guard let values = readCells(line) else { continue }
                name = values[0].value
                title = values.count > 1 ? values[1].value : ""
                var i = 2
                while i < min(columns.count, values.count) {
                    let cell = values[i]
                    if let key = columns[i], cell.quoted || !cell.value.isEmpty {
                        cells[key] = GraphicsRowFields.clamp(cell.value, maxCell)
                    }
                    i += 1
                }
            } else if line.first == "\"", let close = closingQuote(Array(line)), close > 0 {
                // A quoted first cell may itself contain the separator ("Lovelace, Ada", Analyst).
                let chars = Array(line)
                name = trim(String(chars[1..<close]).replacingOccurrences(of: "\"\"", with: "\""))
                var rest = String(chars[(close + 1)...])
                rest = String(rest.drop { isWhite($0) })
                if let c = rest.first, c == "," || c == "\t" || c == ";" { rest = String(rest.dropFirst()) }
                title = unquote(rest)
            } else if let cut = line.firstIndex(where: { $0 == "," || $0 == "\t" || $0 == ";" }) {
                name = unquote(String(line[..<cut]))
                title = unquote(String(line[line.index(after: cut)...]))
            } else {
                name = unquote(line); title = ""
            }
            if first && lowerInvariant(name) == "name" && lowerInvariant(title) == "title" { first = false; continue }
            first = false
            if name.isEmpty { continue }
            name = GraphicsRowFields.clamp(name, maxName)
            title = GraphicsRowFields.clamp(title, maxTitle)
            var id = existing.first { ($0["name"] as? String) == name && !used.contains($0["id"] as? String ?? "") }?["id"] as? String
                ?? newRowId(name, taken: [])
            let baseId = id
            var n = 2
            while used.contains(id) { id = "\(baseId)-\(n)"; n += 1 }
            used.insert(id)
            result.append(["id": id, "name": name, "title": title, "cells": cells])
            if result.count == maxLines { break }
        }
        return result
    }

    /// NameListColumns: the header's variable columns (index → id), or nil when the first
    /// non-blank line is not a "name,title,…" header with more than two cells.
    static func nameListColumns(_ lines: [String], _ variableIds: [String]?) -> [String?]? {
        guard let header = lines.first(where: { !trim($0).isEmpty }), let cells = readCells(trim(header)), cells.count > 2,
              lowerInvariant(cells[0].value) == "name", lowerInvariant(cells[1].value) == "title" else { return nil }
        let allowed = variableIds.map { Set($0.map { $0.lowercased() }) }
        var used: Set<String> = ["name", "title"]
        var columns = [String?](repeating: nil, count: cells.count)
        var i = 2
        while i < cells.count && used.count < maxColumns {
            let id = lowerInvariant(cells[i].value)
            if GraphicsRow.validVariableId(id) && (allowed == nil || allowed!.contains(id)) && !used.contains(id) {
                used.insert(id)
                columns[i] = id
            }
            i += 1
        }
        return columns
    }

    /// ReadNameListCells: comma-separated cells, a cell optionally quoted; nil when a quote is
    /// unbalanced or a quoted cell is followed by anything but a comma.
    static func readCells(_ input: String) -> [(value: String, quoted: Bool)]? {
        var cells: [(value: String, quoted: Bool)] = []
        var line = Array(input)
        while true {
            let cell = Array(line.drop { $0 == " " })
            if cell.first == "\"" {
                guard let close = closingQuote(cell) else { return nil }
                cells.append((unquote(String(cell[...close])), true))
                line = Array(cell[(close + 1)...].drop { $0 == " " })
                if line.isEmpty { return cells }
                if line[0] != "," { return nil }
            } else {
                guard let comma = line.firstIndex(of: ",") else {
                    cells.append((trim(String(line)), false))
                    return cells
                }
                cells.append((trim(String(line[..<comma])), false))
                line = Array(line[comma...])
            }
            line = Array(line.dropFirst())
        }
    }

    /// ClosingQuote: the index of the quote closing a cell that opens at index 0 ("" is an escape).
    static func closingQuote(_ line: [Character]) -> Int? {
        var i = 1
        while i < line.count {
            if line[i] != "\"" { i += 1; continue }
            if i + 1 < line.count && line[i + 1] == "\"" { i += 2; continue }
            return i
        }
        return nil
    }

    static func unquote(_ raw: String) -> String {
        let cell = trim(raw)
        let chars = Array(cell)
        guard chars.count >= 2, chars.first == "\"", chars.last == "\"" else { return cell }
        return trim(String(chars[1..<(chars.count - 1)]).replacingOccurrences(of: "\"\"", with: "\""))
    }

    // ── text ─────────────────────────────────────────────────────────────────

    /// Variants → the editor's text (round trip of `parse`).
    static func text(_ variants: [JSONObject]) -> String {
        var columns: [String] = []
        for variant in variants {
            for key in orderedCellKeys(variant) where !columns.contains(key) { columns.append(key) }
        }
        func quote(_ value: String) -> String { "\"" + value.replacingOccurrences(of: "\"", with: "\"\"") + "\"" }
        func escape(_ value: String) -> String {
            value.contains(where: { $0 == "," || $0 == ";" || $0 == "\t" || $0 == "\"" }) ? quote(value) : value
        }
        func name(_ v: JSONObject) -> String { v["name"] as? String ?? "" }
        func title(_ v: JSONObject) -> String { v["title"] as? String ?? "" }
        if columns.isEmpty {
            return variants.map { title($0).isEmpty ? escape(name($0)) : "\(escape(name($0))), \(escape(title($0)))" }.joined(separator: "\n")
        }
        var lines = ["name,title," + columns.joined(separator: ",")]
        for variant in variants {
            let cells = variant["cells"] as? JSONObject ?? [:]
            var values = [escape(name(variant)), escape(title(variant))]
            values += columns.map { (cells[$0] as? String).map(quote) ?? "" }
            lines.append(values.joined(separator: ","))
        }
        return lines.joined(separator: "\n")
    }

    /// A variant's cell keys. C# enumerates them in the order the document wrote them; JSONSerialization
    /// keeps no key order, so the Mac lists them ordinally (ruling: the editor's column order may differ
    /// from Windows for a Windows-authored list whose cells are not in ordinal order; the parsed cells
    /// are the same set either way).
    static func orderedCellKeys(_ variant: JSONObject) -> [String] {
        (variant["cells"] as? JSONObject ?? [:]).keys.sorted()
    }

    // ── ids ──────────────────────────────────────────────────────────────────

    /// NewRowId: lower-case a-z0-9 and dashes, no leading digits or dashes, at most 40 chars,
    /// "graphic" when nothing is left, unique among `taken` (-2, -3, …).
    static func newRowId(_ name: String, taken: [String]) -> String {
        let mapped = lowerInvariant(name).utf16.map { unit -> Character in
            (unit >= 0x61 && unit <= 0x7A) || (unit >= 0x30 && unit <= 0x39) ? Character(Unicode.Scalar(unit)!) : "-"
        }
        var slug = String(mapped).split(separator: "-", omittingEmptySubsequences: true).joined(separator: "-")
        slug = String(slug.drop { $0 == "-" || ("0"..."9").contains($0) })
        if slug.isEmpty { slug = "graphic" }
        if slug.count > 40 { slug = String(slug.prefix(40)); while slug.hasSuffix("-") { slug.removeLast() } }
        let used = Set(taken)
        if !used.contains(slug) { return slug }
        var n = 2
        while used.contains("\(slug)-\(n)") { n += 1 }
        return "\(slug)-\(n)"
    }

    // ── C# string helpers ────────────────────────────────────────────────────

    static func isWhite(_ c: Character) -> Bool { c.unicodeScalars.allSatisfy { CharacterSet.whitespacesAndNewlines.contains($0) } }
    /// string.Trim(): leading and trailing white space.
    static func trim(_ s: String) -> String { s.trimmingCharacters(in: .whitespacesAndNewlines) }
    /// ToLowerInvariant: .NET's simple (one-to-one) case mapping. A scalar whose full lowercase is one
    /// scalar maps to it (A-Z, and the Kelvin sign U+212A to k); one whose lowercase expands (U+0130
    /// dotted capital I becomes i + combining dot) is kept, as .NET keeps it. Pinned by names-editor vectors.
    static func lowerInvariant(_ s: String) -> String {
        String(String.UnicodeScalarView(s.unicodeScalars.map { scalar in
            let lower = scalar.properties.lowercaseMapping.unicodeScalars
            return lower.count == 1 ? lower.first! : scalar
        }))
    }
}
