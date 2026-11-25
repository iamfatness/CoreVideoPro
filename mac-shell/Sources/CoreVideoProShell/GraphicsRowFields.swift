// GraphicsRowFields — which of a row's template variables the Operate inspector
// types (G4-G7 Task 1). Port of MC/Services/GraphicsRowFields.cs:20-39,78-81,
// GraphicsMappedValues.Eligible (:10-11), GraphicsDataTable.IsDataField (:34-35),
// GraphicsTemplateEditor.HumanizeId (:194-198) and the WinUI control's private
// TitleFieldKey (W/Controls/GraphicsPlayoutControl.cs:165-166). Pinned by
// contracts/graphics-shell/rowfields.vectors.json.

import Foundation

/// One text variable an operator types on an Operate playlist row (GraphicsRowTypeableField).
struct GraphicsTypeableField: Equatable {
    let id: String
    let label: String
    let maxLength: Int?
}

enum GraphicsRowFields {
    /// Keys the inspector already handles (row name, Title line / Sources) or the template fills itself (clock, LIVE label).
    static let handled = ["name", "title", "time", "label"]
    /// The core's default cap for a string variable with no maxLength (native GraphicsTemplate.cpp; Windows CoreStringLimit).
    static let coreStringLimit = 512
    /// The Title line box's MaxLength (GraphicsPlayoutControl.cs RefreshInspector).
    static let titleLineLimit = 512

    /// A Guest row's Title line is `title`; any other row's is the first of title / subtitle / label its fields carry.
    static func titleFieldKey(_ row: GraphicsRow) -> String? {
        if row.kind == GraphicsRow.guest { return "title" }
        let fields = row.fields
        return ["title", "subtitle", "label"].first { fields[$0] != nil }
    }

    /// The declarative variables of the row's exact template pin: the installed template when the
    /// core has it, else the saved pin's manifest. A compatibility template or an empty row has none.
    static func templateFields(_ row: GraphicsRow, state: GraphicsState?, pins: [GraphicsTemplatePin]) -> [GraphicsTemplateField] {
        let image = row.image
        let id = image.map { $0["templateId"] as? String ?? "" } ?? row.templateId
        let version = image.map { ($0["templateVersion"] as? NSNumber)?.intValue ?? 0 } ?? row.templateVersion
        guard !id.isEmpty else { return [] }
        if let installed = state?.templates.first(where: { $0.templateId == id && $0.templateVersion == version }) {
            return installed.runtime == "declarative" ? installed.fields : []
        }
        guard let pin = pins.first(where: { $0.templateId == id && $0.templateVersion == version }),
              pin.definition["layers"] != nil else { return [] }
        return GraphicsState.fields(pin.definition["fields"])
    }

    /// Every free-text variable other than `handled` and the row's own Title line (`titleKey`).
    /// Variables with a fixed choice list are not typed; neither are a data row's data fields.
    static func typeable(_ row: GraphicsRow, state: GraphicsState?, pins: [GraphicsTemplatePin], titleKey: String?) -> [GraphicsTypeableField] {
        let dataRow = row.dataBinding != nil
        return templateFields(row, state: state, pins: pins)
            .filter { $0.type == "string" && $0.choices.isEmpty && !handled.contains($0.id) && $0.id != titleKey && (!dataRow || !isDataField($0.id)) }
            .map { GraphicsTypeableField(id: $0.id, label: label(of: $0), maxLength: $0.maxLength) }
    }

    /// GraphicsMappedValues.Eligible: a variable a names-list cell or a Sources column may fill.
    static func eligibleForMapping(_ field: GraphicsTemplateField) -> Bool {
        !handled.contains(field.id) && ["string", "number", "color", "boolean"].contains(field.type)
    }

    /// GraphicsDataTable.IsDataField: a field the data lifecycle writes, never the operator.
    static func isDataField(_ id: String) -> Bool {
        id.hasPrefix("table-row-") || id == "crawl-items" || id == "crawl-revision" || id == "data-revision"
    }

    static func label(of field: GraphicsTemplateField) -> String {
        if let friendly = field.label, (1...128).contains(friendly.utf16.count) { return friendly }
        return humanize(field.id)
    }

    /// GraphicsTemplateEditor.HumanizeId: dashes and underscores become spaces, first letter capitalised.
    static func humanize(_ id: String) -> String {
        let text = id.replacingOccurrences(of: "-", with: " ").replacingOccurrences(of: "_", with: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard let first = text.first else { return id }
        return first.uppercased() + text.dropFirst()
    }

    /// The row's text for `key` (GraphicsRowFields.Text): a string value, else "".
    static func text(_ row: GraphicsRow, _ key: String) -> String { row.fields[key] as? String ?? "" }

    /// `text` cut to at most `limit` UTF-16 units, as C# `s[..limit]` (a WinUI TextBox MaxLength): a
    /// combining mark past the cut is dropped exactly as on Windows. A surrogate pair split by the cut
    /// becomes U+FFFD (C# would keep a lone surrogate, which no JSON writer accepts either).
    static func clamp(_ text: String, _ limit: Int) -> String {
        let units = text.utf16
        guard units.count > limit else { return text }
        return String(decoding: Array(units.prefix(limit)), as: UTF16.self)
    }

    // ── Shot Layout overrides (GraphicsRowFields.cs:41-47) ──────────────────

    /// A field a layout may override: not the clock or LIVE label, not a data field, and a scalar type.
    static func canOverrideLayoutField(_ field: GraphicsTemplateField) -> Bool {
        field.id != "time" && field.id != "label" && !isDataField(field.id) && ["string", "number", "boolean", "color"].contains(field.type)
    }

    static func validLayoutOverride(_ field: GraphicsTemplateField, _ value: Any) -> Bool {
        guard canOverrideLayoutField(field), validField(value, field) else { return false }
        return field.type != "color" || ((value as? String)?.utf8.count ?? 0) <= (field.maxLength ?? 128)
    }

    /// GraphicsCommandRequest.ValidField: choices, then the type's own rule.
    static func validField(_ value: Any, _ field: GraphicsTemplateField) -> Bool {
        if !field.choices.isEmpty && !field.choices.contains(where: { ($0 as AnyObject).isEqual(value) }) { return false }
        let isBool = (value as? NSNumber).map { CFGetTypeID($0) == CFBooleanGetTypeID() } ?? false
        switch field.type {
        case "string": return (value as? String).map { $0.utf8.count <= (field.maxLength ?? 512) } ?? false
        case "color":
            guard let c = value as? String, c.utf16.count == 7 || c.utf16.count == 9, c.hasPrefix("#") else { return false }
            return c.dropFirst().allSatisfy(\.isHexDigit) && c.dropFirst().allSatisfy(\.isASCII)
        case "image": return (value as? String).map { $0.isEmpty || GraphicsCommandRequest.validId($0) } ?? false
        case "number":
            guard let n = value as? NSNumber, !isBool else { return false }
            let d = n.doubleValue
            return d.isFinite && d >= (field.minimum ?? -1e9) && d <= (field.maximum ?? 1e9)
        case "boolean": return isBool
        default: return false
        }
    }
}
