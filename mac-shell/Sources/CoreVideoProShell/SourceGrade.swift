// Per-source colour grade: the model, the source identity a grade is bound to, and the
// saved-grades file (grading plan 2026-11-16, Task 4; spec
// docs/superpowers/specs/2026-11-15-mac-source-grading-scopes-design.md §5).
//
// This is NOT the Program Look (ColorGrade.swift, `set-color-grade`). A per-source grade is
// applied by the core per plan layer and REPLACES the Program Look on that layer.

import Foundation

/// Twin of the Windows ColorGrade (Models/ProductionModels.cs). JSON keys are the
/// System.Text.Json Web-default camelCase names, so presets move between platforms.
struct SourceColorGrade: Codable, Equatable {
    var lut = "none"
    var exposure = 0
    var contrast = 0
    var saturation = 0
    var temperature = 0
    /// The advanced document (G2); travels to the core by reference (register-grade-document).
    var advanced: AdvancedGradeDocument? = nil

    static let looks = ["none", "neutral", "warm-film", "cool-broadcast", "punch"]
    static let editorRange = -50...50      // ColorGradeEditorWindow.xaml slider range
    static let wireRange = -100...100      // MediaCore::applySourceGrade refuses |axis| > 100

    init(lut: String = "none", exposure: Int = 0, contrast: Int = 0, saturation: Int = 0,
         temperature: Int = 0, advanced: AdvancedGradeDocument? = nil) {
        self.lut = lut
        self.exposure = exposure
        self.contrast = contrast
        self.saturation = saturation
        self.temperature = temperature
        self.advanced = advanced
    }

    /// Tolerant like System.Text.Json: a missing key keeps the C# default. A present key of
    /// the wrong type throws (the saved-file loader names that entry and skips it).
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        lut = try c.decodeIfPresent(String.self, forKey: .lut) ?? "none"
        exposure = try c.decodeIfPresent(Int.self, forKey: .exposure) ?? 0
        contrast = try c.decodeIfPresent(Int.self, forKey: .contrast) ?? 0
        saturation = try c.decodeIfPresent(Int.self, forKey: .saturation) ?? 0
        temperature = try c.decodeIfPresent(Int.self, forKey: .temperature) ?? 0
        advanced = try c.decodeIfPresent(AdvancedGradeDocument.self, forKey: .advanced)
    }

    func validated(range: ClosedRange<Int>) throws -> SourceColorGrade {
        guard Self.looks.contains(lut),
              [exposure, contrast, saturation, temperature].allSatisfy(range.contains)
        else { throw GradeValidationError(message: "Unsupported Basic grade.") }
        return self
    }

    /// Windows ColorGrade.Summary, exactly (temperature is not in it there either).
    var summary: String {
        func f(_ v: Int) -> String { v > 0 ? "+\(v)" : "\(v)" }
        return "Look \(lut) · exposure \(f(exposure)) · contrast \(f(contrast)) · saturation \(f(saturation))"
    }

    /// Look "none", every axis 0 and no document. The "neutral" LOOK adds contrast, so it is
    /// not neutral.
    var isNeutral: Bool {
        lut == "none" && exposure == 0 && contrast == 0 && saturation == 0 && temperature == 0 && advanced == nil
    }

    /// The `grade` object of `set-source-grade` (root fields read by MediaCore::applySourceGrade).
    func wireGrade(documentRef: String?) -> JSONObject {
        var grade: JSONObject = ["lut": lut, "exposure": exposure, "contrast": contrast,
                                 "saturation": saturation, "temperature": temperature]
        if let documentRef { grade["advanced"] = ["documentRef": documentRef] }
        return grade
    }
}

struct GradeValidationError: Error, Equatable { let message: String }

/// The source a grade is for, and the identity it is BOUND to. The frame key (`sourceId`) is
/// what the core matches; the binding is what decides whether a grade may land on the source
/// that currently holds that key (a reused Zoom id with another person must not).
enum GradeSourceIdentity: Equatable {
    case capture(deviceId: String)
    case zoom(participantId: String, persistentId: String, sourceGeneration: Int64)

    /// The frame key: "capture:<id>", or the RAW Zoom participant id (never "zoom:<pid>").
    var sourceId: String {
        switch self {
        case .capture(let deviceId): return "capture:\(deviceId)"
        case .zoom(let participantId, _, _): return participantId
        }
    }

    /// The saved-file key (Windows GradePersistenceKey); nil for an anonymous Zoom source.
    var stableKey: String? {
        switch self {
        case .capture(let deviceId): return "capture:\(deviceId)"
        case .zoom(_, let persistentId, _): return persistentId.isEmpty ? nil : "zoom-person:\(persistentId)"
        }
    }

    var binding: String {
        switch self {
        case .capture(let deviceId): return "capture:\(deviceId)"
        case .zoom(let participantId, let persistentId, let generation):
            return persistentId.isEmpty ? "gen:\(participantId):\(generation)" : "person:\(persistentId)"
        }
    }

    var isZoom: Bool { if case .zoom = self { return true }; return false }
}

/// ~/Library/Application Support/CoreVideoPro/source-grades.json (spec §5.9). Separate from
/// ShellPrefs because a G2/G3 grade can embed 1.5 MB of LUT text.
struct SourceGradeFile: Equatable {
    var grades: [String: SourceColorGrade] = [:]
    var order: [String] = []               // oldest first; every key of `grades` exactly once
    /// G7 (Phase 2 Task 9, plan P11): per-person scope regions, revision 0, at most 64.
    var scopeRegions: [String: GradeScopeRoi] = [:]
    var scopeRegionOrder: [String] = []    // oldest first

    static let capacity = 64
    static let retiredNotice = " · oldest saved source grade retired (64-source limit)"

    /// The operator's file. Never touched by a harness run.
    static let realPath = NSHomeDirectory()
        + "/Library/Application Support/CoreVideoPro/source-grades.json"

    /// The ShellPrefs.path rule: under the harness, a per-process temp file.
    static let path: String = {
        guard ShellHarness.isActive else { return realPath }
        return NSTemporaryDirectory() + "corevideo-shell-harness-\(getpid())/source-grades.json"
    }()

    static func isPersistableKey(_ key: String) -> Bool {
        key.hasPrefix("capture:") || key.hasPrefix("zoom-person:")
    }

    /// Tolerant per entry. An unreadable file is an empty file with no notice.
    static func load(from path: String) -> (file: SourceGradeFile, notices: [String]) {
        guard let data = FileManager.default.contents(atPath: path),
              let root = (try? JSONSerialization.jsonObject(with: data)) as? JSONObject,
              let rawGrades = root["grades"] as? JSONObject
        else { return (SourceGradeFile(), []) }
        let listed = (root["order"] as? [Any])?.compactMap { $0 as? String } ?? []
        var seen = Set<String>()
        var candidates = listed.filter { seen.insert($0).inserted && rawGrades[$0] != nil }
        candidates += rawGrades.keys.filter { !seen.contains($0) }.sorted()
        var file = SourceGradeFile()
        var notices: [String] = []
        for key in candidates {
            guard file.order.count < capacity else { break }
            guard isPersistableKey(key) else { continue }          // foreign keys: silently dropped
            do {
                let value = rawGrades[key] ?? NSNull()
                guard JSONSerialization.isValidJSONObject(value) else {
                    throw GradeValidationError(message: "\(key) is not a grade")
                }
                let entry = try JSONSerialization.data(withJSONObject: value)
                file.grades[key] = try JSONDecoder().decode(SourceColorGrade.self, from: entry)
                file.order.append(key)
            } catch let error as GradeValidationError {
                notices.append("Saved grade unavailable: \(error.message)")
            } catch {
                notices.append("Saved grade unavailable: \(key) could not be read")
            }
        }
        // Scope regions: only stable keys and valid regions survive (StudioViewModel.ScopePreferences.cs:7-12).
        if let rawRegions = root["scopeRegions"] as? JSONObject {
            let listedRegions = (root["scopeRegionOrder"] as? [Any])?.compactMap { $0 as? String } ?? []
            var seenRegions = Set<String>()
            var keys = listedRegions.filter { seenRegions.insert($0).inserted && rawRegions[$0] != nil }
            keys += rawRegions.keys.filter { !seenRegions.contains($0) }.sorted()
            for key in keys where isPersistableKey(key) && file.scopeRegionOrder.count < capacity {
                guard let node = rawRegions[key] as? JSONObject, let roi = GradeScopeRoi(json: node), roi.isValid else { continue }
                file.scopeRegions[key] = roi
                file.scopeRegionOrder.append(key)
            }
        }
        return (file, notices)
    }

    /// Atomic write; creates the directory.
    func save(to path: String) throws {
        let dir = (path as NSString).deletingLastPathComponent
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let body = Envelope(version: 1, grades: grades, order: order,
                            scopeRegions: scopeRegions, scopeRegionOrder: scopeRegionOrder)
        try encoder.encode(body).write(to: URL(fileURLWithPath: path), options: .atomic)
    }

    /// Re-saving a key moves it to the end; a 65th key retires the oldest and returns the notice.
    mutating func remember(_ grade: SourceColorGrade, key: String) -> String? {
        if grades[key] != nil {
            order.removeAll { $0 == key }
            order.append(key)
            grades[key] = grade
            return nil
        }
        var notice: String?
        if order.count >= Self.capacity, let oldest = order.first {
            order.removeFirst()
            grades[oldest] = nil
            notice = Self.retiredNotice
        }
        grades[key] = grade
        order.append(key)
        return notice
    }

    /// Revision 0, newest last; a 65th key retires the oldest.
    mutating func rememberScopeRegion(_ roi: GradeScopeRoi, key: String) {
        guard Self.isPersistableKey(key), roi.isValid else { return }
        var stored = roi; stored.revision = 0
        scopeRegionOrder.removeAll { $0 == key }
        scopeRegionOrder.append(key)
        scopeRegions[key] = stored
        while scopeRegionOrder.count > Self.capacity { scopeRegions[scopeRegionOrder.removeFirst()] = nil }
    }

    mutating func forget(key: String) {
        grades[key] = nil
        order.removeAll { $0 == key }
    }

    private struct Envelope: Encodable {
        let version: Int
        let grades: [String: SourceColorGrade]
        let order: [String]
        let scopeRegions: [String: GradeScopeRoi]
        let scopeRegionOrder: [String]
    }
}
