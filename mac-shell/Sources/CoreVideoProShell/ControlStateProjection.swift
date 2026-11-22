// ControlStateProjection.swift — pure projections from the Mac model onto the
// Windows `ControlState` fields (C/ControlState.cs), plus the push key that keeps
// meter churn from pushing /ws and OSC feedback ten times a second.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 7).

import Foundation

enum ControlStateProjection {
    /// Windows StudioViewModel's two Zoom status texts (StudioViewModel.cs:11098,11145).
    /// The Mac core publishes `in_meeting` (AppModel.applyZoom); anything else is offline.
    static func zoomStatus(meetingState: String) -> String {
        meetingState == "in_meeting" ? "Zoom Live" : "Zoom Offline"
    }

    /// Windows' engine texts where Windows has one (StudioViewModel.cs:11112,11118);
    /// "Engine off" and "no meeting" are Mac texts (Windows' are free-form).
    static func engineStatus(captureEnabled: Bool, rawMediaActive: Bool, meetingState: String) -> String {
        guard captureEnabled else { return "Engine off" }
        if rawMediaActive { return "Engine on — Zoom media live." }
        if meetingState == "in_meeting" {
            return "Engine waiting for Zoom media access — ask the host to approve the request."
        }
        return "Engine on — no meeting."
    }

    /// One row per slot, in slot order: Windows `ControlInputState` camelCased.
    static func inputs(_ slots: [ShowInputSlot], devices: [CaptureDeviceRow]) -> [[String: Any]] {
        slots.map { slot in
            [
                "slot": slot.id,
                "inShow": slot.inShow,
                "kind": ControlSourceId.windowsKind(of: slot, devices: devices),
                "sourceId": ControlSourceId.canonical(kind: slot.kind, sourceId: slot.sourceId).map { $0 as Any } ?? NSNull(),
                "name": slot.name,
            ]
        }
    }
}

/// The comparison key for a state push. A push still SENDS the full state, meters
/// included; meter-only changes just never cause one (the Mac form of Windows'
/// curated `FeedbackProps`, StudioControlSurface.cs:27-55).
enum ControlStatePushKey {
    static let meterKeys: Set<String> = ["programTruePeakDbfs", "programLoudnessLufs"]
    static let audioSourceMeterKeys: Set<String> = ["level", "peakDbfs", "rmsDbfs"]

    static func key(_ state: [String: Any]) -> String {
        var filtered = state.filter { !meterKeys.contains($0.key) }
        if let sources = filtered["audioSources"] as? [[String: Any]] {
            filtered["audioSources"] = sources.map { $0.filter { !audioSourceMeterKeys.contains($0.key) } }
        }
        return ControlJSON.encode(filtered) ?? ""
    }
}
