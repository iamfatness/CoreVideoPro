// CoreVideo Tiles shell policies, ported from the Windows services of the same
// names (native-shell/CoreVideoPro.WinUI/Services/Tiles*.cs). Pure functions over
// values; AppModel+Tiles feeds them the live roster, slots and devices.

import Foundation

enum TilesMembershipPolicy {
    /// `TilesMembershipPolicy.NormalizeMode`: "manual" | "routed" | else "eligible".
    static func normalizeMode(_ mode: String?) -> String {
        switch mode?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "manual": return "manual"
        case "routed": return "routed"
        default: return "eligible"
        }
    }

    /// `TilesMembershipPolicy.Resolve`, statement for statement. `roster == nil`
    /// uses saved keys as-is; otherwise every saved key resolves through
    /// `TilesIdentityPolicy.resolveLiveSourceId` (Windows' builder always passes a roster).
    static func resolve(_ s: TilesSettings, eligible: [String], routed: Set<String>?,
                        roster: [TilesIdentityPolicy.Person]?, meetingId: String?) -> [String] {
        let mode = normalizeMode(s.membershipMode)
        func live(_ key: String?) -> String? {
            guard let roster else { return key }
            return TilesIdentityPolicy.resolveLiveSourceId(key, roster: roster, currentMeetingId: meetingId,
                                                           boundMeetingId: s.boundMeetingId)
        }
        var excluded = Set<String>()
        for key in s.excludedSourceIds + (mode == "eligible" ? [] : s.omittedSourceIds) {
            if let id = live(key), !id.isEmpty { excluded.insert(id) }
        }
        var available: [String] = []
        var seen = Set<String>()
        for id in eligible where !id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && !excluded.contains(id) && seen.insert(id).inserted {
            available.append(id)
        }
        if mode == "routed" {
            let routedSet = routed ?? []
            available = available.filter { routedSet.contains($0) }
        }
        let present = Set(available)
        var slots = [String?](repeating: nil, count: min(max(s.maxTiles, 1), 64))
        var used = Set<String>()
        let manual = s.manualSlots.map { live($0) }
        if mode == "manual" {
            // Slot positions are kept even when a source is absent: the core
            // reserves those cells and draws only fresh admitted sources.
            return manual.prefix(slots.count).map { id in
                if let id, !id.isEmpty, !excluded.contains(id), used.insert(id).inserted { return id }
                return ""
            }
        }
        for index in 0..<min(slots.count, manual.count) {
            if let id = manual[index], present.contains(id), used.insert(id).inserted { slots[index] = id }
        }
        var next = 0
        for id in available {
            if !used.insert(id).inserted { continue }
            while next < slots.count, slots[next] != nil { next += 1 }
            if next == slots.count { break }
            slots[next] = id
        }
        return slots.compactMap { $0 }
    }
}

/// A local (non-Zoom) show input the wall may seat (Windows `TilesLocalSource`).
struct TilesLocalSource: Equatable {
    let id: String
    let name: String
    let group: String
}

/// `TilesLocalSourcePolicy` (#1071): in-show local capture inputs are wall members too.
enum TilesLocalSourcePolicy {
    /// A canonical local id: "capture:" or "media:" (case-sensitive, like Windows).
    static func isLocalSourceId(_ id: String?) -> Bool {
        guard let id, !id.isEmpty else { return false }
        return id.hasPrefix("capture:") || id.hasPrefix("media:")
    }

    /// `GroupFor`, keyed by the Windows ShowInputKind name
    /// (`ControlSourceId.windowsKind(of:devices:)` produces it for a Mac slot).
    static func groupFor(windowsKind: String) -> String {
        switch windowsKind {
        case "Screen": return "Screen"
        case "SrtIngest": return "SRT"
        case "RtmpIngest": return "RTMP"
        case "Ndi": return "NDI"
        case "Browser": return "Browser"
        case "Media": return "Media"
        default: return "Camera"
        }
    }

    /// In-show slots bound to a local CAPTURE source (never media yet: the core
    /// opens media only for a route), slot order, deduped by "capture:<id>".
    /// Name: the device name when enumerated, else the slot label "Input NN".
    static func localVideoSources(_ slots: [ShowInputSlot], devices: [CaptureDeviceRow],
                                  displayName: (String, String) -> String = { $1 }) -> [TilesLocalSource] {
        var result: [TilesLocalSource] = []
        var seen = Set<String>()
        for slot in slots.sorted(by: { $0.id < $1.id }) where slot.inShow && slot.kind == "capture" {
            guard let id = slot.sourceKey, isLocalSourceId(id), seen.insert(id).inserted else { continue }
            // Windows falls back to `SlotLabel` ("Input NN"), never the slot's display name.
            let derived = devices.first { $0.id == slot.sourceId }?.name ?? String(format: "Input %02d", slot.id)
            result.append(TilesLocalSource(id: id, name: displayName(id, derived),
                                           group: groupFor(windowsKind: ControlSourceId.windowsKind(of: slot, devices: devices))))
        }
        return result
    }

    /// `RoutedSourceIds`: every in-show slot's canonical id, Zoom and local alike.
    static func routedSourceIds(_ slots: [ShowInputSlot]) -> Set<String> {
        Set(slots.filter(\.inShow).compactMap(\.sourceKey).filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty })
    }
}

/// `TilesIdentityPolicy` (#479): saved Zoom identities resolve to this meeting's
/// session id at use time; a leftover per-session id is never applied.
enum TilesIdentityPolicy {
    static let zoomPidPrefix = "zoom-pid:"
    static let zoomNamePrefix = "zoom-name:"
    static let zoomSessionPrefix = "zoom:"

    struct Person: Equatable {
        let sessionSourceId: String
        let displayName: String
        var persistentId: String? = nil
    }

    static func person(from participant: RosterParticipant) -> Person {
        Person(sessionSourceId: "zoom:" + participant.id, displayName: participant.name,
               persistentId: participant.persistentId)
    }

    /// `ResolveLiveSourceId`. (Windows takes the meeting ids but never reads them.)
    static func resolveLiveSourceId(_ persisted: String?, roster: [Person], currentMeetingId: String?,
                                    boundMeetingId: String?) -> String? {
        guard let persisted, !persisted.isEmpty else { return persisted }
        if TilesLocalSourcePolicy.isLocalSourceId(persisted) { return persisted }
        if persisted.hasPrefix(zoomPidPrefix) {
            let pid = String(persisted.dropFirst(zoomPidPrefix.count))
            return roster.first {
                guard let p = $0.persistentId, !p.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return false }
                return p == pid
            }?.sessionSourceId
        }
        if persisted.hasPrefix(zoomNamePrefix) {
            let name = String(persisted.dropFirst(zoomNamePrefix.count))
            var matches: [String] = []
            for entry in roster where entry.displayName.caseInsensitiveCompare(name) == .orderedSame
                && !matches.contains(entry.sessionSourceId) {
                matches.append(entry.sessionSourceId)
            }
            return matches.count == 1 ? matches[0] : nil
        }
        if persisted.hasPrefix(zoomSessionPrefix), persisted.split(separator: ":", omittingEmptySubsequences: false).count == 2 {
            return nil   // a per-session id is reassigned every meeting (#479)
        }
        return persisted
    }
}
