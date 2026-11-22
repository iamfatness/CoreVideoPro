// ZoomCameraQuality — Z1 of the outputs spec (§3.4 / §5.4): the Zoom camera
// ceilings. Verbatim ports of Windows ZoomCameraResolutionPreference and
// ZoomCameraFrameRatePreference (Services/*.cs), proven against the Windows
// test table (ZoomCameraResolutionPreferenceTests).
//
// Ruling carried from Windows (ZoomEngineRuntime.cpp:246-265): the preference is
// LATCHED by a valid join; an edit during a meeting applies at the next join.
// So "applied" is what the last successful join SENT (Windows
// onMeetingJoined(cameraMaxTier, cameraMaxFps)), not the current picker value.
// The fps cap limits local camera playout only, never Zoom transport or share.

import Foundation
import SwiftUI

enum ZoomCameraResolutionPreference {
    static let defaultChoice = "1080p"
    static let choices = ["360p", "720p", "1080p"]

    static func normalize(_ choice: String?) -> String {
        switch choice {
        case "360p": return "360p"
        case "720p": return "720p"
        default: return defaultChoice
        }
    }

    static func toSdkTier(_ choice: String?) -> Int {
        switch normalize(choice) {
        case "360p": return 0
        case "720p": return 1
        default: return 2
        }
    }

    static func fromSdkTier(_ tier: Int) -> String {
        switch tier {
        case 0: return "360p"
        case 1: return "720p"
        default: return defaultChoice
        }
    }

    static func dimensions(_ choice: String?) -> (width: Int, height: Int) {
        switch normalize(choice) {
        case "360p": return (640, 360)
        case "720p": return (1280, 720)
        default: return (1920, 1080)
        }
    }

    static func status(preferred: String?, applied: String?, inMeeting: Bool) -> String {
        let wanted = normalize(preferred)
        let current = normalize(applied)
        if !inMeeting {
            return "Preferred max \(wanted). Applies on the next Zoom join; Format shows actual delivery."
        }
        return wanted == current
            ? "Requested max \(current) this meeting. Format shows what Zoom actually delivers."
            : "Current request \(current); \(wanted) pending for next Zoom join."
    }
}

enum ZoomCameraFrameRatePreference {
    static let defaultFps = 60
    static let choices = [15, 24, 25, 30, 60]

    static func normalize(_ fps: Int) -> Int { choices.contains(fps) ? fps : defaultFps }

    static func status(preferred: Int, applied: Int, inMeeting: Bool) -> String {
        let wanted = normalize(preferred)
        let current = normalize(applied)
        if !inMeeting {
            return "Local guest video max \(wanted) fps applies on the next Zoom join. Format shows Zoom's delivered fps."
        }
        return wanted == current
            ? "Local guest video max \(current) fps this meeting. Zoom may deliver more; Format shows actual input."
            : "Local guest video max \(current) fps now; \(wanted) fps pending for next Zoom join."
    }
}

/// What one join sent, and what is applied in the current meeting.
struct ZoomCameraCeiling: Equatable {
    var tier = 2
    var fps = 60
}

extension AppModel {
    var zoomCameraMaxStatus: String {
        ZoomCameraResolutionPreference.status(
            preferred: zoomCameraMaxResolution,
            applied: ZoomCameraResolutionPreference.fromSdkTier(zoomCameraApplied.tier),
            inMeeting: Self.isInMeeting(meetingState))
    }

    var zoomCameraMaxFpsStatus: String {
        ZoomCameraFrameRatePreference.status(
            preferred: zoomCameraMaxFps, applied: zoomCameraApplied.fps,
            inMeeting: Self.isInMeeting(meetingState))
    }

    func setZoomCameraMaxResolution(_ choice: String) {
        let next = ZoomCameraResolutionPreference.normalize(choice)
        guard next != zoomCameraMaxResolution else { return }
        zoomCameraMaxResolution = next
        persistPrefsNow()
    }

    func setZoomCameraMaxFps(_ fps: Int) {
        let next = ZoomCameraFrameRatePreference.normalize(fps)
        guard next != zoomCameraMaxFps else { return }
        zoomCameraMaxFps = next
        persistPrefsNow()
    }

    /// The ceiling a join sends (Windows SettingsViewModel.cs:815-816).
    var zoomCameraJoinCeiling: ZoomCameraCeiling {
        ZoomCameraCeiling(tier: min(max(ZoomCameraResolutionPreference.toSdkTier(zoomCameraMaxResolution), 0), 2),
                          fps: ZoomCameraFrameRatePreference.normalize(zoomCameraMaxFps))
    }

    /// Called whenever meetingState changes: a join that reaches the meeting
    /// latches what it sent as the applied ceiling (Windows OnZoomCameraMeetingJoined).
    func latchZoomCameraCeilingIfJoined() {
        guard Self.isInMeeting(meetingState), let sent = zoomCameraJoinSent else { return }
        zoomCameraApplied = sent
        zoomCameraJoinSent = nil
    }
}

/// Zoom tab: the two pickers and their requested-vs-applied lines.
struct ZoomCameraQualityControls: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Zoom quality").font(.grotesk(13, .semibold))
            HStack(spacing: 12) {
                Picker("Camera max", selection: Binding(
                    get: { model.zoomCameraMaxResolution },
                    set: { model.setZoomCameraMaxResolution($0) })) {
                    ForEach(ZoomCameraResolutionPreference.choices, id: \.self) { Text($0).tag($0) }
                }
                .frame(width: 200)
                Picker("Guest fps max", selection: Binding(
                    get: { model.zoomCameraMaxFps },
                    set: { model.setZoomCameraMaxFps($0) })) {
                    ForEach(ZoomCameraFrameRatePreference.choices, id: \.self) { Text("\($0) fps").tag($0) }
                }
                .frame(width: 200)
            }
            Text(model.zoomCameraMaxStatus).font(.plexMono(10)).foregroundStyle(Studio.secondary)
            Text(model.zoomCameraMaxFpsStatus).font(.plexMono(10)).foregroundStyle(Studio.secondary)
        }
    }
}
