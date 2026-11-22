// CaptureAudioSync — Z2 of the outputs spec (§3.4 / §5.4): the per-capture-device
// audio sync nudge, -20 / +20 ms per press, clamped to +/-500 ms.
//
// Windows: SourcesInputsPage.xaml:121-134 (the two buttons and AudioSyncLabel on
// each capture device row), StudioViewModel.cs:7135-7172 (Nudge*/Reset,
// SetCaptureAudioSyncOffsetAsync), MediaCoreSupervisor.cs:845-870 (the wire).
//
// Two copies of the offset exist in the core and Windows feeds both:
//   - `set-capture-audio-sync-offset {deviceId, offsetMs}` sets the capture
//     DEVICE's copy (AvfCaptureDeviceAdapter.mm:184, applied to its embedded audio);
//   - `audioSyncOffsetMs` on that device's sync-capture-audio-sources entry sets
//     the paired microphone's copy (MediaCore.cpp:3461 ->
//     CoreAudioCaptureSourceAdapter.cpp:371).
//
// SESSION STATE, NOT PERSISTED (plan D2): Windows keeps it on CaptureDeviceItem
// and in no prefs file. The full-state sources entry carries it to every core
// generation; the device copy is reconciled against what each snapshot reports
// (a fresh core lists its cameras asynchronously and drops early commands).

import Foundation

enum CaptureAudioSyncPolicy {
    static let stepMs = 20
    static let limitMs = 500

    static func clamp(_ ms: Int) -> Int { min(max(ms, -limitMs), limitMs) }

    /// Review #5: the devices whose CORE copy of the offset differs from the
    /// operator's, among the devices the core actually lists. A fresh core fills
    /// its AVF device list asynchronously and drops a command for an unlisted id,
    /// so the shell reconciles against `captureDevices[].audioSyncOffsetMs`
    /// instead of trusting a one-shot send. Pure.
    static func pendingDeviceCopies(desired: [String: Int], reported: [JSONObject],
                                    inFlight: Set<String>) -> [(deviceId: String, offsetMs: Int)] {
        reported.compactMap { device -> (String, Int)? in
            guard let id = device["id"] as? String, !id.isEmpty, !inFlight.contains(id) else { return nil }
            let want = desired[id] ?? 0
            let have = (device["audioSyncOffsetMs"] as? NSNumber)?.intValue ?? 0
            return want == have ? nil : (id, want)
        }
    }

    /// Which capture rows carry the nudge: cameras and capture cards (video
    /// devices that may carry or pair audio), never a screen (no audio on Mac).
    static func offersNudge(category: String) -> Bool { category == "Cameras" }

    /// Windows CaptureDeviceItem.AudioSyncLabel, verbatim.
    static func label(_ ms: Int) -> String {
        ms == 0 ? "Audio sync 0 ms" : ms > 0 ? "Audio delayed +\(ms) ms" : "Audio advanced \(ms) ms"
    }
}

extension AppModel {
    func captureAudioSyncOffset(_ deviceId: String) -> Int { captureAudioSyncOffsets[deviceId] ?? 0 }

    func nudgeCaptureAudio(deviceId: String, deltaMs: Int) {
        setCaptureAudioSyncOffset(deviceId: deviceId, offsetMs: captureAudioSyncOffset(deviceId) + deltaMs)
    }

    func resetCaptureAudioSync(deviceId: String) {
        setCaptureAudioSyncOffset(deviceId: deviceId, offsetMs: 0)
    }

    /// Windows SetCaptureAudioSyncOffsetAsync: clamp, record, send the device
    /// command, report the outcome in the status line; then the full-state
    /// sources list so a paired microphone follows.
    func setCaptureAudioSyncOffset(deviceId: String, offsetMs: Int) {
        guard let device = captureDevices.first(where: { $0.id == deviceId }) else { return }
        let clamped = CaptureAudioSyncPolicy.clamp(offsetMs)
        captureAudioSyncOffsets[deviceId] = clamped == 0 ? nil : clamped
        syncCaptureAudio()
        guard bridge != nil || captureRequestForTesting != nil else { return }
        Task {
            do {
                _ = try await captureRequestForSync(CoreCommands.setCaptureAudioSyncOffset(
                    deviceId: deviceId, offsetMs: clamped))
                takeStatus = "\(device.name) audio sync set to \(clamped) ms"
            } catch {
                takeStatus = "\(device.name) audio sync not applied: \(error.localizedDescription)"
            }
        }
    }

    /// Review #5: sends the device copy to every listed device whose reported
    /// offset differs from the operator's (a respawned core reports 0 once its
    /// camera is enumerated). One request per device while in flight.
    func reconcileCaptureAudioSyncOffsets(_ reported: [JSONObject]) {
        guard bridge != nil || captureRequestForTesting != nil else { return }
        let pending = CaptureAudioSyncPolicy.pendingDeviceCopies(
            desired: captureAudioSyncOffsets, reported: reported, inFlight: captureSyncInFlight)
        for (deviceId, ms) in pending {
            captureSyncInFlight.insert(deviceId)
            Task {
                _ = try? await captureRequestForSync(CoreCommands.setCaptureAudioSyncOffset(
                    deviceId: deviceId, offsetMs: ms))
                captureSyncInFlight.remove(deviceId)
            }
        }
    }
}
