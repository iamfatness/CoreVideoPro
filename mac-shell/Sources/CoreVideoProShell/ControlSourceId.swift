// ControlSourceId.swift — the ONE place that maps the control API's canonical
// source ids (Windows `zoom:<pid>` / `capture:<id>` / `media:<id>`) onto the Mac's
// Show Input slots (a `kind` plus a bare `sourceId`), and a Mac slot onto the
// Windows ShowInputKind name `/state` publishes. If the Sources rebuild changes
// the slot model, this file (and control/source-id-and-kinds) is what moves.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 7).

import Foundation

enum ControlSourceId: Equatable {
    case zoom(String), capture(String), media(String)

    /// Case-sensitive (Ordinal) prefixes, like Windows; an empty remainder is not an id.
    static func parse(_ text: String) -> ControlSourceId? {
        for (prefix, make) in [("zoom:", ControlSourceId.zoom), ("capture:", ControlSourceId.capture),
                               ("media:", ControlSourceId.media)] where text.hasPrefix(prefix) {
            let rest = String(text.dropFirst(prefix.count))
            return rest.isEmpty ? nil : make(rest)
        }
        return nil
    }

    static func canonical(_ id: ControlSourceId) -> String {
        switch id {
        case .zoom(let pid): return "zoom:" + pid
        case .capture(let device): return "capture:" + device
        case .media(let asset): return "media:" + asset
        }
    }

    /// A Mac slot's canonical id: "zoom:<id>" / "capture:<id>", nil for an empty slot.
    static func canonical(kind: String, sourceId: String) -> String? {
        guard !sourceId.isEmpty else { return nil }
        switch kind {
        case "zoom": return canonical(.zoom(sourceId))
        case "capture": return canonical(.capture(sourceId))
        default: return nil
        }
    }

    /// Windows ShowInputKind name for a Mac slot (ShowInputRosterService.InferCaptureDeviceKind,
    /// W/Services/ShowInputRosterService.cs:96-116; a device missing from the list is a webcam,
    /// StudioControlSurface.ResolveCaptureKind).
    static func windowsKind(of slot: ShowInputSlot, devices: [CaptureDeviceRow]) -> String {
        switch slot.kind {
        case "zoom": return "ZoomParticipant"
        case "capture":
            if slot.sourceId.hasPrefix("screen:") { return "Screen" }
            if slot.sourceId.hasPrefix("browser:") { return "Browser" }
            switch devices.first(where: { $0.id == slot.sourceId })?.vendor.lowercased() ?? "" {
            case "blackmagic": return "Blackmagic"
            case "aja": return "Aja"
            case "srt": return "SrtIngest"
            case "rtmp": return "RtmpIngest"
            case "ndi": return "Ndi"
            default: return "UvcWebcam"
            }
        default: return "Unassigned"
        }
    }
}
