// Per-source microphone pairing — P0 #2 of docs/mac-parity-plan.md.
//
// A capture card's VIDEO and its AUDIO are separate devices. Windows has a
// "Pair a microphone" dropdown on every assigned input row; macOS had none
// (`audioDeviceId` appeared zero times in the shell), so capture sources were
// silent and their ISO recordings were video-only.
//
// This is not cosmetic. `MediaCore::isoSourceHasAudio` decides whether a capture
// ISO gets an AAC track by looking for exactly this pairing in
// captureAudioSources_ — no pairing means a deliberately video-only ISO, which
// is correct behaviour for a bare camera and wrong for an Elgato carrying
// embedded audio.
//
// The core takes the pairing through `sync-capture-audio-sources` (FULL STATE:
// the whole set every time, keyed by captureDeviceId) and keys the resulting PCM
// as `capture:<captureDeviceId>` — the SAME id as the video — which is what
// makes the audio land on the right channel strip and in the right ISO.

import AVFoundation
import Foundation

struct AudioInputDevice: Identifiable, Equatable {
    let id: String       // uniqueID, the stable key we persist and send
    let name: String
}

enum AudioInputs {
    /// Audio capture devices macOS can see.
    ///
    /// Uses AVCaptureDevice rather than raw CoreAudio because capture cards
    /// (Elgato and friends) expose their embedded audio as an AVCaptureDevice
    /// alongside their video, which is exactly the pairing an operator wants —
    /// and it is the same API the video side already enumerates through.
    ///
    /// NOTE ON PERMISSIONS: listing does not require microphone consent, but
    /// CAPTURING does. The core requests that when it actually opens the device;
    /// this list is deliberately free of side effects so opening the Sources tab
    /// never triggers a TCC prompt.
    static func available() -> [AudioInputDevice] {
        // `.microphone`/`.external` are macOS 14+ spellings; the deployment
        // target is 13, so use the names that exist there. DiscoverySession
        // rather than the deprecated devices(for:).
        let session = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInMicrophone, .externalUnknown],
            mediaType: .audio, position: .unspecified)
        return session.devices.map { AudioInputDevice(id: $0.uniqueID, name: $0.localizedName) }
    }

    /// Calls `changed` on the main queue whenever an AVCaptureDevice is connected
    /// or disconnected (the same notifications for audio and video devices).
    static func watch(_ changed: @escaping @MainActor () -> Void) {
        for name in [AVCaptureDevice.wasConnectedNotification, AVCaptureDevice.wasDisconnectedNotification] {
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { changed() }
            }
        }
    }
}

extension AppModel {
    /// Full-state push of every capture→microphone pairing.
    ///
    /// COALESCED: the picker fires per selection and a row rebuild can fire
    /// several at once; one request per change is the per-delta pattern that
    /// starved the command queue until every command timed out.
    func syncCaptureAudio() {
        captureAudioSyncTask?.cancel()
        captureAudioSyncTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard !Task.isCancelled else { return }
            await self?.pushCaptureAudio()
        }
    }

    @MainActor
    func pushCaptureAudio() async {
        guard let bridge else { return }
        _ = try? await bridge.request([
            "type": "media-core-sync", "elapsedMs": elapsedMs(),
            "commands": [captureAudioSourcesCommand()],
        ])
    }

    /// The full-state sync-capture-audio-sources command. Internal: the core
    /// restart path sends it in the SAME request as, and after, the console mix.
    @MainActor
    func captureAudioSourcesCommand() -> JSONObject {
        // FULL STATE, including capture slots with NO microphone: the core
        // clears its map from this command, so omitting an unpaired source is
        // the same as sending it — but sending it explicitly keeps the audio
        // kind honest ("none") instead of leaving a stale pairing behind.
        let sources: [JSONObject] = slots
            .filter { $0.kind == "capture" && !$0.sourceId.isEmpty }
            .map { slot in
                // Z2: the paired microphone's copy of the sync offset (always
                // sent, as Windows does, so a reset to 0 reaches the core).
                var entry: JSONObject = ["captureDeviceId": slot.sourceId,
                                         "audioSyncOffsetMs": captureAudioSyncOffset(slot.sourceId)]
                if !slot.audioDeviceId.isEmpty {
                    entry["audioDeviceId"] = slot.audioDeviceId
                    entry["audioDeviceName"] = slot.audioDeviceName
                    // The core defaults the kind from whether an id is present;
                    // name it anyway so a snapshot reads unambiguously.
                    entry["audioSourceKind"] = "coreaudio-input"
                    entry["nativeAudioDeviceId"] = slot.audioDeviceId
                } else {
                    entry["audioSourceKind"] = "none"
                }
                return entry
            }
            // A5: the local audio source rides the same full-state list.
            + [LocalAudioPolicy.entry(localAudio, devices: audioInputs)].compactMap { $0 }
        return ["type": "sync-capture-audio-sources", "sources": sources]
    }

    func pairAudio(slotId: Int, device: AudioInputDevice?) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        slots[index].audioDeviceId = device?.id ?? ""
        slots[index].audioDeviceName = device?.name ?? ""
        syncCaptureAudio()
        refreshStrips()  // a paired mic is a source: it needs its strip (FADER LAW)
        pushRoutingBase()  // ...and its default sends
    }

    /// Re-lists the audio inputs. `push`: a device arrived or left while the
    /// core runs — if that changes whether the local audio source can be sent,
    /// re-send the capture sources, the strip set and the routing base (a mic
    /// plugged in after launch used to stay "not connected" forever).
    func refreshAudioInputs(push: Bool) {
        let wasConfigured = LocalAudioPolicy.isConfigured(localAudio, devices: audioInputs)
        audioInputs = audioInputsProvider()
        guard push, LocalAudioPolicy.isConfigured(localAudio, devices: audioInputs) != wasConfigured else { return }
        syncCaptureAudio()
        refreshStrips()
        pushRoutingBase()
    }
}
