// Footer health line + multiview placeholder policy. Pure, snapshot-in/string-out.
//
// Ports, rule for rule, from the Windows tree:
//   - native-shell/CoreVideoPro.MediaCore/Services/StalledFeedsReadout.cs  (#1074)
//   - native-shell/CoreVideoPro.MediaCore/Services/StreamDegradationReadout.cs
//   - native-shell/CoreVideoPro.WinUI/Models/MultiviewEmptyStatePolicy.cs  (#1172/#1175)
// Inputs are the core snapshot's `outputSenderSession.senders[].backpressure` /
// `.status` / `.lifecycle.state`, `outputProfile.fps` and `sources[]`
// (kind, health, hasVideo, sinceLastNewFrameMs). Nothing is measured or guessed here,
// and the line never claims a cause: it states two symptoms.

import Foundation

enum StudioHealthReadout {
    /// Fewer stalled feeds than this is one guest's problem, not a shared one.
    static let minStalledFeeds = 2
    /// The ON-AIR stall threshold, mirroring the core's kOnAirStallNs (1.5 s). The bus's own
    /// `health` turns "stalled" at 200 ms, which flickers on cadence noise, so it is never
    /// used alone.
    static let onAirStallMs = 1500.0

    private static func number(_ value: Any?) -> Double? {
        if value is NSNull { return nil }
        if let n = value as? NSNumber { return n.doubleValue }
        return nil
    }

    /// Producing destinations whose own backpressure is engaged (applied divisor above 1
    /// or pressure level above 0). Shared by both readouts so they agree on what
    /// "the stream is in backpressure" means.
    static func pressuredSenders(_ snapshot: JSONObject) -> [JSONObject] {
        let session = snapshot["outputSenderSession"] as? JSONObject ?? [:]
        let senders = session["senders"] as? [JSONObject] ?? []
        return senders.filter { sender in
            let status = sender["status"] as? String ?? ""
            guard status == "live" || status == "warning" else { return false }
            if let lifecycle = sender["lifecycle"] as? JSONObject,
               (lifecycle["state"] as? String) != "producing" { return false }
            guard let bp = sender["backpressure"] as? JSONObject else { return false }
            return (number(bp["appliedDivisor"]) ?? 0) > 1 || (number(bp["level"]) ?? 0) > 0
        }
    }

    /// "RTMP degraded · 15 fps feed" per pressured destination, joined by "; ".
    static func streamDegradation(_ snapshot: JSONObject) -> String? {
        let fps = number((snapshot["outputProfile"] as? JSONObject)?["fps"]) ?? 0
        let parts = pressuredSenders(snapshot).map { sender -> String in
            let applied = number((sender["backpressure"] as? JSONObject)?["appliedDivisor"]) ?? 0
            let cadence: String
            if fps > 0 && applied > 0 {
                cadence = trimmed(fps / applied) + " fps feed"
            } else {
                cadence = "feed rate unknown"
            }
            return "\((sender["destination"] as? String ?? "").uppercased()) degraded · \(cadence)"
        }
        return parts.isEmpty ? nil : parts.joined(separator: "; ")
    }

    /// Zoom video sources stalled by the on-air rule: bus health "stalled" AND at least
    /// `onAirStallMs` since the last new frame. Zoom only; a row without the age field
    /// (an older core) is never counted.
    static func stalledFeedCount(_ sources: [JSONObject]) -> Int {
        sources.filter { source in
            source["kind"] as? String == "zoom"
                && (source["hasVideo"] as? Bool ?? false)
                && source["health"] as? String == "stalled"
                && (number(source["sinceLastNewFrameMs"]).map { $0 >= onAirStallMs } ?? false)
        }.count
    }

    /// "Stream 445 ms behind and 3 Zoom feeds stalled" — or nil.
    static func stalledFeeds(_ snapshot: JSONObject) -> String? {
        guard let pressured = pressuredSenders(snapshot).first,
              let bp = pressured["backpressure"] as? JSONObject else { return nil }
        let stalled = stalledFeedCount(snapshot["sources"] as? [JSONObject] ?? [])
        guard stalled >= minStalledFeeds else { return nil }
        return "\(describeStream(bp)) and \(stalled) Zoom feeds stalled"
    }

    /// Says what the stream is doing in the words the snapshot can back (appliedDivisor is
    /// usually 1 under pressure since #538, so queue depth or level is the truthful fact).
    private static func describeStream(_ bp: JSONObject) -> String {
        let applied = number(bp["appliedDivisor"]) ?? 0
        if applied > 1 { return "Stream at 1/\(Int(applied)) rate" }
        let buffered = number(bp["bufferedMs"]) ?? 0
        if buffered > 0 { return "Stream \(Int(buffered.rounded())) ms behind" }
        return "Stream backpressure level \(Int(number(bp["level"]) ?? 0))"
    }

    /// C# "0.##" invariant formatting: up to two decimals, no trailing zeros.
    private static func trimmed(_ value: Double) -> String {
        var text = String(format: "%.2f", value)
        while text.hasSuffix("0") { text.removeLast() }
        if text.hasSuffix(".") { text.removeLast() }
        return text
    }
}

/// Assignment guidance replaces only a generic empty loading placeholder.
enum MultiviewEmptyStatePolicy {
    struct Surface {
        var hasFrames = false
        var hasPreviewBitmap = false
        var awaitingGpuHandle = false
        var mediaAssetPath = ""
        var status: String
    }

    static func showAssignmentGuidance(tileCount: Int, surface: Surface?) -> Bool {
        if tileCount != 0 { return false }
        guard let surface else { return true }
        if surface.hasFrames || surface.hasPreviewBitmap || surface.awaitingGpuHandle
            || !surface.mediaAssetPath.trimmingCharacters(in: .whitespaces).isEmpty { return false }
        // Unknown or failure statuses remain visible. Never infer that every surface
        // without a picture is merely preparing, or hide its explanation.
        return ["Waiting for video frames", "Preparing video", "Preparing Program"].contains(surface.status)
    }

    /// Mac mapping: the core publishes the multiview as one IOSurface, but an allocated
    /// surface only says the core made it, not that it holds a picture, and the snapshot
    /// carries no multiview frame signal. Every real picture (PGM/PVW cells, sources) is
    /// a tile, so a surface with zero tiles is "preparing" and the guidance stays; with
    /// tiles present the Windows tile rule already hides it. No surface = Windows' null.
    static func showAssignmentGuidance(tileCount: Int, multiviewSurfaceId: UInt32) -> Bool {
        showAssignmentGuidance(
            tileCount: tileCount,
            surface: multiviewSurfaceId == 0
                ? nil
                : Surface(hasFrames: false, status: "Preparing Program"))
    }
}
