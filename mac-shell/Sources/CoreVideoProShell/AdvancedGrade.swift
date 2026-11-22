// The advanced grade document, its reference, and .cvgrade presets (grading plan 2026-11-16,
// Task 9). Twins of the C# AdvancedGradeDocument / GradeOperation
// (native-shell/CoreVideoPro.MediaCore/Models/AdvancedGradeDocument.cs) and of the Windows
// editor's CopyGradeJson / PasteGradeJson, so a document or preset moves between platforms
// unchanged. contracts/grade.fixtures.json pins the verdicts all three platforms agree on.
//
// Decoding is tolerant like System.Text.Json (a missing key keeps the C# default; unknown keys
// such as a Windows preset's `summary` are ignored), EXCEPT an explicit JSON null for
// `operations`, an operation, `curves` or a curve point, which is refused (C# leaves null there
// and Validate refuses it). Encoding always writes every field, so the core's strict parser
// (AdvancedGrade.h) only ever receives complete documents.

import CryptoKit
import Foundation

struct GradeCurvePoint: Codable, Equatable { var x: Double; var y: Double }

struct GradeOperation: Codable, Equatable, Identifiable {
    var id = GradeOperation.newId()
    var kind = "primaries"      // primaries | curves | cube
    var enabled = true
    var intensity = 1.0
    var exposureStops = 0.0, contrast = 1.0, pivot = 0.5, saturation = 1.0
    var temperature = 0.0, tint = 0.0, lift = 0.0, gamma = 1.0, gain = 1.0
    var curves: [[GradeCurvePoint]] = GradeOperation.identityCurves
    var cubeText = "", cubeSha256 = "", name = ""

    static let identityCurves: [[GradeCurvePoint]] =
        Array(repeating: [GradeCurvePoint(x: 0, y: 0), GradeCurvePoint(x: 1, y: 1)], count: 4)
    /// C# Guid.ToString("N"): 32 lowercase hex digits, no dashes.
    static func newId() -> String { UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased() }
    /// C# DisplayName: the name, else the kind.
    var displayName: String {
        name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? kind : name
    }
}

extension GradeOperation {
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = GradeOperation()
        id = try c.decodeIfPresent(String.self, forKey: .id) ?? d.id
        kind = try c.decodeIfPresent(String.self, forKey: .kind) ?? d.kind
        enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? d.enabled
        intensity = try c.decodeIfPresent(Double.self, forKey: .intensity) ?? d.intensity
        exposureStops = try c.decodeIfPresent(Double.self, forKey: .exposureStops) ?? d.exposureStops
        contrast = try c.decodeIfPresent(Double.self, forKey: .contrast) ?? d.contrast
        pivot = try c.decodeIfPresent(Double.self, forKey: .pivot) ?? d.pivot
        saturation = try c.decodeIfPresent(Double.self, forKey: .saturation) ?? d.saturation
        temperature = try c.decodeIfPresent(Double.self, forKey: .temperature) ?? d.temperature
        tint = try c.decodeIfPresent(Double.self, forKey: .tint) ?? d.tint
        lift = try c.decodeIfPresent(Double.self, forKey: .lift) ?? d.lift
        gamma = try c.decodeIfPresent(Double.self, forKey: .gamma) ?? d.gamma
        gain = try c.decodeIfPresent(Double.self, forKey: .gain) ?? d.gain
        if c.contains(.curves), try c.decodeNil(forKey: .curves) {
            throw GradeValidationError(message: "Four curve channels are required.")
        }
        curves = try c.decodeIfPresent([[GradeCurvePoint]].self, forKey: .curves) ?? d.curves
        cubeText = try c.decodeIfPresent(String.self, forKey: .cubeText) ?? d.cubeText
        cubeSha256 = try c.decodeIfPresent(String.self, forKey: .cubeSha256) ?? d.cubeSha256
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? d.name
    }
}

struct AdvancedGradeDocument: Codable, Equatable {
    var version = 2
    var colorSpace = "rec709-sdr"
    var bypass = false
    var intensity = 1.0
    var operations: [GradeOperation] = []

    static let maxCubeTextBytes = 1_500_000
    static let maxTransportBytes = 2_000_000

    /// C# Validate, in its order and with its messages. A cube's text must be present, parse
    /// (CubeLutParser, G3; Phase 1's D8 checked only the hash) and match its SHA-256, within the
    /// 1.5 MB cap.
    func validate() throws {
        func fail(_ message: String) -> GradeValidationError { GradeValidationError(message: message) }
        func range(_ value: Double, _ lo: Double, _ hi: Double) throws {
            guard value.isFinite, value >= lo, value <= hi else { throw fail("Grade parameter is outside its supported range.") }
        }
        guard version == 2, colorSpace == "rec709-sdr" else { throw fail("Unsupported grade version or color space.") }
        try range(intensity, 0, 1)
        guard operations.count <= 8, Set(operations.map(\.id)).count == operations.count else {
            throw fail("Grades allow eight uniquely identified adjustments.")
        }
        guard operations.reduce(0, { $0 + $1.cubeText.utf8.count }) <= Self.maxCubeTextBytes else {
            throw fail("A grade can embed up to 1.5 MB of LUT text.")
        }
        for op in operations {
            guard !op.id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, op.id.utf8.count <= 64,
                  ["primaries", "curves", "cube"].contains(op.kind)
            else { throw fail("Unsupported grade adjustment.") }
            try range(op.intensity, 0, 1); try range(op.exposureStops, -8, 8)
            try range(op.contrast, 0, 4); try range(op.pivot, 0, 1); try range(op.saturation, 0, 4)
            try range(op.temperature, -1, 1); try range(op.tint, -1, 1); try range(op.lift, -1, 1)
            try range(op.gamma, 0.1, 4); try range(op.gain, 0, 4)
            if op.kind == "cube" {
                guard !op.cubeText.isEmpty else { throw fail("LUT content is required.") }
                _ = try CubeLutParser.parse(op.cubeText)      // G3: full parse, before the hash (C# order)
                guard op.cubeSha256 == GradeDocumentRef.sha256Hex(op.cubeText) else {
                    throw fail("LUT content does not match its stored hash.")
                }
            }
            guard op.curves.count == 4 else { throw fail("Four curve channels are required.") }
            for curve in op.curves {
                guard (2...16).contains(curve.count), curve.first?.x == 0, curve.last?.x == 1 else {
                    throw fail("Curves require endpoints and at most sixteen points.")
                }
                var previous: Float = -1
                for point in curve {
                    try range(point.x, 0, 1); try range(point.y, 0, 1)
                    guard Float(point.x) > previous else { throw fail("Curve inputs must be strictly ordered.") }
                    previous = Float(point.x)
                }
            }
        }
    }

    /// C# AdvancedIndicator (ColorGradeEditorViewModel.Advanced.cs).
    var indicator: String {
        if bypass { return "Grade bypassed" }
        if intensity == 0 { return "Overall grade intensity 0" }
        if operations.contains(where: { $0.enabled && $0.intensity > 0 }) { return "Advanced adjustments active" }
        if !operations.isEmpty { return "Advanced adjustments disabled" }
        if intensity != 1 { return "Overall grade intensity \(Int((intensity * 100).rounded()))%" }
        return "No advanced adjustments"
    }

    /// The exact text that is hashed and sent (sorted keys, every field present).
    func canonicalJSON() throws -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return String(decoding: try encoder.encode(self), as: UTF8.self)
    }
}

extension AdvancedGradeDocument {
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = try c.decodeIfPresent(Int.self, forKey: .version) ?? 2
        colorSpace = try c.decodeIfPresent(String.self, forKey: .colorSpace) ?? "rec709-sdr"
        bypass = try c.decodeIfPresent(Bool.self, forKey: .bypass) ?? false
        intensity = try c.decodeIfPresent(Double.self, forKey: .intensity) ?? 1
        if c.contains(.operations), try c.decodeNil(forKey: .operations) {
            throw GradeValidationError(message: "Grades allow eight uniquely identified adjustments.")
        }
        // An explicit null element fails to decode as a GradeOperation, so it is refused too.
        operations = try c.decodeIfPresent([GradeOperation].self, forKey: .operations) ?? []
    }
}

enum GradeDocumentRef {
    /// Lowercase hex SHA-256 of the UTF-8 text (register-grade-document's documentRef).
    static func sha256Hex(_ text: String) -> String {
        SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
    }
}

/// .cvgrade presets: the Windows CopyGradeJson / PasteGradeJson shapes.
enum GradePreset {
    static let maxBytes = 16_500_000

    enum Pasted: Equatable { case grade(SourceColorGrade), document(AdvancedGradeDocument) }

    /// {"grade": …, "presetVersion": 1}, keys sorted.
    static func encode(_ grade: SourceColorGrade) throws -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return String(decoding: try encoder.encode(Envelope(grade: grade, presetVersion: 1)), as: UTF8.self)
    }

    /// GradeValidationError with the Windows message on refusal (VM.Advanced.cs:158-177).
    static func decode(_ text: String) throws -> Pasted {
        guard text.utf16.count <= maxBytes else { throw GradeValidationError(message: "Grade document is too large.") }
        let data = Data(text.utf8)
        guard let root = (try? JSONSerialization.jsonObject(with: data)) as? JSONObject else {
            throw GradeValidationError(message: "Unsupported grade preset.")
        }
        if let version = root["presetVersion"] {
            // A JSON `true` bridges to NSNumber 1 too; only a JSON number 1 is preset version 1.
            guard let number = version as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(), number.intValue == 1,
                  let node = root["grade"] as? JSONObject, JSONSerialization.isValidJSONObject(node)
            else { throw GradeValidationError(message: "Unsupported grade preset.") }
            guard node["lut"] is String else { throw GradeValidationError(message: "Missing grade.") }
            let grade: SourceColorGrade
            do { grade = try JSONDecoder().decode(SourceColorGrade.self, from: JSONSerialization.data(withJSONObject: node)) }
            catch let error as GradeValidationError { throw error }
            catch { throw GradeValidationError(message: "Unsupported grade preset.") }
            let checked = try grade.validated(range: SourceColorGrade.wireRange)
            try checked.advanced?.validate()
            return .grade(checked)
        }
        let document: AdvancedGradeDocument
        do { document = try JSONDecoder().decode(AdvancedGradeDocument.self, from: data) }
        catch let error as GradeValidationError { throw error }
        catch { throw GradeValidationError(message: "Unsupported grade document.") }
        try document.validate()
        return .document(document)
    }

    private struct Envelope: Encodable { let grade: SourceColorGrade; let presetVersion: Int }
}

extension CoreCommands {
    /// Root-level. `documentJson` only when uploading (the first request only asks).
    static func registerGradeDocument(ref: String, json: String?) -> JSONObject {
        var body: JSONObject = ["type": "register-grade-document", "documentRef": ref]
        if let json { body["documentJson"] = json }
        return body
    }
}
