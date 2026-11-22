// SourcesWorkspace — the Sources table's pure half (sources plan Task 6):
// labelled source types, the grouped picker, the Windows column layout and the
// UI-only workspace state. Windows: SourcesTableRowControl.cs:112-120,
// SourcesWorkspaceControl.cs:86-161, ViewModels/SourcesWorkspaceState.cs,
// Services/ShowInputRosterService.cs:96-117,240-248.

import CoreGraphics
import Foundation

enum SourceType: String, Equatable {
    case zoomGuest = "Zoom guest", camera = "Camera", captureCard = "Capture card", screen = "Screen"
    case window = "Window", still = "Still", mediaClip = "Media clip", unassigned = "Unassigned"

    /// Mirrors Windows InferCaptureDeviceKind (ShowInputRosterService.cs:96-117) for the Mac ids:
    /// "screen:" → screen, "window:" → window (SckScreenCaptureAdapter.mm:269,287),
    /// vendor blackmagic|aja (case-insensitive) → captureCard, otherwise camera.
    static func of(kind: String, sourceId: String, vendor: String?, media: [MediaAsset] = []) -> SourceType {
        switch kind {
        case "zoom": return .zoomGuest
        case "media":
            // A missing asset is still a media slot; it reads as a clip until the bin says otherwise.
            return media.first { $0.id == sourceId }?.isStillImage == true ? .still : .mediaClip
        case "capture":
            if sourceId.hasPrefix("screen:") { return .screen }
            if sourceId.hasPrefix("window:") { return .window }
            switch (vendor ?? "").lowercased() {
            case "blackmagic", "aja": return .captureCard
            default: return .camera
            }
        default: return .unassigned
        }
    }

    var category: String {
        switch self {
        case .zoomGuest: return "Zoom"
        case .camera, .captureCard: return "Cameras"
        case .screen, .window: return "Screens"
        case .still, .mediaClip: return "Media"
        case .unassigned: return "Unassigned"
        }
    }

    var pickerPrefix: String {
        switch self {
        case .zoomGuest: return "Zoom · "
        case .camera: return "Camera · "
        case .captureCard: return "Capture card · "
        case .screen: return "Screen · "
        case .window: return "Window · "
        case .still: return "Still · "
        case .mediaClip: return "Media clip · "
        case .unassigned: return ""
        }
    }
}

struct SourcePickerOption: Equatable {
    let sourceKey: String
    let kind: String
    let sourceId: String
    let label: String
    let unavailable: Bool
}

enum SourcePickerCatalog {
    /// Groups in the Windows order Zoom, Camera, Screen. An empty group is omitted. When the slot's
    /// current source is absent, a first group carries one entry reading
    /// "Saved source unavailable — <name>" (ShowInputRosterService.cs:240-248): a missing saved
    /// source is named, never substituted.
    static func groups(roster: [RosterParticipant], devices: [CaptureDeviceRow], media: [MediaAsset] = [],
                       current: ShowInputSlot?) -> [(title: String, options: [SourcePickerOption])] {
        let zoom = roster.map {
            SourcePickerOption(sourceKey: "zoom:" + $0.id, kind: "zoom", sourceId: $0.id,
                               label: SourceType.zoomGuest.pickerPrefix + $0.name, unavailable: false)
        }
        func option(_ device: CaptureDeviceRow) -> (SourceType, SourcePickerOption) {
            let type = SourceType.of(kind: "capture", sourceId: device.id, vendor: device.vendor)
            return (type, SourcePickerOption(sourceKey: "capture:" + device.id, kind: "capture",
                                             sourceId: device.id, label: type.pickerPrefix + device.name,
                                             unavailable: false))
        }
        let typed = devices.map(option)
        let cameras = typed.filter { $0.0.category == "Cameras" }.map(\.1)
        let screens = typed.filter { $0.0.category == "Screens" }.map(\.1)
        // Every bin asset with a picture (Windows BuildSourceOptions :437-443); an audio-only
        // file has nothing to show in a slot.
        let pictures = media.filter {
            !MediaBin.audioExtensions.contains(($0.filePath as NSString).pathExtension.lowercased())
        }
        let mediaOptions = pictures.map { asset -> SourcePickerOption in
            let type: SourceType = asset.isStillImage ? .still : .mediaClip
            return SourcePickerOption(sourceKey: "media:" + asset.id, kind: "media", sourceId: asset.id,
                                      label: type.pickerPrefix + asset.name, unavailable: false)
        }
        var groups: [(title: String, options: [SourcePickerOption])] = [
            ("Zoom", zoom), ("Camera", cameras), ("Screen", screens), ("Media", mediaOptions),
        ].filter { !$0.options.isEmpty }
        if let current, let key = current.sourceKey,
           !groups.contains(where: { $0.options.contains { $0.sourceKey == key } }) {
            let name = current.name.isEmpty ? current.sourceId : current.name
            groups.insert(("Saved", [SourcePickerOption(
                sourceKey: key, kind: current.kind, sourceId: current.sourceId,
                label: "Saved source unavailable — \(name)", unavailable: true)]), at: 0)
        }
        return groups
    }
}

struct SourcesColumnPrefs: Codable, Equatable {
    var showType = true, showSignal = true, showAudio = true, inspectorOpen = false

    init() {}

    /// decodeIfPresent with the defaults: a missing field reads visible.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        showType = ((try? c.decodeIfPresent(Bool.self, forKey: .showType)) ?? nil) ?? true
        showSignal = ((try? c.decodeIfPresent(Bool.self, forKey: .showSignal)) ?? nil) ?? true
        showAudio = ((try? c.decodeIfPresent(Bool.self, forKey: .showAudio)) ?? nil) ?? true
        inspectorOpen = ((try? c.decodeIfPresent(Bool.self, forKey: .inspectorOpen)) ?? nil) ?? false
    }
}

enum SourcesColumnLayout {
    static let headers = ["#", "Source", "Type", "Format", "FPS", "Signal", "Audio", "MV", "ISO", "Actions"]
    static let inspectorWidth: CGFloat = 340

    /// Port of SourcesTableRowControl.ColumnWidths (:115-120). -1 = the flexible Source column;
    /// 0 = hidden.
    static func widths(tableWidth w: CGFloat, prefs: SourcesColumnPrefs) -> [CGFloat] {
        [26,
         -1,
         prefs.showType && w >= 1050 ? max(78, 0.08 * w) : 0,
         max(w >= 750 ? 100 : 85, 0.13 * w),
         max(42, 0.05 * w),
         prefs.showSignal && w >= 750 ? max(86, 0.09 * w) : 0,
         prefs.showAudio && w >= 1050 ? max(86, 0.12 * w) : 0,
         max(36, 0.04 * w),
         max(60, 0.06 * w),
         140]
    }

    /// The thumbnail shows from 750 (:112).
    static func showsThumbnail(tableWidth: CGFloat) -> Bool { tableWidth >= 750 }

    /// The inspector sits beside the table from 1500 (SourcesWorkspaceControl.cs:158).
    static func inspectorBeside(pageWidth: CGFloat) -> Bool { pageWidth >= 1500 }
}

/// UI-only; never writes slots, MV or ISO.
struct SourcesWorkspaceState: Equatable {
    static let categories = ["All sources", "Zoom", "Cameras", "Screens", "Media", "Unassigned"]
    var selectedSlotId: Int?
    var query = ""
    var category = "All sources"
    var columns = SourcesColumnPrefs()

    /// Select + open the inspector.
    mutating func inspect(_ slotId: Int) {
        selectedSlotId = slotId
        columns.inspectorOpen = true
    }

    /// ViewModels/SourcesWorkspaceState.cs:18-34. A filter never moves the selection.
    func matches(slotId: Int, displayName: String, sourceLabel: String, type: SourceType) -> Bool {
        guard category == "All sources" || category == type.category else { return false }
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return true }
        let haystack = String(format: "%02d", slotId) + " " + displayName + " " + sourceLabel + " " + type.rawValue
        return haystack.range(of: needle, options: .caseInsensitive) != nil
    }
}

enum SourcesSummary {
    /// "x/10 assigned · n in MV · n ISO selected · n match filter" (SourcesWorkspaceControl.cs:150).
    static func text(slots: [ShowInputSlot], matching: Int) -> String {
        let assigned = slots.filter { $0.kind != "unassigned" }
        let inMv = assigned.filter(\.inShow).count
        let iso = assigned.filter(\.iso).count
        return "\(assigned.count)/\(slots.count) assigned · \(inMv) in MV · \(iso) ISO selected · \(matching) match filter"
    }
}
