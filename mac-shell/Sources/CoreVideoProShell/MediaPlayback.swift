// MediaPlayback — the Mac ports of the Windows media DECISIONS
// (MediaRoutePlaybackService, MediaBinPlaybackProjection,
// MediaTransportGestureLedger, TakeMediaSelectionRollback) and the read models
// for what the core says about media. Pure types: no AppModel, no I/O, so each
// is tested on its own under its Windows test names.
//
// Play/pause/cue STATE lives in the core (core::MediaTransports, one entry per
// media:<assetId> / background:<assetId>). The shell sends scene membership and
// one-shot gestures, and READS state back from mediaSources[]; it never asserts
// a play state of its own on the wire.

import Foundation

/// One row of snapshot["mediaSources"] (CoreStateCapture.cpp; C# NativeMediaCoreMediaSource).
struct MediaSourceRow: Equatable {
    let sourceId: String
    let mediaAssetId: String?
    let state: String            // "cued" | "live" | "paused" | "ended"; unknown strings kept verbatim
    let loop: Bool
    let onProgram: Bool
    let onPreview: Bool
    let positionMs: Double
    let durationMs: Double       // -1 = unknown
    let decodedVideoFrames: Double?
    let lastDecodedAgeMs: Double?
    let presentedVideoFrames: Double?
    let lastPresentedAgeMs: Double?
    let videoQueued: Double?
    let decoderRestarts: Double?

    /// nil when the node is absent (an older core); [] when present and empty.
    /// Rows without a string sourceId or state are dropped.
    static func parse(_ snapshot: JSONObject) -> [MediaSourceRow]? {
        guard let rows = snapshot["mediaSources"] as? [Any] else { return nil }
        func number(_ value: Any?) -> Double? { (value as? NSNumber)?.doubleValue }
        return rows.compactMap { element -> MediaSourceRow? in
            guard let row = element as? JSONObject,
                  let sourceId = row["sourceId"] as? String,
                  let state = row["state"] as? String else { return nil }
            return MediaSourceRow(
                sourceId: sourceId,
                mediaAssetId: row["mediaAssetId"] as? String,
                state: state,
                loop: row["loop"] as? Bool ?? false,
                onProgram: row["onProgram"] as? Bool ?? false,
                onPreview: row["onPreview"] as? Bool ?? false,
                positionMs: number(row["positionMs"]) ?? -1,
                durationMs: number(row["durationMs"]) ?? -1,
                decodedVideoFrames: number(row["decodedVideoFrames"]),
                lastDecodedAgeMs: number(row["lastDecodedAgeMs"]),
                presentedVideoFrames: number(row["presentedVideoFrames"]),
                lastPresentedAgeMs: number(row["lastPresentedAgeMs"]),
                videoQueued: number(row["videoQueued"]),
                decoderRestarts: number(row["decoderRestarts"]))
        }
    }
}

/// snapshot["mediaPlayback"]: what the core says about the SELECTED asset.
struct MediaPlaybackReadModel: Equatable {
    let status: String
    let mediaAssetId: String?
    let playing: Bool
    let summary: String
    let warnings: [String]

    static func parse(_ snapshot: JSONObject) -> MediaPlaybackReadModel? {
        guard let node = snapshot["mediaPlayback"] as? JSONObject,
              let status = node["status"] as? String else { return nil }
        return MediaPlaybackReadModel(
            status: status,
            mediaAssetId: node["mediaAssetId"] as? String,
            playing: node["playing"] as? Bool ?? false,
            summary: node["summary"] as? String ?? "",
            warnings: node["warnings"] as? [String] ?? [])
    }
}

/// Windows MediaRoutePlaybackService, as pure functions over the route WIRE
/// (what buildRoutes emits) and the core's rows.
enum MediaRoutePlayback {
    /// Windows IsLoopingKind: only the "background" kind loops as a route. The
    /// Mac classifier never produces it, so every route asset plays once.
    static func isLoopingKind(_ kind: String?) -> Bool {
        (kind ?? "").lowercased() == "background"
    }

    /// Assets on fixed media routes, route order, de-duplicated (Windows ProgramMediaAssetIds).
    static func programMediaAssetIds(_ routes: [JSONObject]) -> [String] {
        var ids: [String] = []
        for route in routes where route["mode"] as? String == "fixed" {
            guard let id = route["mediaAssetId"] as? String, !id.isEmpty, !ids.contains(id) else { continue }
            ids.append(id)
        }
        return ids
    }

    /// Windows IsMediaAssetRoutedOnProgram. Works for any bus: pass that bus's routes.
    static func isRoutedOnProgram(_ assetId: String, programRoutes: [JSONObject]) -> Bool {
        !assetId.isEmpty && programMediaAssetIds(programRoutes).contains(assetId)
    }

    /// StudioViewModel.FormatSelectedMediaAssetSummary, verbatim. Program wins over Preview.
    static func selectedSummary(name: String?, isOnProgram: Bool, isCuedInPreview: Bool, playing: Bool) -> String {
        guard let name, !name.trimmingCharacters(in: .whitespaces).isEmpty else { return "No media asset selected" }
        if isOnProgram { return playing ? "Playing \(name) on Program" : "\(name) paused on Program" }
        return isCuedInPreview ? "\(name) cued in Preview" : "\(name) ready to cue"
    }

    /// What Cue says for a file the Mac core refuses (plan default D1), the
    /// decoder's own sentence.
    static let webmRefusal =
        "AVFoundation cannot open .webm files; WebM playback on Mac needs FFmpeg, which this build does not use yet."
}

// ── Play / Pause (S2) ────────────────────────────────────────────────────────

extension MediaRoutePlayback {
    enum TapAction: Equatable { case select, pause, resume }

    /// Windows ResolveTap. Only a non-looping clip ON PROGRAM is paused or resumed,
    /// and only from a state the core reported: an unknown or missing state is a
    /// selection, never a guess.
    static func resolveTap(isOnProgram: Bool, isLooping: Bool, state: String?) -> TapAction {
        guard isOnProgram, !isLooping else { return .select }
        if state == "live" { return .pause }
        return state == "paused" || state == "ended" ? .resume : .select
    }

    /// Windows ResolveTransportRow: the route row (`media:<assetId>`) wins when an
    /// asset is both a route and a scene background; otherwise the first row for
    /// the asset, preferring a live one.
    static func resolveTransportRow(_ assetId: String?, rows: [MediaSourceRow]) -> MediaSourceRow? {
        guard let assetId, !assetId.trimmingCharacters(in: .whitespaces).isEmpty, !rows.isEmpty else { return nil }
        let routeSourceId = "media:" + assetId
        var fallback: MediaSourceRow?
        for row in rows where row.mediaAssetId == assetId {
            if row.sourceId == routeSourceId { return row }
            if fallback == nil || row.state == "live" { fallback = row }
        }
        return fallback
    }

    static func resolveTransportState(_ assetId: String?, rows: [MediaSourceRow]) -> String? {
        resolveTransportRow(assetId, rows: rows)?.state
    }

    /// The real on-air state, from the core's row; never "is it the selection".
    static func isPlayingOnAir(_ assetId: String, rows: [MediaSourceRow]) -> Bool {
        resolveTransportState(assetId, rows: rows) == "live"
    }

    /// StudioViewModel.FormatMediaPlaybackActionLabel.
    static func actionLabel(isOnProgram: Bool, playing: Bool) -> String {
        isOnProgram ? (playing ? "Pause Program" : "Resume Program") : (playing ? "Pause audition" : "Audition")
    }
}

/// Windows MediaBinPlaybackProjection: what a snapshot may rewrite on the Media
/// tab. A row on Program OWNS the selected flag; a Preview-only row or no row
/// never clears a local audition; nothing is rewritten when nothing moved.
enum MediaBinPlaybackProjection {
    struct Result: Equatable {
        let liveSignature: String
        let rewriteBinScalars: Bool
        let rewriteSelection: Bool
        let selectedPlaying: Bool
        let selectedStatus: String?
        let selectedTransportState: String?
    }

    static func resolve(rows: [MediaSourceRow], lastLiveSignature: String, lastSelectedTransportState: String?,
                        selectedId: String?, selectedName: String?, selectedPlaying: Bool) -> Result {
        let signature = liveSignature(rows)
        let rewriteBinScalars = lastLiveSignature != signature
        guard let row = MediaRoutePlayback.resolveTransportRow(selectedId, rows: rows), row.onProgram else {
            return Result(liveSignature: signature, rewriteBinScalars: rewriteBinScalars, rewriteSelection: false,
                          selectedPlaying: selectedPlaying, selectedStatus: nil, selectedTransportState: nil)
        }
        let playing = row.state == "live"
        let stateMoved = lastSelectedTransportState != row.state
        if playing == selectedPlaying && !stateMoved {
            return Result(liveSignature: signature, rewriteBinScalars: rewriteBinScalars, rewriteSelection: false,
                          selectedPlaying: selectedPlaying, selectedStatus: nil, selectedTransportState: row.state)
        }
        let trimmed = (selectedName ?? "").trimmingCharacters(in: .whitespaces)
        let name = trimmed.isEmpty ? (selectedId ?? "") : (selectedName ?? "")
        let status: String? = name.isEmpty ? nil
            : playing ? "Playing \(name) on Program"
            : row.state == "ended" ? "\(name) ended on Program" : "\(name) paused on Program"
        return Result(liveSignature: signature, rewriteBinScalars: rewriteBinScalars, rewriteSelection: true,
                      selectedPlaying: playing, selectedStatus: status, selectedTransportState: row.state)
    }

    /// One expression for a bin row's play indicator: on air, or the local audition.
    static func binRowIsPlaying(_ assetId: String, supportsPlayback: Bool, rows: [MediaSourceRow],
                                selectedId: String?, selectedPlaying: Bool) -> Bool {
        (supportsPlayback && MediaRoutePlayback.isPlayingOnAir(assetId, rows: rows))
            || (selectedPlaying && assetId == selectedId)
    }

    /// The live set, order-independent: the only bin-wide play-state scalar.
    static func liveSignature(_ rows: [MediaSourceRow]) -> String {
        rows.filter { $0.state == "live" }.map { $0.mediaAssetId ?? $0.sourceId }.sorted().joined(separator: "|")
    }
}

/// Windows MediaTransportGestureLedger: one slot per asset, newest tap wins. A
/// pause/play is a ONE-SHOT gesture (never part of the repeating scene sync), so
/// a send that was skipped is re-armed; but a re-armed send must never land
/// after a newer tap, or the clip would flip back on air.
final class MediaTransportGestureLedger {
    enum Step: Equatable { case superseded, retry, giveUp }

    private var current: [String: Int64] = [:]
    private var sequence: Int64 = 0
    private let retryAttempts: Int

    init(retryAttempts: Int = 5) { self.retryAttempts = retryAttempts }

    func claim(_ assetId: String) -> Int64 {
        sequence += 1
        current[assetId] = sequence
        return sequence
    }

    func shouldSend(_ assetId: String, _ token: Int64) -> Bool { current[assetId] == token }

    func onSkipped(_ assetId: String, _ token: Int64, attempt: Int) -> Step {
        guard shouldSend(assetId, token) else { return .superseded }
        if attempt >= retryAttempts {
            current[assetId] = nil
            return .giveUp
        }
        return .retry
    }

    @discardableResult
    func release(_ assetId: String, _ token: Int64) -> Bool {
        guard shouldSend(assetId, token) else { return false }
        current[assetId] = nil
        return true
    }
}

// ── Take promotion and the media-selection rollback (S2) ─────────────────────

extension MediaRoutePlayback {
    /// Windows AssetsEnteringProgram: the pure set diff, in route order.
    static func assetsEnteringProgram(previous: [JSONObject], current: [JSONObject]) -> [String] {
        let before = Set(programMediaAssetIds(previous))
        return programMediaAssetIds(current).filter { !before.contains($0) }
    }

    /// Windows ChooseAssetToPromote: the selected asset when it went live, else the
    /// first that did. A still is never promoted (it has no play state).
    static func chooseAssetToPromote(_ wentLive: [String], selected: String?,
                                     supportsPlayback: ((String) -> Bool)?) -> String? {
        let candidates = wentLive.filter { id in
            !id.trimmingCharacters(in: .whitespaces).isEmpty && (supportsPlayback?(id) ?? true)
        }
        if let selected, !selected.isEmpty, candidates.contains(selected) { return selected }
        return candidates.first
    }

    /// Windows SelectedAssetLeftProgram: only the SELECTED asset's departure counts.
    static func selectedAssetLeftProgram(_ selected: String?, previous: [JSONObject], current: [JSONObject]) -> Bool {
        guard let selected, !selected.trimmingCharacters(in: .whitespaces).isEmpty else { return false }
        return isRoutedOnProgram(selected, programRoutes: previous) && !isRoutedOnProgram(selected, programRoutes: current)
    }
}

/// What a Take can move on the Media tab, captured so a rollback can put it back.
struct MediaSelectionState: Equatable {
    var assetId: String?
    var name: String?
    var supportsPlayback: Bool
    var playing: Bool
    var status: String
}

/// Windows TakeMediaSelectionRollback (T1.3 / #430). A failed Take puts the
/// SCENES back; this decides what the selection reads afterwards:
/// - the pre-Take selection, unless the operator moved it while the sync was
///   pending (their choice is kept, like the scene rollback's newer-edits rule);
/// - for a clip on the RESTORED Program, the playing flag and status come from
///   the real on-air row, never from the saved flag;
/// - an operator pick that the rollback takes off air reads "<name> left Program".
enum TakeMediaSelectionRollback {
    static func resolve(beforeTake: MediaSelectionState, afterTake: MediaSelectionState,
                        current: MediaSelectionState, attemptedProgramRoutes: [JSONObject],
                        restoredProgramRoutes: [JSONObject], rows: [MediaSourceRow]) -> MediaSelectionState {
        let operatorMovedIt = current != afterTake
        var target = operatorMovedIt ? current : beforeTake
        guard let assetId = target.assetId, !assetId.isEmpty, target.supportsPlayback else { return target }
        let name = target.name ?? assetId
        guard MediaRoutePlayback.isRoutedOnProgram(assetId, programRoutes: restoredProgramRoutes) else {
            if operatorMovedIt && MediaRoutePlayback.selectedAssetLeftProgram(
                assetId, previous: attemptedProgramRoutes, current: restoredProgramRoutes) {
                target.playing = false
                target.status = "\(name) left Program"
            }
            return target
        }
        let playing = MediaRoutePlayback.isPlayingOnAir(assetId, rows: rows)
        target.playing = playing
        target.status = playing ? "Playing \(name) on Program" : "\(name) paused on Program"
        return target
    }
}
