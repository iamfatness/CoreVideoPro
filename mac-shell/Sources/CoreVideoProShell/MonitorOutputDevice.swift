// Monitor output device (Tier 0.12; Windows "Monitor output" → "Output device",
// ProductionSettingsWindow.xaml:594-600 and AudioPage.xaml:121-125, bound to
// StudioViewModel.SelectedAudioMonitorDeviceId, persisted in
// production-output-preferences.json).
//
// The Mac always monitored on the system default output, so putting headphones
// on a separate interface meant moving every other app's audio (Zoom's
// included) to it. The core already opens a monitor device by its CoreAudio
// UID (native CoreAudioMonitorOutputAdapter.cpp resolveDevice: UID or name,
// else the default with a warning); the shell never sent one (`deviceId: ""`).
//
// The choice is persisted by UID — stable across reboots and re-plugs, unlike
// the AudioDeviceID. A chosen device that is not connected falls back to the
// system default output LOUDLY (a line under the picker + the warning strip),
// and the choice is kept so plugging the interface back in restores it. This
// differs from Windows, which overwrites the saved choice with the default
// when the device disappears (StudioViewModel.ApplyDiscoveredAudioRenderDevices).

import CoreAudio
import Foundation
import SwiftUI

struct MonitorOutputDevice: Identifiable, Equatable {
    let id: String     // CoreAudio device UID — what is persisted and sent
    let name: String
}

enum MonitorDevicePolicy {
    /// The Windows pseudo-entry's name (AudioRenderDeviceDiscoveryService.cs:49),
    /// and what the core itself names an empty device id (MediaCore.cpp).
    static let systemDefaultName = "System default output"

    struct Resolution: Equatable {
        let deviceId: String       // "" = the system default output
        let deviceName: String
        let fallbackWarning: String?
    }

    /// What the core should open right now. `devices` nil = not enumerated yet
    /// (the core resolves the UID itself and falls back on its own).
    static func resolve(chosen: String, chosenName: String,
                        devices: [MonitorOutputDevice]?) -> Resolution {
        guard !chosen.isEmpty else {
            return Resolution(deviceId: "", deviceName: systemDefaultName, fallbackWarning: nil)
        }
        guard let devices else {
            return Resolution(deviceId: chosen,
                              deviceName: chosenName.isEmpty ? chosen : chosenName,
                              fallbackWarning: nil)
        }
        if let device = devices.first(where: { $0.id == chosen }) {
            return Resolution(deviceId: device.id, deviceName: device.name, fallbackWarning: nil)
        }
        return Resolution(deviceId: "", deviceName: systemDefaultName,
                          fallbackWarning: fallbackWarning(chosenName.isEmpty ? chosen : chosenName))
    }

    static func fallbackWarning(_ name: String) -> String {
        "Monitor device \(name) is not connected. Monitoring through the "
            + "\(systemDefaultName.lowercased()) until it is."
    }

    /// Windows StudioViewModel.AudioMonitorStatus.
    static func status(enabled: Bool, deviceName: String) -> String {
        enabled ? "Monitor target - \(deviceName)" : "Monitor muted"
    }
}

enum MonitorOutputDevices {
    /// Every CoreAudio device with at least one output channel, by name.
    /// Read-only: listing opens nothing.
    static func available() -> [MonitorOutputDevice] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),
                                             &address, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                         &address, 0, nil, &size, &ids) == noErr else { return [] }
        return ids.compactMap { id -> MonitorOutputDevice? in
            guard outputChannels(id) > 0,
                  let uid = stringProperty(id, kAudioDevicePropertyDeviceUID), !uid.isEmpty
            else { return nil }
            return MonitorOutputDevice(id: uid, name: stringProperty(id, kAudioObjectPropertyName) ?? uid)
        }
        .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    /// Calls `onChange` on the main queue whenever a device is added or
    /// removed (an interface unplugged mid-show). Installed once per process.
    static func watch(_ onChange: @escaping () -> Void) {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject),
                                            &address, DispatchQueue.main) { _, _ in onChange() }
    }

    private static func outputChannels(_ id: AudioDeviceID) -> Int {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &address, 0, nil, &size) == noErr, size > 0 else {
            return 0
        }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size),
                                                   alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(id, &address, 0, nil, &size, raw) == noErr else { return 0 }
        let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return list.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    private static func stringProperty(_ id: AudioDeviceID,
                                       _ selector: AudioObjectPropertySelector) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &address, 0, nil, &size, &value) == noErr,
              let value else { return nil }
        return value.takeRetainedValue() as String
    }
}

extension AppModel {
    /// The device the monitor uses right now (the operator's, or the default
    /// when theirs is unplugged). Updates the Settings line as a side effect.
    func monitorDeviceTarget() -> MonitorDevicePolicy.Resolution {
        let resolution = MonitorDevicePolicy.resolve(chosen: monitorDeviceChoice,
                                                     chosenName: monitorDeviceChoiceName,
                                                     devices: monitorOutputDevices)
        let warning = resolution.fallbackWarning ?? ""
        if warning != monitorDeviceWarning {
            monitorDeviceWarning = warning
            if !warning.isEmpty { pushWarning("audio: \(warning)") }
        }
        return resolution
    }

    /// Windows AudioMonitorStatus ("Monitor target - <device>" / "Monitor muted").
    var monitorTargetStatus: String {
        MonitorDevicePolicy.status(
            enabled: monitorEnabled,
            deviceName: MonitorDevicePolicy.resolve(chosen: monitorDeviceChoice,
                                                    chosenName: monitorDeviceChoiceName,
                                                    devices: monitorOutputDevices).deviceName)
    }

    /// The picker: "" = System default output, else a device UID.
    func setMonitorDevice(_ deviceId: String) {
        monitorDeviceChoice = deviceId
        monitorDeviceChoiceName = monitorOutputDevices?.first { $0.id == deviceId }?.name ?? ""
        setMonitor(enabled: monitorEnabled, volume: monitorVolume)
    }

    /// A new device list (launch, every core generation, hot-plug). Re-sends
    /// the monitor when the device it should use changed: the chosen
    /// interface unplugged (fall back) or plugged back in (restore).
    func applyMonitorOutputDevices(_ devices: [MonitorOutputDevice]) {
        let before = MonitorDevicePolicy.resolve(chosen: monitorDeviceChoice,
                                                 chosenName: monitorDeviceChoiceName,
                                                 devices: monitorOutputDevices)
        monitorOutputDevices = devices
        if let name = devices.first(where: { $0.id == monitorDeviceChoice })?.name {
            monitorDeviceChoiceName = name
        }
        let after = monitorDeviceTarget()
        if after.deviceId != before.deviceId { setMonitor(enabled: monitorEnabled, volume: monitorVolume) }
    }

    func refreshMonitorOutputDevices() {
        applyMonitorOutputDevices(MonitorOutputDevices.available())
    }
}

/// The Windows "Output device" combo (Settings) / "Device" (Audio SETUP):
/// System default output, then every output device. A chosen device that is
/// not connected stays listed (marked) so the operator can see what is saved,
/// with the fallback line under it.
struct MonitorDevicePicker: View {
    @EnvironmentObject var model: AppModel
    var width: CGFloat = 260

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Picker("", selection: Binding(
                get: { model.monitorDeviceChoice },
                set: { model.setMonitorDevice($0) })) {
                Text(MonitorDevicePolicy.systemDefaultName).tag("")
                ForEach(model.monitorOutputDevices ?? []) { device in
                    Text(device.name).tag(device.id)
                }
                if !model.monitorDeviceChoice.isEmpty,
                   !(model.monitorOutputDevices ?? []).contains(where: { $0.id == model.monitorDeviceChoice }) {
                    Text("\(model.monitorDeviceChoiceName.isEmpty ? model.monitorDeviceChoice : model.monitorDeviceChoiceName) (not connected)")
                        .tag(model.monitorDeviceChoice)
                }
            }
            .labelsHidden()
            .frame(width: width)
            if !model.monitorDeviceWarning.isEmpty {
                Text(model.monitorDeviceWarning)
                    .font(.grotesk(11)).foregroundStyle(Studio.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
