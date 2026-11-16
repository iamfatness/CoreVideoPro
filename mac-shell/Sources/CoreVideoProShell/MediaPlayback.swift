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
}
