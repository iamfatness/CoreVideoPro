// GraphicsDataTable — data table and crawl fields, manifests and bindings (G4-G7 Task 6).
// Ports of MC/Services/GraphicsDataTable.cs (GraphicsDataBinding, Fields, Manifest, Binding),
// GraphicsCrawlData.cs (GraphicsCrawlOptions, Items, Fields, Manifest) and
// GraphicsDesignSeeds.Manifest("title", …). Row cells are encoded as System.Text.Json does with
// JavaScriptEncoder.UnsafeRelaxedJsonEscaping (GraphicsRelaxedJSON). Pinned by
// contracts/graphics-shell/data-table.vectors.json.

import Foundation

struct GraphicsCrawlOptions: Equatable {
    var speed = 120.0, gap = 48.0, direction = "left", mode = "loop"
    var isValid: Bool { (1...1200).contains(speed) && (0...512).contains(gap) && ["left", "right"].contains(direction) && ["loop", "single"].contains(mode) }
    var json: JSONObject { ["speed": speed, "gap": gap, "direction": direction, "mode": mode] }
    init(speed: Double = 120, gap: Double = 48, direction: String = "left", mode: String = "loop") {
        self.speed = speed; self.gap = gap; self.direction = direction; self.mode = mode
    }
    init?(json: JSONObject) {
        func num(_ key: String, _ fallback: Double) -> Double? {
            guard let v = GraphicsDataBinding.value(json, key) else { return fallback }
            return (v as? NSNumber).flatMap { CFGetTypeID($0) == CFBooleanGetTypeID() ? nil : $0.doubleValue }
        }
        func str(_ key: String, _ fallback: String) -> String? {
            guard let v = GraphicsDataBinding.value(json, key) else { return fallback }
            return v as? String
        }
        guard let speed = num("speed", 120), let gap = num("gap", 48), let direction = str("direction", "left"), let mode = str("mode", "loop") else { return nil }
        self.init(speed: speed, gap: gap, direction: direction, mode: mode)
    }
}

/// A playlist row's data binding (GraphicsDataBinding; Web JSON names, read case-insensitively).
struct GraphicsDataBinding: Equatable {
    var sourceId: String
    var keyColumn: String?
    var columns: [String]
    var sortColumn: String?
    var limit: Int
    var revision: Int64
    var crawl: GraphicsCrawlOptions?
    var liveFollow = false
    var remote = false

    var isValid: Bool {
        let lengthOK: (String?) -> Bool = { $0 == nil || (1...128).contains($0!.utf16.count) }
        guard GraphicsRow.validId(sourceId), (1...GraphicsLocalData.maxVisibleColumns).contains(columns.count),
              columns.allSatisfy({ (1...128).contains($0.utf16.count) }), Set(columns).count == columns.count,
              lengthOK(keyColumn), lengthOK(sortColumn), sortColumn == nil || columns.count < GraphicsLocalData.maxVisibleColumns
        else { return false }
        if let crawl {
            guard crawl.isValid, columns.count == 1, sortColumn == nil, (1...GraphicsCrawlData.maxItems).contains(limit) else { return false }
        } else {
            guard (1...GraphicsLocalData.maxVisibleRows).contains(limit), !liveFollow || remote else { return false }
        }
        return (!remote || keyColumn != nil) && revision > 0
    }

    var json: JSONObject {
        ["sourceId": sourceId, "keyColumn": keyColumn ?? NSNull(), "columns": columns, "sortColumn": sortColumn ?? NSNull(),
         "limit": limit, "revision": revision, "crawl": crawl?.json ?? NSNull(), "liveFollow": liveFollow, "remote": remote]
    }

    init(sourceId: String, keyColumn: String?, columns: [String], sortColumn: String?, limit: Int, revision: Int64,
         crawl: GraphicsCrawlOptions? = nil, liveFollow: Bool = false, remote: Bool = false) {
        self.sourceId = sourceId; self.keyColumn = keyColumn; self.columns = columns; self.sortColumn = sortColumn
        self.limit = limit; self.revision = revision; self.crawl = crawl; self.liveFollow = liveFollow; self.remote = remote
    }

    /// System.Text.Json Web defaults read property names case-insensitively.
    static func value(_ json: JSONObject, _ key: String) -> Any? {
        let hit = json[key] ?? json.first { $0.key.lowercased() == key.lowercased() }?.value
        return hit is NSNull ? nil : hit
    }

    init?(json: JSONObject) {
        func optionalString(_ key: String) -> String?? {
            guard let v = Self.value(json, key) else { return .some(nil) }
            guard let s = v as? String else { return nil }
            return .some(s)
        }
        func bool(_ key: String) -> Bool? {
            guard let v = Self.value(json, key) else { return false }
            guard let n = v as? NSNumber, CFGetTypeID(n) == CFBooleanGetTypeID() else { return nil }
            return n.boolValue
        }
        guard let source = optionalString("sourceId"), let key = optionalString("keyColumn"), let sort = optionalString("sortColumn"),
              let live = bool("liveFollow"), let remote = bool("remote") else { return nil }
        let columns = Self.value(json, "columns") as? [String] ?? []
        let limit = (Self.value(json, "limit") as? NSNumber)?.intValue ?? 0
        let revision = (Self.value(json, "revision") as? NSNumber)?.int64Value ?? 0
        var crawl: GraphicsCrawlOptions?
        if let c = Self.value(json, "crawl") {
            guard let object = c as? JSONObject, let options = GraphicsCrawlOptions(json: object) else { return nil }
            crawl = options
        }
        self.init(sourceId: source ?? "", keyColumn: key, columns: columns, sortColumn: sort, limit: limit, revision: revision,
                  crawl: crawl, liveFollow: live, remote: remote)
    }
}

enum GraphicsDataTable {
    static let rowPrefix = "table-row-", revisionField = "data-revision"

    static func isDataField(_ id: String) -> Bool { GraphicsRowFields.isDataField(id) }
    static func fieldId(_ index: Int) -> String { rowPrefix + String(index + 1) }

    /// The template field values a binding produces, in Windows' insertion order.
    static func orderedFields(_ data: GraphicsLocalData, _ binding: GraphicsDataBinding) throws -> [(String, String)] {
        guard binding.isValid else { throw GraphicsDataError(message: "Choose valid columns, row limit and leaderboard sort.") }
        if binding.crawl != nil { return try GraphicsCrawlData.orderedFields(data, binding) }
        let rows = try data.project(columns: binding.columns, orderBy: binding.sortColumn, limit: binding.limit)
        var fields: [(String, String)] = []
        for index in 0..<binding.limit {
            var encoded = ""
            if index < rows.count {
                var row = rows[index]
                if binding.sortColumn != nil { row.insert(String(index + 1), at: 0) }
                encoded = GraphicsRelaxedJSON.stringArray(row)
            }
            if encoded.utf8.count > 4096 { throw GraphicsDataError(message: "A displayed row is too long. Shorten its values or select fewer columns.") }
            fields.append((fieldId(index), encoded))
        }
        if binding.remote { fields.append((revisionField, String(binding.revision))) }
        return fields
    }

    static func fields(_ data: GraphicsLocalData, _ binding: GraphicsDataBinding) throws -> JSONObject {
        Dictionary(try orderedFields(data, binding), uniquingKeysWith: { $1 })
    }

    static func manifest(templateId: String, name: String, binding: GraphicsDataBinding, data: GraphicsLocalData) throws -> JSONObject {
        if binding.crawl != nil {
            guard binding.isValid else { throw GraphicsDataError(message: "Choose valid crawl settings and one text column.") }
            var crawl = try GraphicsCrawlData.manifest(templateId: templateId, name: name, binding: binding, data: data)
            crawl["metadata"] = ["dataBinding": binding.json]
            return crawl
        }
        let fields = try orderedFields(data, binding)
        var seed = GraphicsDesignSeeds.manifest(kind: "title", templateId: templateId, templateVersion: 1, name: name) ?? [:]
        let height = (binding.limit + 1) * 56
        seed["bounds"] = ["width": 1440, "height": height]
        seed["fields"] = fields.map { ["id": $0.0, "type": "string", "maxLength": 4096, "default": $0.1] as JSONObject }
        let sorted = binding.sortColumn != nil
        let columns = sorted ? ["Rank"] + binding.columns : binding.columns
        seed["layers"] = [[
            "id": "data-table", "type": "table", "x": 0, "y": 0, "width": 1440, "height": height, "rowHeight": 56, "fontSize": 29,
            "font": ["token": "brand.fontBody"], "color": ["token": "brand.onPrimary"], "headerFill": ["token": "brand.primary"],
            "rowFill": "#18232D", "alternateFill": "#243440",
            "columns": columns.enumerated().map { index, column -> JSONObject in
                ["heading": column, "weight": sorted && index == 0 ? 0.4 : 1,
                 "align": column == binding.sortColumn || (sorted && index == 0) ? "right" : "left"]
            },
            "rows": fields.map(\.0).filter { $0.hasPrefix(rowPrefix) }.map { ["field": $0] as JSONObject },
            "metadata": ["label": "Data table"],
        ] as JSONObject]
        seed["metadata"] = ["dataBinding": binding.json]
        return seed
    }

    /// The valid binding a data design's manifest carries in metadata.dataBinding, else nil.
    static func binding(fromDefinition definition: JSONObject) -> GraphicsDataBinding? {
        guard let metadata = definition["metadata"] as? JSONObject, let data = metadata["dataBinding"] as? JSONObject,
              let binding = GraphicsDataBinding(json: data), binding.isValid else { return nil }
        return binding
    }
}

enum GraphicsCrawlData {
    static let maxItems = 64, itemsField = "crawl-items", revisionField = "crawl-revision"

    /// A text column's trimmed, non-empty cells, filtered THEN limited (Items).
    static func items(_ data: GraphicsLocalData, column: String, limit: Int) throws -> [String] {
        guard let index = data.columns.firstIndex(where: { $0.id == column && $0.kind == "text" }) else {
            throw GraphicsDataError(message: "Choose a text column for the crawl.")
        }
        return Array(data.rows.map { GraphicsLocalData.trim($0.cells[index]) }.filter { !$0.isEmpty }.prefix(max(0, limit)))
    }

    static func orderedFields(_ data: GraphicsLocalData, _ binding: GraphicsDataBinding) throws -> [(String, String)] {
        let items = try items(data, column: binding.columns[0], limit: binding.limit)
        if items.isEmpty || items.contains(where: { $0.unicodeScalars.contains { ["\r", "\n", "\t", "\0"].contains($0) } }) {
            throw GraphicsDataError(message: "The crawl needs nonempty, single-line text.")
        }
        let encoded = GraphicsRelaxedJSON.stringArray(items)
        if encoded.utf8.count > 4096 { throw GraphicsDataError(message: "The crawl text is too long. Shorten its items or choose a smaller limit.") }
        return [(itemsField, encoded), (revisionField, String(binding.revision))]
    }

    static func manifest(templateId: String, name: String, binding: GraphicsDataBinding, data: GraphicsLocalData) throws -> JSONObject {
        let options = binding.crawl ?? GraphicsCrawlOptions()
        let fields = try orderedFields(data, binding)
        var seed = GraphicsDesignSeeds.manifest(kind: "title", templateId: templateId, templateVersion: 1, name: name) ?? [:]
        seed["bounds"] = ["width": 1440, "height": 64]
        seed["fields"] = fields.map { ["id": $0.0, "type": "string", "maxLength": 4096, "default": $0.1] as JSONObject }
        seed["layers"] = [[
            "id": "data-crawl", "type": "crawl", "x": 0, "y": 0, "width": 1440, "height": 64, "fontSize": 34,
            "font": ["token": "brand.fontBody"], "color": ["token": "brand.onPrimary"], "items": ["field": itemsField],
            "speed": options.speed, "gap": options.gap, "direction": options.direction, "mode": options.mode,
            "metadata": ["label": "Data crawl"],
        ] as JSONObject]
        return seed
    }
}

/// GraphicsDesignSeeds: the blank starting points; only "title" is needed before the Builder (G8).
enum GraphicsDesignSeeds {
    /// contracts/graphics-seeds/title.json, compacted (the file Windows embeds; a test compares them).
    static let titleSeedText = #"{"schemaVersion":1,"templateId":"design-title-000000","templateVersion":1,"name":"Title","family":"title","canvas":{"width":1920,"height":1080},"bounds":{"width":1440,"height":288},"brandTokens":["brand.primary","brand.onPrimary","brand.accent","brand.fontBody"],"fields":[{"id":"heading","type":"string","default":"Heading","required":true,"maxLength":128},{"id":"subheading","type":"string","default":"Subheading","maxLength":256}],"layers":[{"id":"plate","metadata":{"label":"Plate"},"type":"rect","x":0,"y":0,"width":1440,"height":288,"fill":{"token":"brand.primary"}},{"id":"heading-label","metadata":{"label":"Heading"},"type":"text","text":{"field":"heading"},"x":48,"y":52,"width":1344,"height":100,"font":{"token":"brand.fontBody"},"fontWeight":600,"fontSize":64,"lineHeight":1.2,"color":{"token":"brand.onPrimary"},"align":"center","maxLines":1,"overflow":"ellipsis"},{"id":"subheading-label","metadata":{"label":"Subheading"},"type":"text","text":{"field":"subheading"},"x":48,"y":174,"width":1344,"height":58,"font":{"token":"brand.fontBody"},"fontWeight":600,"fontSize":32,"lineHeight":1.2,"color":{"token":"brand.onPrimary"},"align":"center","maxLines":1,"overflow":"ellipsis","collapseWhenEmpty":true}],"animations":{"in":{"type":"fade","durationMs":250,"easing":"ease-out"},"out":{"type":"fade","durationMs":200,"easing":"ease-in"}},"defaultPlacement":{"anchor":"center","x":0,"y":0,"scale":1}}"#
    static var titleSeed: JSONObject {
        (try? JSONSerialization.jsonObject(with: Data(titleSeedText.utf8))) as? JSONObject ?? [:]
    }

    static func manifest(kind: String, templateId: String, templateVersion: Int, name: String) -> JSONObject? {
        guard kind == "title" else { return nil }
        var manifest = titleSeed
        manifest["templateId"] = templateId
        manifest["templateVersion"] = templateVersion
        manifest["name"] = name
        return manifest
    }
}

/// System.Text.Json string escaping with JavaScriptEncoder.UnsafeRelaxedJsonEscaping, measured
/// against the real encoder (data-table vectors): quote and backslash and \b \t \n \f \r get short
/// escapes; other C0 controls, U+007F-U+009F, non-space separators (U+00A0, U+2028, U+2029, …),
/// private use, unassigned code points, U+FEFF and every supplementary-plane character (as a
/// surrogate pair) get \uXXXX with upper-case hex; everything else is written as is.
enum GraphicsRelaxedJSON {
    static func stringArray(_ values: [String]) -> String { "[" + values.map(string).joined(separator: ",") + "]" }

    static func string(_ value: String) -> String {
        var out = "\""
        func hex(_ unit: UInt32) { out += String(format: "\\u%04X", unit) }
        for scalar in value.unicodeScalars {
            switch scalar {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\u{08}": out += "\\b"
            case "\t": out += "\\t"
            case "\n": out += "\\n"
            case "\u{0C}": out += "\\f"
            case "\r": out += "\\r"
            default:
                let v = scalar.value
                if v > 0xFFFF {
                    let u = v - 0x10000
                    hex(0xD800 + (u >> 10)); hex(0xDC00 + (u & 0x3FF))
                } else if escaped(scalar) {
                    hex(v)
                } else {
                    out.unicodeScalars.append(scalar)
                }
            }
        }
        return out + "\""
    }

    static func escaped(_ scalar: Unicode.Scalar) -> Bool {
        if scalar.value < 0x20 || (0x7F...0x9F).contains(scalar.value) || scalar.value == 0xFEFF { return true }
        switch scalar.properties.generalCategory {
        case .control, .privateUse, .unassigned, .surrogate, .lineSeparator, .paragraphSeparator: return true
        case .spaceSeparator: return scalar != " "
        default: return false
        }
    }
}
