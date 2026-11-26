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
//                   through the core alias (core/AudioControlSourcePolicy.h, T1.6),
//                   while Program or Preview carries a fixed media route (Windows
//                   ResolveSceneMediaAudioSourceIds).
// and nothing else: the core's echo supplies measurements only (see mergeStrips).
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

/// The Zoom -> program audio topology (A1). A line-for-line port of Windows
/// ZoomAudioModePreference (prefs v9/v10): PARSE is permissive and falls back to
/// the product default, perGuestIso, for anything it cannot read; `persisted`
/// is a total switch so a third topology cannot be written as an old value.
enum ZoomAudioMode: Equatable {
    case programMix, perGuestIso

    static let programMixValue = "programMix"
    static let perGuestIsoValue = "perGuestIso"

    static func parse(_ persisted: String?) -> ZoomAudioMode {
        persisted?.trimmingCharacters(in: .whitespacesAndNewlines)
            .caseInsensitiveCompare(programMixValue) == .orderedSame ? .programMix : .perGuestIso
    }

    var persisted: String {
        switch self {
        case .programMix: return Self.programMixValue
        case .perGuestIso: return Self.perGuestIsoValue
        }
    }

    /// Windows SetZoomAudioMode's CommandStatus sentences, verbatim.
    var statusSentence: String {
        switch self {
        case .perGuestIso:
            return "Per-guest ISO audio: each guest routes to program through their own fader (Zoom's combined mix is off program)."
        case .programMix:
            return "Zoom program mix: Zoom's combined, echo-cancelled mix routes to program; guest strips meter only."
        }
    }
}

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

    /// A1: a guest stem's strip is meter-only while Zoom program mix is on
    /// (its PCM reaches program inside zoom-mix, not through this fader).
    static func isMeterOnly(_ id: String, mode: ZoomAudioMode) -> Bool {
        mode == .programMix && isZoomParticipantChannel(id)
    }

    static func isMediaClipId(_ id: String) -> Bool {
        id.hasPrefix("media:") && id.count > "media:".count
    }

    /// Zoom audio ids in ROSTER order (Windows BuildAudioMixChannels runs over the
    /// room roster). The source set arrives in BUDGET order — Program first, then
    /// Preview — so using it directly reordered the A1's faders on every cue and
    /// Take. Ids missing from the roster keep their relative order at the end.
    static func rosterOrdered(_ ids: [String], roster: [String]) -> [String] {
        let rank = Dictionary(roster.enumerated().map { ($0.element, $0.offset) },
                              uniquingKeysWith: { first, _ in first })
        return ids.enumerated().sorted { lhs, rhs in
            let l = rank[lhs.element] ?? Int.max, r = rank[rhs.element] ?? Int.max
            return l != r ? l < r : lhs.offset < rhs.offset
        }.map(\.element)
    }

    /// The strips the console must have, in console order.
    /// `mediaStrip`: Program or Preview carries a fixed media route (Windows
    /// ResolveSceneMediaAudioSourceIds over both buses' routes).
    static func expectedSourceIds(meetingAudio: Bool, zoomAudioIds: [String],
                                  captureAudioIds: [String], mediaStrip: Bool) -> [String] {
        var ids: [String] = []
        if meetingAudio { ids.append(meetingMixId) }
        ids.append(contentsOf: zoomAudioIds)
        ids.append(contentsOf: captureAudioIds)
        if mediaStrip { ids.append(mediaId) }
        var seen: Set<String> = []
        return ids.filter { !$0.isEmpty && seen.insert($0).inserted }
    }

    /// The WHOLE echo -> strip merge (the pure-function-over-the-whole-decision
    /// shape CLAUDE.md asks for after #481: a test can construct the regression).
    /// `remembered`: the A1's settings per id for the whole MEETING (Windows
    /// RememberMixerChannelSettings). A source that leaves the source set for one
    /// push — moved off the wall and back, a slot swap, a slot briefly offline —
    /// must come back with its mute and fader, never live at unity because of a
    /// video-side action.
    /// `zoomMuted`: roster ids muted IN ZOOM, resolved fresh by the caller every
    /// merge (Windows SVM:8818-8825). It lands only in `sourceMuted` (display and
    /// meter), never in the A1's `muted` and never carried from `prior` (#481).
    static func mergeStrips(echo: [JSONObject], prior: [AudioStrip],
                            remembered: [String: AudioStrip] = [:],
                            expected: [String], zoomMuted: Set<String> = []) -> [AudioStrip] {
        var echoById: [String: JSONObject] = [:]
        for entry in echo {
            guard let id = entry["participantId"] as? String, !id.isEmpty,
                  echoById[id] == nil else { continue }
            echoById[id] = entry
        }
        // The strip set is EXACTLY the modelled sources. Echoed ids outside it are
        // never strips (review M1): after the first sync the core echoes back only
        // the channels the shell sent, so an echo pass-through could only re-admit
        // ids the shell itself had synced — an unpaired mic or the meeting's
        // zoom-mix stayed on the console forever. A Zoom non-source is not on the
        // mixer (MixerChannelSetPolicy) and a media:<id> clip is governed by the
        // "media" strip; unmodelled PCM (background:<id>, local audio) has no
        // route either, so a strip would not make it audible.
        let ids = expected
        let priorById = Dictionary(prior.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        return ids.map { id in
            let entry = echoById[id]
            // Shell-owned state: the prior strip's, else a fresh unmuted unity strip.
            var strip = priorById[id] ?? remembered[id] ?? AudioStrip(
                id: id, outputLevel: 0, manualGainDb: 0, muted: false, solo: false,
                pan: 0, status: "waiting-for-pcm")
            // Measurements only.
            strip.outputLevel = entry?["outputLevel"] as? Int ?? 0
            strip.status = entry?["status"] as? String ?? "waiting-for-pcm"
            strip.inputLevel = inputLevel(entry)
            strip.gainReductionDb = (entry?["gainReductionDb"] as? NSNumber)?.doubleValue ?? 0
            strip.sourceMuted = zoomMuted.contains(id)
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
    /// `mediaRouted`: PROGRAM carries a fixed media route — Windows seeds the
    /// "media" sends only then (EnsureDefaultMediaAudioRoutingSends gated on
    /// ResolveSceneMediaAudioSourceIds(programRoutes)), so a clip cued only on
    /// Preview is never audible on master.
    ///
    /// `zoomAudioMode` (A1, Windows EnsureDefaultZoomAudioRoutingSends): in
    /// PerGuestIso the stems get the defaults and, AFTER the operator's edits, the
    /// doubling guard drops every zoom-mix send to a default bus — even one the
    /// operator enabled, exactly as Windows removes it. In ProgramMix zoom-mix gets
    /// the defaults (only while the meeting mix is subscribed: no strip, no route),
    /// stems get none, and an operator's own stem crosspoint survives.
    ///
    /// `busInserts` (A7): each bus's processing chain, keyed by bus id. It is
    /// attached to EVERY send of that bus, AFTER the defaults, the operator's
    /// edits and the doubling guard (Windows SVM:9222-9230: "Default/fallback
    /// sends are synthesized after the matrix rows above. Attach each bus's
    /// insert chain here so MASTER VST processing remains live even when a
    /// source is using an implicit default route."). The core takes a bus's
    /// chain from any send to it that carries one (MediaCore.cpp
    /// applyOrderedBusInserts), so a bus with no send has no chain on the wire.
    static func baseRoutingSends(
        zoomAudioIds: [String], captureAudioIds: [String], mediaRouted: Bool,
        operatorEdits: [AudioRouteKey: AudioRouteControlProjection.Draft],
        zoomAudioMode: ZoomAudioMode = .perGuestIso, meetingAudio: Bool = false,
        busInserts: [String: [String]] = [:]
    ) -> [JSONObject] {
        var matrix: [AudioRouteKey: Double] = [:]
        let zoomDefaults: [String]
        switch zoomAudioMode {
        case .perGuestIso: zoomDefaults = zoomAudioIds
        case .programMix: zoomDefaults = meetingAudio ? [meetingMixId] : []
        }
        for source in zoomDefaults + captureAudioIds + (mediaRouted ? [mediaId] : [])
            where !source.isEmpty {
            for bus in defaultSendBuses {
                matrix[AudioRouteKey(source: source, bus: bus)] = 0
            }
        }
        for (key, edit) in operatorEdits {
            if edit.enabled { matrix[key] = max(-60, min(10, edit.gainDb)) } else { matrix[key] = nil }
        }
        if zoomAudioMode == .perGuestIso {
            for bus in defaultSendBuses { matrix[AudioRouteKey(source: meetingMixId, bus: bus)] = nil }
        }
        return matrix.keys.sorted().map { key in
            ["sourceId": key.source, "busId": key.bus, "gainDb": matrix[key] ?? 0,
             "busPluginInserts": busInserts[key.bus] ?? []]
        }
    }

    /// Windows AudioMeterScale.ToLevel: a calibrated -60..0 dBFS -> 0..100 scale.
    static func meterLevel(dbfs: Double) -> Int { AudioMeterScale.toLevel(dbfs) }

    /// The pre-mute input level from an echo entry: the core's measured
    /// `inputPeakDbfs` (published per channel once the console is synced, #481),
    /// else the native mixer's own 0..100 `inputLevel` (before any sync).
    static func inputLevel(_ entry: JSONObject?) -> Int {
        guard let entry else { return 0 }
        if let peak = (entry["inputPeakDbfs"] as? NSNumber)?.doubleValue {
            return meterLevel(dbfs: peak)
        }
        return max(0, min(100, entry["inputLevel"] as? Int ?? 0))
    }

    /// What a strip's meter shows (Windows ResolveChannelMeterLevel +
    /// ShowLevelWhileMuted): live, the output level; muted, the PRE-MUTE input
    /// level, dimmed. A muted strip used to read silence even while the guest
    /// talked — the A1 could not see who was speaking into a muted channel.
    /// Windows ResolveChannelMeterLevel(…, Muted || SourceMuted): a guest muted
    /// in Zoom also shows the pre-mute input, dimmed (A4, D6).
    static func meterReading(_ strip: AudioStrip) -> (level: Int, dimmed: Bool) {
        (strip.muted || strip.sourceMuted) ? (strip.inputLevel, true) : (strip.outputLevel, false)
    }
}
