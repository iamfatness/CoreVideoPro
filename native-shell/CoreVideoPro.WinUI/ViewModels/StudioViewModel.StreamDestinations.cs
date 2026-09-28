using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private IReadOnlyList<string> BuildSelectedStreamDestinations(bool validatedOnly = false)
    {
        var destinations = new List<string>(4);
        if (StreamRtmpEnabled &&
            (!validatedOnly || StudioStreamOutputValidation.CanSerializeRtmpSettings(
                StreamRtmpProtocol,
                StreamRtmpServerUrl,
                StreamRtmpStreamKey)))
        {
            destinations.Add("rtmp");
        }

        if (StreamNdiEnabled &&
            (!validatedOnly || StudioStreamOutputValidation.CanSerializeNdiSettings(StreamNdiProgramName)))
        {
            destinations.Add("ndi");
        }

        if (StreamSrtEnabled &&
            (!validatedOnly || StudioStreamOutputValidation.CanSerializeSrtSettings(
                StreamSrtMode,
                StreamSrtHost,
                StreamSrtPort,
                StreamSrtLatencyMs,
                StreamSrtStreamId,
                StreamSrtKeyLength,
                StreamSrtPassphrase)))
        {
            destinations.Add("srt");
        }

        if (Hls.Enabled &&
            (!validatedOnly || StudioStreamOutputValidation.ValidateHls(Hls.PlaylistUrl) is null))
        {
            destinations.Add("hls");
        }

        return destinations;
    }

    private IReadOnlyList<MediaCoreStreamDestinationWire> BuildStreamDestinationSettings()
    {
        var destinations = new List<MediaCoreStreamDestinationWire>(4);
        var streamProfile = BuildRequestedOutputProfile(
            "stream",
            StreamRenderResolution,
            StreamRenderFps,
            StreamVideoCodec,
            NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps),
            NormalizeAudioBitrateKbps(StreamAudioBitrateKbps));
        if (StreamRtmpEnabled &&
            StudioStreamOutputValidation.CanSerializeRtmpSettings(
                StreamRtmpProtocol,
                StreamRtmpServerUrl,
                StreamRtmpStreamKey))
        {
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "rtmp",
                Label: "RTMP",
                Protocol: StudioStreamOutputValidation.NormalizeRtmpProtocol(StreamRtmpProtocol),
                Url: StudioStreamOutputValidation.BuildRtmpUrl(StreamRtmpProtocol, StreamRtmpServerUrl),
                StreamKey: NormalizeOutputText(StreamRtmpStreamKey, string.Empty),
                FfmpegBinDirectory: NormalizeOptionalOutputText(FfmpegBinDirectory),
                Fps: streamProfile.Fps,
                TargetBitrateMbps: streamProfile.TargetBitrateMbps,
                AudioBitrateKbps: streamProfile.AudioBitrateKbps,
                VideoCodec: streamProfile.Codec,
                EncoderMode: NormalizeStreamEncoderMode(StreamEncoderMode),
                KeyframeIntervalSeconds: Math.Clamp(StreamKeyframeIntervalSeconds, 0.5, 10),
                RateControl: NormalizeStreamRateControl(StreamRateControl),
                H264Profile: NormalizeStreamH264Profile(StreamH264Profile),
                BFrames: (int)Math.Clamp(Math.Round(StreamBFrames), 0, 4),
                AllowEnhancedRtmp: StreamAllowEnhancedRtmp));
        }

        if (StreamNdiEnabled &&
            StudioStreamOutputValidation.CanSerializeNdiSettings(StreamNdiProgramName))
        {
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "ndi",
                Label: "NDI",
                NdiName: NormalizeOutputText(StreamNdiProgramName, "CoreVideo Pro Program"),
                NdiGroup: NormalizeOutputText(StreamNdiGroupName, "public"),
                Fps: streamProfile.Fps,
                TargetBitrateMbps: streamProfile.TargetBitrateMbps,
                AudioBitrateKbps: streamProfile.AudioBitrateKbps,
                VideoCodec: streamProfile.Codec,
                EncoderMode: NormalizeStreamEncoderMode(StreamEncoderMode)));
        }

        if (StreamSrtEnabled &&
            StudioStreamOutputValidation.CanSerializeSrtSettings(
                StreamSrtMode,
                StreamSrtHost,
                StreamSrtPort,
                StreamSrtLatencyMs,
                StreamSrtStreamId,
                StreamSrtKeyLength,
                StreamSrtPassphrase))
        {
            var latencyMs = ParsePositiveInt(StreamSrtLatencyMs);
            var keyLength = StudioStreamOutputValidation.ParseSrtKeyLength(StreamSrtKeyLength);
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "srt",
                Label: "SRT",
                Mode: StudioStreamOutputValidation.NormalizeSrtMode(StreamSrtMode),
                Host: NormalizeOutputText(StreamSrtHost, string.Empty),
                Port: ParsePositiveInt(StreamSrtPort),
                LatencyMs: latencyMs,
                LatencyUs: latencyMs is null ? null : latencyMs * 1000,
                Passphrase: NormalizeOutputText(StreamSrtPassphrase, string.Empty),
                KeyLength: keyLength,
                StreamId: NormalizeOptionalOutputText(StreamSrtStreamId),
                Fps: streamProfile.Fps,
                TargetBitrateMbps: streamProfile.TargetBitrateMbps,
                VideoCodec: streamProfile.Codec,
                EncoderMode: NormalizeStreamEncoderMode(StreamEncoderMode)));
        }

        if (Hls.Enabled && StudioStreamOutputValidation.ValidateHls(Hls.PlaylistUrl) is null)
        {
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "hls",
                Label: "HLS",
                Protocol: "hls",
                Url: Hls.PlaylistUrl.Trim(),
                FfmpegBinDirectory: NormalizeOptionalOutputText(FfmpegBinDirectory),
                Fps: streamProfile.Fps,
                TargetBitrateMbps: streamProfile.TargetBitrateMbps,
                AudioBitrateKbps: streamProfile.AudioBitrateKbps,
                VideoCodec: streamProfile.Codec,
                EncoderMode: NormalizeStreamEncoderMode(StreamEncoderMode),
                KeyframeIntervalSeconds: Math.Clamp(StreamKeyframeIntervalSeconds, 0.5, 10),
                RateControl: NormalizeStreamRateControl(StreamRateControl),
                H264Profile: NormalizeStreamH264Profile(StreamH264Profile),
                BFrames: (int)Math.Clamp(Math.Round(StreamBFrames), 0, 4)));
        }

        return destinations;
    }

}
