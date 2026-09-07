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
        // FULL STATE, including capture slots with NO microphone: the core
        // clears its map from this command, so omitting an unpaired source is
        // the same as sending it — but sending it explicitly keeps the audio
        // kind honest ("none") instead of leaving a stale pairing behind.
        let sources: [JSONObject] = slots
            .filter { $0.kind == "capture" && !$0.sourceId.isEmpty }
            .map { slot in
                var entry: JSONObject = ["captureDeviceId": slot.sourceId]
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
        _ = try? await bridge.request([
            "type": "media-core-sync", "elapsedMs": elapsedMs(),
            "commands": [["type": "sync-capture-audio-sources", "sources": sources]],
        ])
    }

    func pairAudio(slotId: Int, device: AudioInputDevice?) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        slots[index].audioDeviceId = device?.id ?? ""
        slots[index].audioDeviceName = device?.name ?? ""
        syncCaptureAudio()
    }

    /// Re-assert pairings after a reconnect: the core rebuilds its map from
    /// scratch each launch, so a persisted pairing that is never pushed does
    /// nothing — the same trap the colour grade had.
    func reassertCaptureAudio() {
        if slots.contains(where: { $0.kind == "capture" && !$0.audioDeviceId.isEmpty }) {
            syncCaptureAudio()
        }
    }
}
