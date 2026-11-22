// OutputProfiles — the recording render profile and the stream encoder settings
// (outputs spec §3.1 / §5.1, slice O1). Pure values: no model, no I/O.
//
// Windows: Production Settings > Render profiles + Streaming > advanced
// (ProductionSettingsWindow.xaml:172-251, :339-412), sent by
// MediaCoreCommandBuilder.OutputProfilePayload (:850-860) as
// `recordingOutputProfile` on start-program-output and as `renderProfile` (plus
// targetBitrateMbps / audioBitrateKbps) on set-recording-targets (:799-805).
//
// Mac differences, each deliberate (plan D3-D5):
// - the STREAM keeps the canvas size and the destination's fixed 30 fps — a
//   stream fps picker would push 1080p60 BGRA through the CPU ffmpeg pipe;
// - the stream codec stays H.264 (no hevc_videotoolbox in EncoderPolicy on Apple);
// - the recording codec offers H.264 and H.265; AV1 is listed disabled with its
//   reason (the AVF writer refuses it, AVFoundationEncoderAdapter.mm);
// - no recording quality picker: the AVF writer has no quality knob, the bitrate
//   is what the encoder honours;
// - no hardware-encoder picker: VideoToolbox is the only encoder on a Mac.

import Foundation

/// Defaults = Windows DefaultRecordingOutputProfile (the 1080p60 canvas
/// profile with 192 kbps audio, MediaCoreProductionSyncContext.cs:412-424), which
/// is also exactly what the core records at when no profile is sent
/// (MediaCore.h recordingOutput* defaults) — so a Mac that never opens the
/// picker records as it did before this slice.
struct RecordingProfile: Codable, Equatable {
    var width = 1920
    var height = 1080
    var codec = "h264"
    /// Whole Mbps: the core's RecordingSessionRequest.targetBitrateMbps is an
    /// int (MediaCore.cpp:2814 truncates on every platform), so a 0.5 step would
    /// show a value the file never gets. Windows' 8.2 default is written as 8.
    var targetBitrateMbps = 8.0
    var audioBitrateKbps = 192

    /// Windows OutputResolutionOptions / OutputFpsOptions / VideoCodecOptions.
    static let sizes: [(width: Int, height: Int, label: String)] = [
        (1280, 720, "1280x720 (720p)"), (1920, 1080, "1920x1080 (1080p)"),
        (2560, 1440, "2560x1440 (1440p)"), (3840, 2160, "3840x2160 (4K)"),
    ]
    /// No recording fps choice (review #4): neither writer decimates (Windows MF
    /// sets MF_MT_FRAME_RATE as metadata, the Mac AVF writer ignores it), so the
    /// file runs at the render rate. The payload carries the canvas fps so the
    /// published recordingFps tells the truth.
    /// (value, label, available). AV1 is shown so the operator knows why it is absent.
    static let codecs: [(value: String, label: String, available: Bool)] = [
        ("h264", "H.264 / AVC", true), ("h265", "H.265 / HEVC", true), ("av1", "AV1", false),
    ]
    static let av1Unavailable = "AV1 recording is not available on this Mac"
    static let bitrateRange: ClosedRange<Double> = 1...80
    static let audioKbpsRange: ClosedRange<Int> = 32...512

    /// Anything a hand-edited file or a control call could put here that the
    /// pickers cannot produce falls back to `fallback`'s value, field by field.
    func sanitized(fallback: RecordingProfile) -> RecordingProfile {
        var out = self
        if !Self.sizes.contains(where: { $0.width == width && $0.height == height }) {
            out.width = fallback.width
            out.height = fallback.height
        }
        if !Self.codecs.contains(where: { $0.value == codec && $0.available }) { out.codec = fallback.codec }
        out.targetBitrateMbps = targetBitrateMbps.isFinite
            ? min(max(targetBitrateMbps.rounded(.down), Self.bitrateRange.lowerBound), Self.bitrateRange.upperBound)
            : fallback.targetBitrateMbps
        out.audioBitrateKbps = min(max(audioBitrateKbps, Self.audioKbpsRange.lowerBound),
                                   Self.audioKbpsRange.upperBound)
        return out
    }

    /// Windows OutputProfilePayload, field for field. `fps` = the canvas rate.
    func payload(fps: Int) -> JSONObject {
        ["profileId": "\(height)p\(fps)", "resolution": "\(width)x\(height)",
         "width": width, "height": height, "fps": fps,
         "targetBitrateMbps": targetBitrateMbps, "audioBitrateKbps": audioBitrateKbps,
         "codec": codec]
    }
}

struct StreamEncoderSettings: Codable, Equatable {
    var audioBitrateKbps = 160
    var keyframeIntervalSeconds = 2.0
    var rateControl = "cbr"
    var h264Profile = "high"
    var bFrames = 2

    /// Windows StreamRateControlOptions / StreamH264ProfileOptions.
    static let rateControls: [(value: String, label: String)] = [
        ("cbr", "CBR (constant bitrate)"), ("vbr", "VBR (variable bitrate)"),
    ]
    static let h264Profiles: [(value: String, label: String)] = [
        ("auto", "Auto"), ("baseline", "Baseline"), ("main", "Main"), ("high", "High"),
    ]
    /// The core's own clamps (MediaCore.cpp readOutputDestinationSettings and
    /// RtmpOutputSenderAdapter: keyframe 0.5-10 s, B-frames 0-4, audio 32-512).
    static let keyframeRange: ClosedRange<Double> = 0.5...10
    static let bFramesRange: ClosedRange<Int> = 0...4
    static let audioKbpsRange: ClosedRange<Int> = 32...512

    func sanitized() -> StreamEncoderSettings {
        var out = self
        let defaults = StreamEncoderSettings()
        out.audioBitrateKbps = min(max(audioBitrateKbps, Self.audioKbpsRange.lowerBound),
                                   Self.audioKbpsRange.upperBound)
        out.keyframeIntervalSeconds = keyframeIntervalSeconds.isFinite
            ? min(max(keyframeIntervalSeconds, Self.keyframeRange.lowerBound), Self.keyframeRange.upperBound)
            : defaults.keyframeIntervalSeconds
        if !Self.rateControls.contains(where: { $0.value == rateControl }) { out.rateControl = defaults.rateControl }
        if !Self.h264Profiles.contains(where: { $0.value == h264Profile }) { out.h264Profile = defaults.h264Profile }
        out.bFrames = min(max(bFrames, Self.bFramesRange.lowerBound), Self.bFramesRange.upperBound)
        return out
    }

    /// The destination-row fields this value owns. ONE list so `apply(to:)`
    /// and the every-field test cannot drift apart.
    static let ownedKeys = ["audioBitrateKbps", "keyframeIntervalSeconds", "rateControl",
                            "h264Profile", "bFrames"]

    /// Overwrites the encoder fields of one destination row (plan D6: the
    /// streaming lane's per-destination builder applies this to each row).
    /// An explicit destination value wins over the stream profile in the core
    /// (#606), so the operator's value must be ON the row.
    func apply(to row: JSONObject) -> JSONObject {
        var row = row
        let values: [String: Any] = [
            "audioBitrateKbps": audioBitrateKbps, "keyframeIntervalSeconds": keyframeIntervalSeconds,
            "rateControl": rateControl, "h264Profile": h264Profile, "bFrames": bFrames,
        ]
        for key in Self.ownedKeys { row[key] = values[key] }
        return row
    }
}
