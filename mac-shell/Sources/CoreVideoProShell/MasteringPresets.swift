// Mastering presets and the user-preset library — a port of the Windows
// W/Models/MasteringPresetCatalog.cs and W/Models/MasteringPresetLibrary.cs.
// Pure: no model, no I/O. Windows throws on an unknown preset or stage id; the
// Mac returns nil (the menu only offers known ids, and a remote caller gets a
// refusal instead of a crash).

import Foundation

enum MasteringPresetCatalog {
    /// Windows TargetIndex -> LUFS (StudioViewModel.cs:1291).
    static let targetLufsByIndex: [Double] = [-14, -16, -23]

    /// Positional, exactly the Windows constructor order (CatalogCs:35-45):
    /// Enabled, TargetIndex, Glue, Ceiling, MaxRide, InputGain, HP, LP,
    /// LowShelf, Presence, HighShelf, Width, Limiter. Glue character fields are
    /// the record defaults (2.0 / 30 / 250 / 0 / off / 0 / 0 / 0).
    private static func make(_ enabled: Bool, _ targetIndex: Int, _ glue: Double, _ ceiling: Double,
                             _ maxRide: Double, _ inputGain: Double, _ highPass: Double, _ lowPass: Double,
                             _ lowShelf: Double, _ presence: Double, _ highShelf: Double, _ width: Double,
                             _ limiter: Bool) -> MasteringParams {
        var p = MasteringParams()
        p.enabled = enabled; p.targetLufs = targetLufsByIndex[targetIndex]; p.glueAmount = glue
        p.ceilingDbfs = ceiling; p.maxRideDb = maxRide; p.inputGainDb = inputGain
        p.highPassHz = highPass; p.lowPassHz = lowPass; p.lowShelfDb = lowShelf
        p.presenceDb = presence; p.highShelfDb = highShelf; p.stereoWidth = width
        p.limiterEnabled = limiter
        p.glueRatio = 2.0; p.glueAttackMs = 30.0; p.glueReleaseMs = 250.0; p.glueMakeupDb = 0
        p.glueMultiband = false; p.glueBandLowDb = 0; p.glueBandMidDb = 0; p.glueBandHighDb = 0
        return p
    }

    static var neutral: MasteringParams { make(true, 0, 0.0, -1.3, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, true) }

    /// Windows MasteringPresetCatalog.Create (ids trimmed + lower-cased).
    static func create(_ presetId: String) -> MasteringParams? {
        switch presetId.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "streaming": return make(true, 0, 0.35, -1.0, 6.0, 0.0, 30.0, 0.0, 0.5, 1.0, 0.5, 1.05, true)
        case "podcast": return make(true, 1, 0.45, -1.0, 8.0, 0.0, 70.0, 0.0, 0.0, 1.5, 0.5, 1.0, true)
        case "broadcast": return make(true, 2, 0.30, -1.0, 4.0, 0.0, 30.0, 0.0, 0.0, 0.5, 0.0, 1.0, true)
        case "neutral": return neutral
        default: return nil
        }
    }

    /// Windows MasteringPresetCatalog.ResetStage. Never touches enabled or target.
    static func resetStage(_ current: MasteringParams, stage: String) -> MasteringParams? {
        var p = current
        switch stage.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "input": p.inputGainDb = 0
        case "filter": p.highPassHz = 0; p.lowPassHz = 0
        case "tone": p.lowShelfDb = 0; p.presenceDb = 0; p.highShelfDb = 0
        case "ride": p.maxRideDb = 0
        case "glue", "dynamics":
            p.glueAmount = 0; p.glueRatio = 2.0; p.glueAttackMs = 30.0; p.glueReleaseMs = 250.0
            p.glueMakeupDb = 0; p.glueMultiband = false
            p.glueBandLowDb = 0; p.glueBandMidDb = 0; p.glueBandHighDb = 0
        case "width": p.stereoWidth = 1.0
        case "loudness", "image": p.maxRideDb = 0; p.stereoWidth = 1.0
        case "ceiling", "limiter": p.ceilingDbfs = -1.3; p.limiterEnabled = true
        default: return nil
        }
        return p
    }
}

/// Windows MasteringUserPreset.
struct MasteringUserPreset: Codable, Equatable, Identifiable {
    var id: String
    var name: String
    var settings: MasteringParams
}

/// Windows MasteringPresetLibrary. Operations never throw; failure is nil.
/// Insertion order, never sorted.
enum MasteringPresetLibrary {
    static let builtInNames = ["Streaming", "Podcast", "Broadcast", "Neutral"]
    static let maxPresets = 32
    static let maxNameLength = 48

    static func isBuiltInName(_ name: String?) -> Bool {
        guard let trimmed = name?.trimmingCharacters(in: .whitespacesAndNewlines), !trimmed.isEmpty else { return false }
        return builtInNames.contains { $0.caseInsensitiveCompare(trimmed) == .orderedSame }
    }

    /// Trimmed; nil when empty, longer than 48 (rejected, not truncated) or built in.
    static func normalizeName(_ name: String?) -> String? {
        guard let trimmed = name?.trimmingCharacters(in: .whitespacesAndNewlines), !trimmed.isEmpty,
              trimmed.count <= maxNameLength, !isBuiltInName(trimmed) else { return nil }
        return trimmed
    }

    private static func sameName(_ a: String, _ b: String) -> Bool { a.caseInsensitiveCompare(b) == .orderedSame }

    /// Same name (any case) overwrites in place keeping id and position; a new
    /// name appends `user-<32 hex>`; a full library refuses only a NEW name.
    static func save(_ presets: [MasteringUserPreset], name: String?, settings: MasteringParams,
                     newId: () -> String = { "user-" + UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased() })
        -> (presets: [MasteringUserPreset], saved: MasteringUserPreset)? {
        guard let normalized = normalizeName(name) else { return nil }
        if let index = presets.firstIndex(where: { sameName($0.name, normalized) }) {
            var updated = presets
            updated[index].name = normalized
            updated[index].settings = settings
            return (updated, updated[index])
        }
        guard presets.count < maxPresets else { return nil }
        let saved = MasteringUserPreset(id: newId(), name: normalized, settings: settings)
        return (presets + [saved], saved)
    }

    static func rename(_ presets: [MasteringUserPreset], id: String?, to newName: String?)
        -> (presets: [MasteringUserPreset], renamed: MasteringUserPreset)? {
        guard let normalized = normalizeName(newName),
              let index = presets.firstIndex(where: { $0.id == id }) else { return nil }
        if presets.contains(where: { $0.id != presets[index].id && sameName($0.name, normalized) }) { return nil }
        var updated = presets
        updated[index].name = normalized
        return (updated, updated[index])
    }

    static func delete(_ presets: [MasteringUserPreset], id: String?)
        -> (presets: [MasteringUserPreset], deleted: MasteringUserPreset)? {
        guard let target = presets.first(where: { $0.id == id }) else { return nil }
        return (presets.filter { $0.id != target.id }, target)
    }

    /// Windows FromPersisted: the rack limits. The target is the Mac's continuous
    /// slider range (-24...-9 LUFS, AudioConsole.swift) in place of Windows' index 0...2.
    static func clamped(_ p: MasteringParams) -> MasteringParams {
        func c(_ v: Double, _ lo: Double, _ hi: Double) -> Double { v.isFinite ? min(hi, max(lo, v)) : lo }
        var r = p
        r.targetLufs = c(p.targetLufs, -24, -9)
        r.glueAmount = c(p.glueAmount, 0, 1)
        r.ceilingDbfs = c(p.ceilingDbfs, -6, -0.5)
        r.maxRideDb = c(p.maxRideDb, 0, 12)
        r.inputGainDb = c(p.inputGainDb, -12, 12)
        r.highPassHz = c(p.highPassHz, 0, 300)
        r.lowPassHz = c(p.lowPassHz, 0, 20000)
        r.lowShelfDb = c(p.lowShelfDb, -12, 12)
        r.presenceDb = c(p.presenceDb, -12, 12)
        r.highShelfDb = c(p.highShelfDb, -12, 12)
        r.stereoWidth = c(p.stereoWidth, 0, 2)
        r.glueRatio = c(p.glueRatio, 1, 10)
        r.glueAttackMs = c(p.glueAttackMs, 1, 200)
        r.glueReleaseMs = c(p.glueReleaseMs, 20, 1000)
        r.glueMakeupDb = c(p.glueMakeupDb, -6, 6)
        r.glueBandLowDb = c(p.glueBandLowDb, -6, 6)
        r.glueBandMidDb = c(p.glueBandMidDb, -6, 6)
        r.glueBandHighDb = c(p.glueBandHighDb, -6, 6)
        return r
    }

    /// Windows FromPersistedPresets: drop blank id, unusable or duplicate names
    /// (first wins), clamp each, stop at 32.
    static func loaded(_ persisted: [MasteringUserPreset]?) -> [MasteringUserPreset] {
        var restored: [MasteringUserPreset] = []
        for preset in persisted ?? [] {
            guard !preset.id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  let name = normalizeName(preset.name),
                  !restored.contains(where: { sameName($0.name, name) }) else { continue }
            restored.append(MasteringUserPreset(id: preset.id, name: name, settings: clamped(preset.settings)))
            if restored.count >= maxPresets { break }
        }
        return restored
    }
}
