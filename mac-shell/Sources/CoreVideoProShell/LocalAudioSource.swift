// LocalAudioSource — A5 of the audio parity spec (§4.3): a microphone or audio
// interface input that is its own console source, not paired with a capture card.
//
// Windows: Audio > SETUP > "Local audio source" (AudioPage.xaml:158-176), state
// LocalAudioSourceEnabled + SelectedLocalAudioCaptureDeviceId, wire entry built
// in StudioViewModel.cs:9193-9207, status ResolveLocalAudioOperatorStatus
// (:8471-8499), default sends EnsureDefaultLocalAudioRoutingSends (:9567-9590).
//
// The core keys this PCM as `local-machine-audio` (CoreAudioCaptureSourceAdapter
// participantIdForSource: the one capture id that is NOT prefixed `capture:`),
// so the strip, the five default sends and the wire entry all use that id.
// System audio (a Core Audio process tap, A8) is not this slice: a loopback
// device such as BlackHole can be chosen here as an ordinary input.

import Foundation
import SwiftUI

struct LocalAudioSetting: Equatable {
    var enabled = false
    var deviceId = ""
    var deviceName = ""
}

enum LocalAudioPolicy {
    static let sourceId = "local-machine-audio"

    /// The sync-capture-audio-sources entry, or nil unless the source is enabled
    /// AND its device is connected (Windows ResolveSelectedLocalAudioCaptureDevice
    /// returns null for an absent device, so nothing is sent for it).
    static func entry(_ setting: LocalAudioSetting, devices: [AudioInputDevice],
                      audioSyncOffsetMs: Int = 0) -> JSONObject? {
        guard setting.enabled, let device = devices.first(where: { $0.id == setting.deviceId }) else {
            return nil
        }
        var entry: JSONObject = [
            "captureDeviceId": sourceId, "audioDeviceId": device.id, "audioDeviceName": device.name,
            "audioSourceKind": "coreaudio-input", "nativeAudioDeviceId": device.id,
        ]
        if audioSyncOffsetMs != 0 { entry["audioSyncOffsetMs"] = audioSyncOffsetMs }
        return entry
    }

    /// Windows IsLocalAudioSourceConfigured: the source is a console source only
    /// while it would actually be sent.
    static func isConfigured(_ setting: LocalAudioSetting, devices: [AudioInputDevice]) -> Bool {
        entry(setting, devices: devices) != nil
    }

    /// Windows LocalAudioOperatorStatus / ResolveLocalAudioOperatorStatus, plus
    /// the Mac monitor picker's rule for a saved device that is not connected.
    /// `captureAudioSources`: the core's `captureAudioSources.sources[]` rows —
    /// the status is read from what the core reports, never from intent.
    static func status(_ setting: LocalAudioSetting, devices: [AudioInputDevice],
                       captureAudioSources: [JSONObject]) -> String {
        guard setting.enabled else { return "Local audio source off" }
        guard !setting.deviceId.isEmpty else { return "Choose an audio input" }
        guard devices.contains(where: { $0.id == setting.deviceId }) else {
            let name = setting.deviceName.isEmpty ? setting.deviceId : setting.deviceName
            return "\(name) is not connected"
        }
        guard let row = captureAudioSources.first(where: {
            ($0["captureDeviceId"] as? String) == sourceId
        }) else { return "Waiting for audio" }
        if (row["signalPresent"] as? Bool) == true { return "Audio detected" }
        if (row["captureStreaming"] as? Bool) != true { return "Starting audio source" }
        return "No audio detected"
    }

    /// The core's rows, tolerant of the node being absent or of either shape.
    static func captureAudioRows(_ snapshot: JSONObject) -> [JSONObject] {
        if let node = snapshot["captureAudioSources"] as? JSONObject {
            return node["sources"] as? [JSONObject] ?? []
        }
        return snapshot["captureAudioSources"] as? [JSONObject] ?? []
    }
}

extension AppModel {
    /// A5 entry point for the SETUP card. Persists, then re-sends the full
    /// capture-audio state, the strip set and the routing base (a configured
    /// local source is a console source: strip + five default sends).
    func setLocalAudio(enabled: Bool? = nil, device: AudioInputDevice? = nil) {
        if let enabled { localAudio.enabled = enabled }
        if let device {
            localAudio.deviceId = device.id
            localAudio.deviceName = device.name
        }
        persistPrefsNow()
        syncCaptureAudio()
        refreshStrips()
        pushRoutingBase()
    }

    var localAudioStatus: String {
        LocalAudioPolicy.status(localAudio, devices: audioInputs,
                                captureAudioSources: LocalAudioPolicy.captureAudioRows(lastSnapshot))
    }
}

/// SETUP card (Windows AudioPage.xaml:151-180): title 12 semibold, status
/// 10 pt muted, the "Input / loopback" picker full width, then a full-width
/// "Source" toggle. AudioSetupSurface draws the card chrome around it (left
/// column, A7 Task 5). A saved device that is unplugged stays listed and marked.
struct LocalAudioSourceCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            VStack(alignment: .leading, spacing: 5) {
                Text("Local audio source").font(.grotesk(12, .semibold))
                Text(model.localAudioStatus).font(.plexMono(10)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            VStack(alignment: .leading, spacing: 3) {
                Text("Input / loopback").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                Picker("", selection: Binding(
                    get: { model.localAudio.deviceId },
                    set: { id in
                        if let device = model.audioInputs.first(where: { $0.id == id }) {
                            model.setLocalAudio(device: device)
                        }
                    })) {
                    if model.localAudio.deviceId.isEmpty {
                        Text("Choose an audio input").tag("")
                    } else if !model.audioInputs.contains(where: { $0.id == model.localAudio.deviceId }) {
                        Text("\(model.localAudio.deviceName.isEmpty ? model.localAudio.deviceId : model.localAudio.deviceName) (not connected)")
                            .tag(model.localAudio.deviceId)
                    }
                    ForEach(model.audioInputs) { device in Text(device.name).tag(device.id) }
                }
                .labelsHidden()
                .frame(maxWidth: .infinity)
                .accessibilityLabel("Local audio input")
            }
            SetupToggleButton(title: "Source", isOn: Binding(
                get: { model.localAudio.enabled },
                set: { model.setLocalAudio(enabled: $0) }))
        }
    }
}
