using CoreVideoPro.Control;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private bool _applyingStreamDestination;

    internal ControlInvokeResult ConfigureStreamDestination(bool srt, IReadOnlyList<object?> args) =>
        StreamDestinationControl.Configure(srt, args,
            !_outputPreferencesLoaded || StreamingRequested || Streaming || !CanSetStreaming(true),
            CaptureProductionOutputPreferences(), _outputPreferencesStore.Save, next =>
            {
                // Suppress per-property saves/syncs: a destination is one committed edit.
                _applyingStreamDestination = true;
                var wasApplyingProfile = _applyingStreamingProfile;
                _applyingStreamingProfile = true;
                try
                {
                    if (srt)
                    {
                        StreamSrtEnabled = next.StreamSrtEnabled;
                        StreamSrtMode = next.StreamSrtMode ?? "caller";
                        StreamSrtHost = next.StreamSrtHost ?? "";
                        StreamSrtPort = next.StreamSrtPort ?? "9000";
                        StreamSrtLatencyMs = next.StreamSrtLatencyMs ?? "120";
                        StreamSrtStreamId = next.StreamSrtStreamId ?? "";
                        StreamSrtKeyLength = next.StreamSrtKeyLength ?? "0";
                        StreamSrtPassphrase = next.StreamSrtPassphrase ?? "";
                    }
                    else
                    {
                        StreamRtmpEnabled = next.StreamRtmpEnabled;
                        StreamRtmpProtocol = next.StreamRtmpProtocol ?? "rtmps";
                        StreamRtmpServerUrl = next.StreamRtmpServerUrl ?? "";
                        StreamRtmpStreamKey = next.StreamRtmpStreamKey ?? "";
                    }
                }
                finally { _applyingStreamingProfile = wasApplyingProfile; _applyingStreamDestination = false; }
                foreach (var property in new[] { nameof(StreamDestinationSummary), nameof(StreamConfigurationSummary),
                    nameof(StreamRtmpSummary), nameof(StreamSrtSummary) }) OnPropertyChanged(property);
            });
}
