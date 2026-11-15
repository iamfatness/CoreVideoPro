// AudioConsolePolicy — which channel strips the Mac audio console has, and how a
// strip's state is merged with the core's echo. Pure: no model, no I/O.
//
// THE FADER LAW (owner rule, CLAUDE.md): no audio source reaches any bus without
// a strip. Once the shell syncs any channel, the core DROPS a routed source that
// has no strip (MediaCore.cpp, routed-source build). The Mac used to build its
// strips ONLY from the core's echo (audioMixSession.participants) — and after the
// first channel sync the core echoes back only the channels it was sent
// (CoreStateCapture::audioMixSessionState). So a guest who started talking after
// the operator's first fader/mute/insert edit never got a strip, was dropped from
// every bus, and never appeared on the console.
//
// Windows builds the strip set from what the shell itself knows
// (StudioViewModel.BuildExpectedAudioSourceIds + MixerChannelSetPolicy). This is
// that port, scaled to the Mac model:
//   - "zoom-mix"  — the Zoom program mix, whenever meeting-audio is subscribed
//                   ("Zoom program mix" fader; it is THE audible path in the
//                   program-mix topology and must never lack a fader);
//   - every Zoom audio SOURCE (the participant-audio set, camera-off included);
//   - "capture:<id>" for every capture slot with a paired microphone;
//   - "media"     — the ONE media strip that governs every media:<assetId> clip
//                   through the core alias (core/AudioControlSourcePolicy.h, T1.6).
//                   Always present, like Windows' always-present Media row, so a
//                   clip that starts later is never strip-less.
// plus anything else the core echoes (a PCM source the shell does not model yet),
// EXCEPT a Zoom participant channel outside the source set (Windows
// MixerChannelSetPolicy: non-sources are not on the mixer) and raw media:<id>
// clip ids (they are governed by the "media" strip, never a strip of their own —
// a per-clip strip would bypass the media pre-sum).
//
// THE A1's MUTE IS ONLY SET BY THE A1 (#481). A strip's mute, gain, pan and solo
// are SHELL-OWNED: a new strip starts unmuted at unity, and an existing strip keeps
// its own values. They are never adopted from the echo. The echo's `muted` is not
// the A1's mute: before any channel sync it is the native mixer's voice-activity
// flag (AudioDsp.h: metrics.muted = !frame.voiceActive — every quiet guest reads
// "muted"), and after a core restart it is the fresh core's default "unmuted",
// which silently erased the A1's mutes. Only measurements (levels, status) come
// from the echo.

import Foundation

enum AudioConsolePolicy {
    static let meetingMixId = "zoom-mix"
    static let mediaId = "media"
    /// The buses every default send covers (Windows EnsureDefault*AudioRoutingSends).
    static let defaultSendBuses = ["master", "pgm-l", "pgm-r", "stream", "mon"]

    /// Windows MixerChannelSetPolicy.IsZoomParticipantChannel: a bare id (no
    /// scheme) that is not one of the shell's named sources.
    static func isZoomParticipantChannel(_ id: String) -> Bool {
        !id.isEmpty && !id.contains(":")
            && id != meetingMixId && id != "local-machine-audio" && id != mediaId
    }

    static func isMediaClipId(_ id: String) -> Bool {
        id.hasPrefix("media:") && id.count > "media:".count
    }

    /// The strips the console must have, in console order.
    static func expectedSourceIds(meetingAudio: Bool, zoomAudioIds: [String],
                                  captureAudioIds: [String]) -> [String] {
        var ids: [String] = []
        if meetingAudio { ids.append(meetingMixId) }
        ids.append(contentsOf: zoomAudioIds)
        ids.append(contentsOf: captureAudioIds)
        ids.append(mediaId)
        var seen: Set<String> = []
        return ids.filter { !$0.isEmpty && seen.insert($0).inserted }
    }

    /// The WHOLE echo -> strip merge (the pure-function-over-the-whole-decision
    /// shape CLAUDE.md asks for after #481: a test can construct the regression).
    static func mergeStrips(echo: [JSONObject], prior: [AudioStrip],
                            expected: [String]) -> [AudioStrip] {
        var echoById: [String: JSONObject] = [:]
        var echoOrder: [String] = []
        for entry in echo {
            guard let id = entry["participantId"] as? String, !id.isEmpty,
                  echoById[id] == nil else { continue }
            echoById[id] = entry
            echoOrder.append(id)
        }
        let expectedSet = Set(expected)
        var ids = expected
        for id in echoOrder where !expectedSet.contains(id) {
            if isZoomParticipantChannel(id) || isMediaClipId(id) { continue }
            ids.append(id)
        }
        let priorById = Dictionary(prior.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        return ids.map { id in
            let entry = echoById[id]
            // Shell-owned state: the prior strip's, else a fresh unmuted unity strip.
            var strip = priorById[id] ?? AudioStrip(
                id: id, outputLevel: 0, manualGainDb: 0, muted: false, solo: false,
                pan: 0, status: "waiting-for-pcm")
            // Measurements only.
            strip.outputLevel = entry?["outputLevel"] as? Int ?? 0
            strip.status = entry?["status"] as? String ?? "waiting-for-pcm"
            return strip
        }
    }

    /// The BASE routing matrix the shell sends (sync-audio-routing-matrix): the
    /// Windows defaults (EnsureDefaultZoomAudioRoutingSends in its default
    /// PerGuestIso topology, EnsureDefaultCaptureAudioRoutingSends,
    /// EnsureDefaultMediaAudioRoutingSends) with the operator's own crosspoint
    /// edits applied on top.
    ///
    /// - every Zoom audio source (its isolated stem, through its own strip),
    ///   every paired capture mic and the "media" strip -> master, pgm-l, pgm-r,
    ///   stream, mon at 0 dB;
    /// - zoom-mix gets NO default send: summing the meeting mix with the stems
    ///   doubles every voice (~110 ms skew, the measured internal echo). Its strip
    ///   exists; routing it is a deliberate operator choice.
    ///
    /// A fresh Mac console used to route nothing. Once anything is routed the
    /// core stops falling back to the raw unmuted sum (MediaCore: "a routed console
    /// never falls back"), so every source the shell knows must be in this base or
    /// it is silent. Carrying the operator's edits in the base (not only in the
    /// revisioned set-audio-route-control overrides, which a core restart loses)
    /// is what lets a respawned core get the operator's routing back.
    static func baseRoutingSends(
        zoomAudioIds: [String], captureAudioIds: [String],
        operatorEdits: [AudioRouteKey: AudioRouteControlProjection.Draft]
    ) -> [JSONObject] {
        var matrix: [AudioRouteKey: Double] = [:]
        for source in zoomAudioIds + captureAudioIds + [mediaId] where !source.isEmpty {
            for bus in defaultSendBuses {
                matrix[AudioRouteKey(source: source, bus: bus)] = 0
            }
        }
        for (key, edit) in operatorEdits {
            if edit.enabled { matrix[key] = max(-60, min(10, edit.gainDb)) } else { matrix[key] = nil }
        }
        return matrix.keys.sorted().map { key in
            ["sourceId": key.source, "busId": key.bus, "gainDb": matrix[key] ?? 0,
             "busPluginInserts": [] as [String]]
        }
    }
}
