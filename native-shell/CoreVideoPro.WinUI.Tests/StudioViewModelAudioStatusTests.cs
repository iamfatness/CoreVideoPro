using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioViewModelAudioStatusTests
{
    [Fact]
    public void FormatAudioMixerFailureStatus_PreservesLoadFailureDetail()
    {
        var status = StudioViewModel.FormatAudioMixerFailureStatus(
            new InvalidOperationException("Failed to load audio mixer XAML."));

        Assert.Equal("Audio mixer failed to open: Failed to load audio mixer XAML.", status);
    }

    [Fact]
    public void FormatAudioMixerActionFailureStatus_PreservesRuntimeFailureDetail()
    {
        var status = StudioViewModel.FormatAudioMixerActionFailureStatus(
            new InvalidOperationException("Mixer channel disappeared while updating gain."));

        Assert.Equal("Audio mixer action failed: Mixer channel disappeared while updating gain.", status);
    }

    [Theory]
    [InlineData("local-machine-audio", true)]
    [InlineData("capture:uvc-01", true)]
    [InlineData("media", true)]
    [InlineData("zoom-mix", false)]
    [InlineData("active-speaker", false)]
    [InlineData("screen-share", false)]
    public void IsConcreteAudioMixSourceId_TreatsMediaAsMixerChannelButKeepsPlaceholdersOut(
        string sourceId,
        bool expected)
    {
        Assert.Equal(expected, StudioViewModel.IsConcreteAudioMixSourceId(sourceId));
    }

    [Fact]
    public void ResolveSceneMediaAudioSourceIds_AddsGenericMediaMixerChannelForMediaRoutes()
    {
        var sourceIds = StudioViewModel.ResolveSceneMediaAudioSourceIds(
            [
                new SourceRoute
                {
                    Id = "media-route",
                    Mode = SourceRouteMode.Fixed,
                    ParticipantId = ShowInputRosterService.ToMediaSourceId("clip-intro")
                },
                new SourceRoute
                {
                    Id = "guest-route",
                    Mode = SourceRouteMode.Fixed,
                    ParticipantId = "guest-1"
                }
            ]);

        Assert.Equal(["media"], sourceIds);
    }

    [Theory]
    [InlineData(ShowInputKind.ZoomParticipant, true)]
    [InlineData(ShowInputKind.UvcWebcam, false)]
    [InlineData(ShowInputKind.Blackmagic, false)]
    [InlineData(ShowInputKind.SrtIngest, false)]
    [InlineData(ShowInputKind.Media, false)]
    [InlineData(ShowInputKind.Unassigned, false)]
    public void IsShowInputAudioSource_OnlyTreatsZoomInputsAsImplicitAudioSources(
        ShowInputKind kind,
        bool expected)
    {
        var slot = new ShowInputSlot
        {
            SlotNumber = 1,
            Kind = kind,
            ParticipantId = kind is ShowInputKind.ZoomParticipant or ShowInputKind.Media ? "source-1" : null,
            CaptureDeviceId = kind is ShowInputKind.UvcWebcam or ShowInputKind.Blackmagic or ShowInputKind.SrtIngest ? "capture-1" : null,
            InShow = true
        };

        Assert.Equal(expected, StudioViewModel.IsShowInputAudioSource(slot));
    }

    [Fact]
    public void FormatMixerLufsLabel_UsesLufsUnitsNotDbfs()
    {
        var label = StudioViewModel.FormatMixerLufsLabel(-18.37);

        Assert.Equal("-18.4 LUFS", label);
        Assert.DoesNotContain("dBFS", label, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_LeadsWithFfmpegActionAndRemovesMediaCorePrefix()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: start-program-output failed. ffmpeg.exe was not found in C:\\ffmpeg\\bin."));

        Assert.StartsWith(
            "Streaming start failed: FFmpeg is not ready. Choose the FFmpeg bin folder in Settings > FFmpeg.",
            status);
        Assert.DoesNotContain("media-core sync failed", status, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("ffmpeg.exe was not found", status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_LeadsWithRtmpActionForConnectionFailures()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: RTMP sender exited. Connection refused ffmpeg exited with code 1"));

        Assert.StartsWith(
            "Streaming start failed: RTMP output failed. Check the server URL, stream key, and network.",
            status);
        Assert.Contains("Connection refused", status, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatStreamingFailureStatus_RemovesNestedNativeSyncPrefixes()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: native-media-core-sync failed: start-program-output failed. missing:ffmpeg executable"));

        Assert.StartsWith(
            "Streaming start failed: FFmpeg is not ready. Choose the FFmpeg bin folder in Settings > FFmpeg.",
            status);
        Assert.Contains("FFmpeg executable was not found", status, StringComparison.Ordinal);
        Assert.DoesNotContain("media-core", status, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("start-program-output", status, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("missing:ffmpeg", status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_PrioritizesFfmpegRuntimeOverRtmpContext()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: start-program-output failed: RTMP sender requires FFmpeg runtime on this machine (ffmpeg.exe was not found)."));

        Assert.StartsWith(
            "Streaming start failed: FFmpeg is not ready. Choose the FFmpeg bin folder in Settings > FFmpeg.",
            status);
        Assert.Equal("FFmpeg not ready", StudioViewModel.FormatOutputStatusBrief(status));
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsMissingRtmpConfiguration()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("Configure RTMP server URL before streaming."));

        Assert.StartsWith(
            "Streaming start failed: RTMP settings are incomplete. Configure the server URL and stream key before streaming.",
            status);
        Assert.Equal("RTMP settings missing", StudioViewModel.FormatOutputStatusBrief(status));
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsNoSelectedDestination()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("Select at least one stream destination."));

        Assert.StartsWith(
            "Streaming start failed: No stream destination is selected. Enable RTMP, NDI, or SRT before streaming.",
            status);
        Assert.Equal("No stream destination", StudioViewModel.FormatOutputStatusBrief(status));
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsUnavailableNdiOutput()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("NDI output is selected, but no NDI sender module is available in this build."));

        Assert.StartsWith(
            "Streaming start failed: NDI output is not available. Install the NDI runtime or use a build with NDI output enabled.",
            status);
        Assert.Equal("NDI unavailable", StudioViewModel.FormatOutputStatusBrief(status));
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsMissingNdiProfileCapability()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("NDI output is selected, but the native media core profile is missing ndi-output."));

        Assert.StartsWith(
            "Streaming start failed: NDI output is not available. Install the NDI runtime or use a build with NDI output enabled.",
            status);
        Assert.Equal("NDI unavailable", StudioViewModel.FormatOutputStatusBrief(status));
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsUnavailableSrtOutput()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("srt-output-unavailable: SRT output sender is not available in this build."));

        Assert.StartsWith(
            "Streaming start failed: SRT output is not available in this build. Use RTMP/NDI or install a build with SRT output enabled.",
            status);
        Assert.Equal("SRT unavailable", StudioViewModel.FormatOutputStatusBrief(status));
    }

    [Fact]
    public void ValidateStreamDestinationCapabilities_BlocksSelectedUnsupportedOutputs()
    {
        var profile = new NativeMediaCoreProfile
        {
            Name = "CoreVideo Pro Native Media Core",
            Renderer = "d3d11",
            MaxProgramResolution = "1920x1080",
            MaxProgramFps = 60,
            MaxParticipantFeeds = 8,
            MaxIsoRecordings = 8,
            Capabilities = ["rtmp-output"]
        };

        Assert.Null(StudioViewModel.ValidateStreamDestinationCapabilities(true, false, false, profile));
        Assert.Equal(
            "NDI output is selected, but the native media core profile is missing ndi-output.",
            StudioViewModel.ValidateStreamDestinationCapabilities(false, true, false, profile));
        Assert.Equal(
            "SRT output is selected, but the native media core profile is missing srt-output.",
            StudioViewModel.ValidateStreamDestinationCapabilities(false, false, true, profile));
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsSenderNotArmed()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("Selected stream destinations (NDI) did not arm a native output sender. Sender state idle:0."));

        Assert.StartsWith(
            "Streaming start failed: Native output sender did not start. Check Stream settings and open Health for sender diagnostics.",
            status);
        Assert.Equal("Stream sender not armed", StudioViewModel.FormatOutputStatusBrief(status));
    }

    [Fact]
    public void TryFormatStreamingStartNoSenderFailure_ReportsIdleNativeSenderForRequestedDestination()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "idle",
                ActiveSenderCount = 0
            },
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "ndi",
                    Status = "idle",
                    Message = "NDI sender idle."
                }
            ]
        };

        Assert.True(StudioViewModel.TryFormatStreamingStartNoSenderFailure(snapshot, ["ndi"], out var failureStatus));
        Assert.Contains("Selected stream destinations (NDI) did not arm a native output sender.", failureStatus, StringComparison.Ordinal);
        Assert.Equal("Stream sender not armed", StudioViewModel.FormatOutputStatusBrief(failureStatus));
    }

    [Fact]
    public void TryFormatStreamingStartNoSenderFailure_ReportsUnavailableWarningSender()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "warning",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "ndi:program",
                        Destination = "ndi",
                        Status = "warning",
                        LastResultCode = "ndi-output-unavailable",
                        Warning = "NDI output is selected, but no NDI sender module is available in this build."
                    }
                ]
            }
        };

        Assert.True(StudioViewModel.TryFormatStreamingStartNoSenderFailure(snapshot, ["ndi"], out var failureStatus));
        Assert.Equal("NDI unavailable", StudioViewModel.FormatOutputStatusBrief(failureStatus));
    }

    [Fact]
    public void FormatStreamingFailureStatus_RemovesRtmpSenderWrappers()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core request failed: program-output failed: output sender failed during sync: RTMP sender URL must include a host and application path."));

        Assert.StartsWith(
            "Streaming start failed: RTMP output failed. Check the server URL, stream key, and network.",
            status);
        Assert.Contains("RTMP sender URL must include a host and application path.", status, StringComparison.Ordinal);
        Assert.DoesNotContain("media-core request failed", status, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("program-output failed", status, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("output sender failed", status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_RemovesRtmpSenderFailedWrapper()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: RTMP sender failed: FFmpeg exited with code 1 after ingest rejected credentials."));

        Assert.StartsWith(
            "Streaming start failed: RTMP output failed. Check the server URL, stream key, and network.",
            status);
        Assert.Contains("FFmpeg exited with code 1 after ingest rejected credentials.", status, StringComparison.Ordinal);
        Assert.DoesNotContain("media-core", status, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("RTMP sender failed", status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsProgramFrameReadiness()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: output sender failed: RTMP sender is waiting for a program frame."));

        Assert.StartsWith(
            "Streaming start failed: Program video is not ready. Put a valid source on Program before streaming.",
            status);
        Assert.Contains("RTMP sender is waiting for a program frame.", status, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatStreamingFailureStatus_MapsWrappedInFlightSyncToBusyStatus()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync in flight; skipped for backpressure"));

        Assert.StartsWith(
            "Streaming start failed: Media core is busy applying changes. Wait a moment and try Stream again.",
            status);
        Assert.DoesNotContain("media-core sync in flight", status, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("backpressure", status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatOutputStatusBrief_CollapsesStreamingSettingsFailure()
    {
        var fullStatus = StudioViewModel.FormatStreamingFailureStatus(
            "settings",
            new InvalidOperationException("media-core sync failed: output sender failed during sync: RTMP sender exited. Connection refused"));

        Assert.Equal("RTMP output failed", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
    }

    [Fact]
    public void FormatOutputStatusBrief_CollapsesInvalidStreamingSettingsStoppedStatus()
    {
        var fullStatus = StudioViewModel.FormatStreamingFailureStatus(
            "settings",
            new InvalidOperationException("Configure RTMP stream key before streaming.")) + " Streaming stopped.";

        Assert.Equal("RTMP settings missing", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
        Assert.Contains("Streaming stopped", fullStatus, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatOutputStatusBrief_SurfacesActionableStreamingFailureAndKeepsDetailsAvailable()
    {
        var fullStatus = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: start-program-output failed. RTMP sender exited. Connection refused ffmpeg exited with code 1"));

        Assert.Equal("RTMP output failed", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
        Assert.Contains("Connection refused", fullStatus, StringComparison.Ordinal);
        Assert.DoesNotContain("media-core sync failed", fullStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatOutputStatusBrief_SurfacesProgramReadinessFailure()
    {
        var fullStatus = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: output sender failed: RTMP sender is waiting for a program frame."));

        Assert.Equal("Program video not ready", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
    }

    [Fact]
    public void FormatOutputStatusBrief_SurfacesBusyMediaCoreStreamingFailure()
    {
        var fullStatus = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync in flight; skipped for backpressure"));

        Assert.Equal("Media core busy", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
    }

    [Fact]
    public void FormatRecordingFailureStatus_SurfacesProgramReadinessFailure()
    {
        var fullStatus = StudioViewModel.FormatRecordingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: output sender failed: recording is waiting for a program frame."));

        Assert.StartsWith(
            "Recording start failed: Program video is not ready. Put a valid source on Program before recording.",
            fullStatus);
        Assert.Equal("Program video not ready", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.DoesNotContain("media-core sync failed", fullStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatRecordingFailureStatus_SurfacesBusyMediaCoreFailure()
    {
        var fullStatus = StudioViewModel.FormatRecordingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync in flight; skipped for backpressure"));

        Assert.StartsWith(
            "Recording start failed: Media core is busy applying changes. Wait a moment and try Record again.",
            fullStatus);
        Assert.Equal("Media core busy", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.DoesNotContain("backpressure", fullStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatRecordingFailureStatus_SurfacesTargetFailures()
    {
        var fullStatus = StudioViewModel.FormatRecordingFailureStatus(
            "start",
            new IOException("media-core request failed: target folder path is not writable or disk is full."));

        Assert.StartsWith(
            "Recording start failed: Recording target is not ready. Check the folder path and disk space.",
            fullStatus);
        Assert.Equal("Recording target not ready", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.Contains("disk is full", fullStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_SurfacesNativeMediaCoreFailures()
    {
        var fullStatus = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core request failed: native media core process exited while applying start-program-output."));

        Assert.StartsWith(
            "Streaming start failed: Media core failed while starting stream. Open Details for the native error.",
            fullStatus);
        Assert.Equal("Native core exited", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
        Assert.Contains("native media core process exited", fullStatus, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("media-core request failed", fullStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatOutputStatusBrief_SurfacesNativeMediaCorePipeFailures()
    {
        var fullStatus = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new IOException("media-core request failed: JSON-RPC broken pipe while applying start-program-output."));

        Assert.Equal("Native core pipe failed", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
        Assert.Contains("broken pipe", fullStatus, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("media-core request failed", fullStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatOutputStatusBrief_DoesNotShowDetailsForShortIdleStatus()
    {
        Assert.Equal("Outputs idle", StudioViewModel.FormatOutputStatusBrief("Outputs idle"));
        Assert.False(StudioViewModel.ShouldShowOutputStatusDetails("Outputs idle"));
    }

    [Theory]
    [InlineData("RTMP output failed: FFmpeg exited with code 1.", "RTMP output failed")]
    [InlineData("RTMP output warning: RTMP sender is retrying after connection refused.", "Stream warning")]
    [InlineData("Output warning: RTMP sender needs a configured RTMP/RTMPS server URL and stream key.", "RTMP output warning")]
    [InlineData("Output failed: FFmpeg stdin write failed; the RTMP process stopped or rejected frames.", "RTMP output failed")]
    public void FormatOutputStatusBrief_CollapsesStreamOutputHealthStatus(string status, string expected)
    {
        Assert.Equal(expected, StudioViewModel.FormatOutputStatusBrief(status));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(status));
    }

    [Theory]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public void ResolveStreamingStateAfterFailedRetry_RollsBackRequestedState(bool requestedStarting, bool expectedStreaming)
    {
        Assert.Equal(expectedStreaming, StudioViewModel.ResolveStreamingStateAfterFailedRetry(requestedStarting));
    }

    [Theory]
    [InlineData(true, "Streaming start failed: Media core is busy applying changes. Wait a moment and try Stream again.", "Media core busy")]
    [InlineData(false, "Streaming stop failed: Media core is busy applying changes. Wait a moment and try Stream again.", "Stream stop failed")]
    public void FormatStreamSyncRetryExhaustedStatus_ShowsActionableBusyState(
        bool requestedStarting,
        string expectedStatus,
        string expectedBrief)
    {
        var status = StudioViewModel.FormatStreamSyncRetryExhaustedStatus(requestedStarting);

        Assert.Equal(expectedStatus, status);
        Assert.Equal(expectedBrief, StudioViewModel.FormatOutputStatusBrief(status));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(status));
    }

    [Theory]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public void ResolveRecordingStateAfterFailedRetry_RollsBackRequestedState(bool requestedStarting, bool expectedRecording)
    {
        Assert.Equal(expectedRecording, StudioViewModel.ResolveRecordingStateAfterFailedRetry(requestedStarting));
    }

    [Theory]
    [InlineData(true, "Recording start failed: Media core is busy applying changes. Wait a moment and try Record again.", "Media core busy")]
    [InlineData(false, "Recording stop failed: Media core is busy applying changes. Wait a moment and try Record again.", "Recording stop failed")]
    public void FormatRecordingSyncRetryExhaustedStatus_ShowsActionableBusyState(
        bool requestedStarting,
        string expectedStatus,
        string expectedBrief)
    {
        var status = StudioViewModel.FormatRecordingSyncRetryExhaustedStatus(requestedStarting);

        Assert.Equal(expectedStatus, status);
        Assert.Equal(expectedBrief, StudioViewModel.FormatOutputStatusBrief(status));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(status));
    }

    [Fact]
    public void TryFormatStreamingStartHealthFailure_MapsNativeOutputWarningsToStartFailure()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "rtmp",
                    Status = "warning",
                    Message = "RTMP sender requires FFmpeg runtime on this machine (ffmpeg.exe was not found)."
                }
            ]
        };

        Assert.True(StudioViewModel.TryFormatStreamingStartHealthFailure(snapshot, out var failureStatus));
        Assert.StartsWith(
            "Streaming start failed: FFmpeg is not ready. Choose the FFmpeg bin folder in Settings > FFmpeg.",
            failureStatus);
        Assert.Equal("FFmpeg not ready", StudioViewModel.FormatOutputStatusBrief(failureStatus));
    }

    [Fact]
    public void TryFormatStreamingStartHealthFailure_UsesSenderResultAndRuntimeDetail()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "warning",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "ndi:program",
                        Destination = "ndi",
                        Status = "warning",
                        LastResultCode = "ndi-output-unavailable",
                        RuntimeDetail = "LibNDI runtime-missing on this machine."
                    }
                ]
            }
        };

        Assert.True(StudioViewModel.TryFormatStreamingStartHealthFailure(snapshot, out var failureStatus));
        Assert.StartsWith(
            "Streaming start failed: NDI output is not available. Install the NDI runtime or use a build with NDI output enabled.",
            failureStatus);
        Assert.Contains("ndi-output-unavailable", failureStatus, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("runtime-missing", failureStatus, StringComparison.OrdinalIgnoreCase);
        Assert.Equal("NDI unavailable", StudioViewModel.FormatOutputStatusBrief(failureStatus));
    }

    [Fact]
    public void TryFormatStreamingStartHealthFailure_PrefersSenderDetailOverGenericOutputHealth()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "ndi",
                    Status = "warning",
                    Message = "NDI sender warning."
                }
            ],
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "warning",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "ndi:program",
                        Destination = "ndi",
                        Status = "warning",
                        LastResultCode = "ndi-output-unavailable",
                        RuntimeDetail = "LibNDI runtime-missing on this machine."
                    }
                ]
            }
        };

        Assert.True(StudioViewModel.TryFormatStreamingStartHealthFailure(snapshot, out var failureStatus));
        Assert.StartsWith(
            "Streaming start failed: NDI output is not available. Install the NDI runtime or use a build with NDI output enabled.",
            failureStatus);
        Assert.DoesNotContain("NDI sender warning.", failureStatus, StringComparison.Ordinal);
        Assert.Contains("runtime-missing", failureStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void TryFormatRecordingStartHealthFailure_MapsNativeOutputWarningsToStartFailure()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "recording",
                    Status = "failed",
                    Message = "Recording target folder path is not writable."
                }
            ]
        };

        Assert.True(StudioViewModel.TryFormatRecordingStartHealthFailure(snapshot, out var failureStatus));
        Assert.StartsWith(
            "Recording start failed: Recording target is not ready. Check the folder path and disk space.",
            failureStatus);
        Assert.Equal("Recording target not ready", StudioViewModel.FormatOutputStatusBrief(failureStatus));
    }

    [Fact]
    public void TryFormatRecordingStartHealthFailure_MapsRecordingSessionError()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            Recording = new NativeMediaCoreRecordingSession
            {
                SessionId = "session-1",
                Active = true,
                Status = "failed",
                WriterStatus = "failed",
                TargetFolder = @"C:\recordings",
                FilenamePrefix = "show",
                Format = "mkv",
                Quality = "high",
                ProgramPath = @"C:\recordings\show.mkv",
                Error = "Native media core process exited while starting recording."
            }
        };

        Assert.True(StudioViewModel.TryFormatRecordingStartHealthFailure(snapshot, out var failureStatus));
        Assert.StartsWith(
            "Recording start failed: Media core failed while starting recording. Open Details for the native error.",
            failureStatus);
        Assert.Equal("Native core exited", StudioViewModel.FormatOutputStatusBrief(failureStatus));
    }

    [Fact]
    public void TryFormatStreamingStartHealthFailure_IgnoresIdleOutputSnapshot()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "rtmp",
                    Status = "idle",
                    Message = "RTMP idle."
                }
            ]
        };

        Assert.False(StudioViewModel.TryFormatStreamingStartHealthFailure(snapshot, out var failureStatus));
        Assert.Equal(string.Empty, failureStatus);
    }

    [Fact]
    public void TryFormatRecordingStartHealthFailure_IgnoresIdleOutputSnapshot()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "recording",
                    Status = "idle",
                    Message = "Recording idle."
                }
            ]
        };

        Assert.False(StudioViewModel.TryFormatRecordingStartHealthFailure(snapshot, out var failureStatus));
        Assert.Equal(string.Empty, failureStatus);
    }


    [Fact]
    public void FormatAudioMonitorEngineStatus_UsesFramesAndMonitorStateWithoutMasterPercent()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MasterLevel = 87,
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };

        var status = StudioViewModel.FormatAudioMonitorEngineStatus(audio);

        Assert.Equal("960 mixed frames - monitor 480 playback frames", status);
        Assert.DoesNotContain("% master", status, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatAudioMonitorEngineStatus_ReportsMutedWithoutSyntheticMasterLevel()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MasterLevel = 72,
            MixedFrameCount = 480,
            MonitorEnabled = false,
            MonitorStatus = "muted"
        };

        var status = StudioViewModel.FormatAudioMonitorEngineStatus(audio);

        Assert.Equal("480 mixed frames - monitor muted", status);
        Assert.DoesNotContain("72", status, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatAudioMonitorEngineStatus_ShowsMonBusSignalWhenMonitorIsMuted()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 480,
            MonitorEnabled = false,
            MonitorStatus = "muted"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            RoutedMonitorFrames = 480
        };

        var status = StudioViewModel.FormatAudioMonitorEngineStatus(audio, capture);

        Assert.Equal("480 mixed frames - monitor muted - MON bus has 480 frames ready", status);
    }

    [Fact]
    public void FormatAudioMonitorEngineStatus_ShowsHardwarePlaybackGap()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "armed",
            MonitorFramesPlayed = 0
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            RoutedMonitorFrames = 960
        };

        var status = StudioViewModel.FormatAudioMonitorEngineStatus(audio, capture);

        Assert.Equal("960 mixed frames - monitor armed, waiting for audio - MON bus 960 frames, no hardware playback frames", status);
    }

    [Fact]
    public void FormatAudioMonitorEngineStatus_ShowsZeroVolumeMonitorState()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "volume-zero",
            MonitorFramesPlayed = 0
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            RoutedMonitorFrames = 960
        };

        var status = StudioViewModel.FormatAudioMonitorEngineStatus(audio, capture);

        Assert.Equal("960 mixed frames - monitor volume at 0% - MON bus 960 frames, no hardware playback frames", status);
    }

    [Fact]
    public void FormatStudioMonitorSummary_SeparatesMissingMonBusFromOutputDevice()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "armed"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatStudioMonitorSummary(audio, capture, true, "Studio Headphones");

        Assert.Equal("Monitor armed, waiting for audio - Studio Headphones - no PCM on MON bus", status);
    }

    [Fact]
    public void FormatStudioMonitorSummary_ShowsWhenPcmExistsButMonBusIsUnrouted()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "armed"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatStudioMonitorSummary(audio, capture, true, "Studio Headphones");

        Assert.Equal("Monitor armed, waiting for audio - Studio Headphones - no PCM on MON bus; source PCM 960, PGM 960", status);
    }

    [Fact]
    public void FormatStudioMonitorSummary_ReportsFallbackPlaybackWhenMonBusIsUnrouted()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatStudioMonitorSummary(audio, capture, true, "Studio Headphones");

        Assert.Equal("Monitor 480 playback frames - Studio Headphones - fallback monitor mix 480 frames; no routed MON bus", status);
    }

    [Fact]
    public void FormatAudioMonitorEngineStatus_ShowsWhenPcmExistsButMonBusIsUnrouted()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "armed"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatAudioMonitorEngineStatus(audio, capture);

        Assert.Equal("960 mixed frames - monitor armed, waiting for audio - no PCM on MON bus; source PCM 960, PGM 960", status);
    }

    [Fact]
    public void FormatAudioMonitorEngineStatus_ReportsFallbackPlaybackWhenMonBusIsUnrouted()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatAudioMonitorEngineStatus(audio, capture);

        Assert.Equal("960 mixed frames - monitor 480 playback frames - fallback monitor mix 480 frames; no routed MON bus", status);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsNoPcmPath()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "idle",
            Summary = "No PCM",
            MonitorStatus = "muted"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "waiting",
            Summary = "Waiting",
            SourceCount = 1,
            StreamingCount = 1,
            Sources =
            [
                new NativeMediaCoreCaptureAudioSource
                {
                    CaptureDeviceId = "local-machine-audio",
                    AudioDeviceName = "Desk Mix",
                    AudioSourceKind = "loopback",
                    Paired = true,
                    CaptureStreaming = true
                }
            ]
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM none | mix none | PGM none | MON none | monitor off | check source PCM - Desk Mix (loopback, streaming, no frames)", status);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsMonitorOffWhenMonBusHasSignal()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = false,
            MonitorStatus = "muted"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 960,
            Sources =
            [
                new NativeMediaCoreCaptureAudioSource
                {
                    CaptureDeviceId = "local-machine-audio",
                    AudioDeviceName = "Desk Mix",
                    AudioSourceKind = "wasapi-loopback",
                    Paired = true,
                    CaptureStreaming = true,
                    CaptureFramesReceived = 960
                }
            ]
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON 960 | monitor off - Desk Mix (wasapi-loopback, 960 frames)", status);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsMonitorPlaybackGap()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "armed"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 960,
            Sources =
            [
                new NativeMediaCoreCaptureAudioSource
                {
                    CaptureDeviceId = "local-machine-audio",
                    AudioDeviceName = "Desk Mix",
                    AudioSourceKind = "wasapi-loopback",
                    Paired = true,
                    CaptureStreaming = true,
                    CaptureFramesReceived = 960
                }
            ]
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON 960 | playback none (armed, waiting for audio) | check monitor output - Desk Mix (wasapi-loopback, 960 frames)", status);
    }

    [Fact]
    public void FormatCaptureAudioSourceStatus_ShortensPacketlessLoopbackWarning()
    {
        var status = StudioViewModel.FormatCaptureAudioSourceStatus(new NativeMediaCoreCaptureAudioSource
        {
            CaptureDeviceId = "local-machine-audio",
            SourceId = "local-machine-audio",
            AudioDeviceName = "Game (TC-HELICON GoXLR)",
            AudioSourceKind = "wasapi-loopback",
            Paired = true,
            CaptureStreaming = true,
            EmptyPacketPolls = 12,
            CaptureSampleRate = 48000,
            CaptureChannels = 2,
            EndpointName = "Game (TC-HELICON GoXLR)",
            LastError = "GetNextPacketSize hr=0x88890004",
            Warning = "WASAPI capture is open on 'Game (TC-HELICON GoXLR)' ({0.0.0.00000000}.{e05ed7eb-60cc-4d05-82f9-341ed8b4e6b4}) but the endpoint has not produced loopback packets."
        });

        Assert.Equal(
            "Game (TC-HELICON GoXLR) (wasapi-loopback) -> local-machine-audio: loopback idle - play audio through this output or choose an input, 0 frames, 12 silent polls 48000 Hz/2 ch via Game (TC-HELICON GoXLR), GetNextPacketSize hr=0x88890004, issue: no loopback packets from selected output; play audio through that output or choose another source",
            status);
    }

    [Fact]
    public void FormatCaptureAudioSourceWarningForOperator_TruncatesUnexpectedWarnings()
    {
        var warning = new string('x', 180);

        var formatted = StudioViewModel.FormatCaptureAudioSourceWarningForOperator(warning);

        Assert.Equal(140, formatted.Length);
        Assert.EndsWith("...", formatted);
    }

    [Fact]
    public void FormatCaptureAudioSourceStatus_ShowsQueuedAndUnderrunEvidence()
    {
        var status = StudioViewModel.FormatCaptureAudioSourceStatus(new NativeMediaCoreCaptureAudioSource
        {
            CaptureDeviceId = "local-machine-audio",
            SourceId = "local-machine-audio",
            AudioDeviceName = "Desk Mix",
            AudioSourceKind = "wasapi-loopback",
            Paired = true,
            CaptureStreaming = true,
            CaptureFramesReceived = 960,
            CaptureFramesRendered = 480,
            CaptureQueuedFrames = 480,
            CaptureUnderrunCount = 2,
            CaptureSampleRate = 48000,
            CaptureChannels = 2,
            PeakDbfs = -12,
            RmsDbfs = -18,
            SignalPresent = true,
            EndpointName = "Speakers"
        });

        Assert.Equal(
            "Desk Mix (wasapi-loopback) -> local-machine-audio: streaming, 960 frames, rendered 480, queued 480, underruns 2, peak -12.0 dBFS, rms -18.0 dBFS 48000 Hz/2 ch via Speakers",
            status);
    }

    [Fact]
    public void FormatCaptureAudioSourceStatus_ShowsCaptureTimingEvidence()
    {
        var status = StudioViewModel.FormatCaptureAudioSourceStatus(new NativeMediaCoreCaptureAudioSource
        {
            CaptureDeviceId = "local-machine-audio",
            SourceId = "local-machine-audio",
            AudioDeviceName = "Desk Mix",
            AudioSourceKind = "wasapi-loopback",
            Paired = true,
            CaptureStreaming = true,
            CaptureFramesReceived = 960,
            CaptureStartedAtMs = 1000,
            CaptureLastFrameAtMs = 2450,
            CaptureLastFrameAgeMs = 38,
            CaptureStoppedAtMs = 3200,
            CaptureSampleRate = 48000,
            CaptureChannels = 2,
            PeakDbfs = -12,
            RmsDbfs = -18,
            SignalPresent = true,
            EndpointName = "Speakers"
        });

        Assert.Equal(
            "Desk Mix (wasapi-loopback) -> local-machine-audio: streaming, 960 frames, started 1000ms, last PCM 2450ms, age 38ms, stopped 3200ms, peak -12.0 dBFS, rms -18.0 dBFS 48000 Hz/2 ch via Speakers",
            status);
    }

    [Fact]
    public void BuildCaptureAudioSourceTelemetry_IncludesFreshnessAndQueueEvidence()
    {
        var method = typeof(StudioViewModel).GetMethod(
            "BuildCaptureAudioSourceTelemetry",
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
        Assert.NotNull(method);

        var telemetry = Assert.IsType<string>(method.Invoke(null,
        [
            new NativeMediaCoreCaptureAudioSources
            {
                Status = "warning",
                Summary = "capture stale",
                SourceCount = 1,
                StreamingCount = 1,
                Sources =
                [
                    new NativeMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = "local-machine-audio",
                        SourceId = "local-machine-audio",
                        AudioSourceKind = "wasapi-loopback",
                        Paired = true,
                        CaptureStreaming = true,
                        CaptureFramesReceived = 960,
                        CaptureFramesRendered = 480,
                        CaptureQueuedFrames = 240,
                        CaptureUnderrunCount = 2,
                        CaptureLastFrameAgeMs = 1500,
                        EmptyPacketPolls = 4,
                        SignalPresent = true,
                        PeakDbfs = -12,
                        RmsDbfs = -18,
                        CaptureSampleRate = 48000,
                        CaptureChannels = 2,
                        EndpointName = "Speakers",
                        Warning = "Audio capture PCM is stale"
                    }
                ]
            }
        ]));

        Assert.Contains("rendered=480", telemetry, StringComparison.Ordinal);
        Assert.Contains("queued=240", telemetry, StringComparison.Ordinal);
        Assert.Contains("underruns=2", telemetry, StringComparison.Ordinal);
        Assert.Contains("ageMs=1500", telemetry, StringComparison.Ordinal);
        Assert.Contains("warning=Audio capture PCM is stale", telemetry, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsMonitorPlaybackWorking()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 480
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON 480 | playback 480", status);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsStreamAndRecordingAudioProof()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 480
        };
        var senders = new NativeMediaCoreOutputSenderSession
        {
            Status = "live",
            ActiveSenderCount = 1,
            Senders =
            [
                new NativeMediaCoreOutputSender
                {
                    SenderId = "rtmp",
                    Destination = "rtmp",
                    Status = "live",
                    AudioFramesSent = 960,
                    AudioBytesSent = 7680,
                    AudioChannels = 2,
                    AudioSampleRate = 48000
                }
            ]
        };
        var recording = BuildRecordingSession(new NativeMediaCoreRecordingProof
        {
            AudioPacketsObserved = 2,
            AudioPresent = true,
            AudioSampleCount = 960,
            AudioChannels = 2,
            AudioSampleRate = 48000
        });

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture, senders, recording);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON 480 | playback 480 | stream audio 960 frames @ 48000 Hz | record audio 960 samples @ 48000 Hz", status);
    }

    [Fact]
    public void FormatAudioProofSummary_FlagsStreamAndRecordingAudioGaps()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 480
        };
        var senders = new NativeMediaCoreOutputSenderSession
        {
            Status = "live",
            ActiveSenderCount = 1,
            Senders =
            [
                new NativeMediaCoreOutputSender
                {
                    SenderId = "rtmp",
                    Destination = "rtmp",
                    Status = "live",
                    AudioFramesSent = 0
                }
            ]
        };
        var recording = BuildRecordingSession(new NativeMediaCoreRecordingProof
        {
            AudioPacketsObserved = 0,
            AudioPresent = false,
            AudioSampleCount = 0
        });

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture, senders, recording);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON 480 | playback 480 | stream audio none | record audio none | check stream audio; check recording audio", status);
    }

    [Fact]
    public void FormatAudioProofSummary_FlagsMissingRecordingProofTelemetry()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 480
        };
        var recording = BuildRecordingSession(null);

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture, recording: recording);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON 480 | playback 480 | record proof missing | check recording proof", status);
    }

    [Fact]
    public void BuildAudioMeterSourceSummary_LabelsCaptureSourceDrivingMeters()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            Participants =
            [
                new NativeMediaCoreParticipantAudioChannel
                {
                    ParticipantId = "local-machine-audio",
                    InputLevel = 72,
                    OutputLevel = 64,
                    GainDb = 0,
                    RmsDbfs = -18.2,
                    PeakDbfs = -7.4,
                    Status = "balanced"
                }
            ]
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio routed",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            Sources =
            [
                new NativeMediaCoreCaptureAudioSource
                {
                    CaptureDeviceId = "local-machine-audio",
                    SourceId = "local-machine-audio",
                    AudioDeviceName = "Desk Mix",
                    AudioSourceKind = "wasapi-loopback",
                    Paired = true,
                    CaptureStreaming = true,
                    CaptureFramesReceived = 960
                }
            ]
        };

        var status = StudioViewModel.BuildAudioMeterSourceSummary(audio, capture);

        Assert.Equal("Meters: Desk Mix [loopback] 64% peak -7.4 dBFS; monitor listens to MON bus.", status);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsProgramRoutingGap()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "PCM received",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "armed"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio ready",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 0,
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM none | MON none | playback none (armed, waiting for audio) | route source to PGM", status);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsMonitorRoutingGap()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "armed"
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio ready",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON none | playback none (armed, waiting for audio) | route source to MON", status);
    }

    [Fact]
    public void FormatAudioProofSummary_DoesNotReportMonitorRoutingGapWhenFallbackPlaybackWorks()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program audio routed",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio ready",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 0
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON none | playback 480", status);
    }

    [Fact]
    public void FormatAudioProofSummary_ShowsFallbackMonitorFramesSeparatelyFromRoutedMon()
    {
        var audio = new NativeMediaCoreAudioMixSession
        {
            Status = "live",
            Summary = "Program mix balanced",
            MixedFrameCount = 960,
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorFramesPlayed = 480
        };
        var capture = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            Summary = "Capture audio ready",
            SourceCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 960,
            RoutedMasterFrames = 960,
            RoutedMonitorFrames = 0,
            FallbackMonitorFrames = 480,
            MonitorFramesPlayed = 480
        };

        var status = StudioViewModel.FormatAudioProofSummary(audio, capture);

        Assert.Equal("sources 1/1 | PCM 960 | mix 960 | PGM 960 | MON fallback 480 | playback 480", status);
    }

    [Fact]
    public void BuildWaitingForPcmAudioMixChannel_ClearsStaleMeterSignalButPreservesOperatorControls()
    {
        var prior = new ParticipantAudioMix
        {
            ParticipantId = "local-machine-audio",
            OutputLevel = 74,
            GainDb = 2,
            ManualGainDb = 3.5,
            Pan = -0.25,
            Solo = true,
            NoiseSuppression = true,
            Muted = true,
            Status = "native-pcm",
            Lufs = -18.2,
            TruePeakDb = -4.5,
            PluginInserts = ["Compressor"]
        };

        var waiting = StudioViewModel.BuildWaitingForPcmAudioMixChannel("local-machine-audio", prior);

        Assert.Equal("local-machine-audio", waiting.ParticipantId);
        Assert.Equal(0, waiting.OutputLevel);
        Assert.Equal(0, waiting.GainDb);
        Assert.Equal(-120, waiting.Lufs);
        Assert.Equal(-120, waiting.TruePeakDb);
        Assert.Equal("waiting-for-pcm", waiting.Status);
        Assert.Equal(3.5, waiting.ManualGainDb);
        Assert.Equal(-0.25, waiting.Pan);
        Assert.True(waiting.Solo);
        Assert.True(waiting.NoiseSuppression);
        Assert.True(waiting.Muted);
        Assert.Equal(["Compressor"], waiting.PluginInserts);
    }

    [Fact]
    public void IsLocalAudioSourceConfigured_RejectsEnabledSourceWithoutSelectedDevice()
    {
        var configured = StudioViewModel.IsLocalAudioSourceConfigured(
            localAudioSourceEnabled: true,
            selectedLocalAudioCaptureDeviceId: "",
            audioCaptureDevices:
            [
                AudioDevice("loopback-1")
            ]);

        Assert.False(configured);
    }

    [Fact]
    public void IsLocalAudioSourceConfigured_RejectsStaleSelectedDevice()
    {
        var configured = StudioViewModel.IsLocalAudioSourceConfigured(
            localAudioSourceEnabled: true,
            selectedLocalAudioCaptureDeviceId: "missing",
            audioCaptureDevices:
            [
                AudioDevice("loopback-1")
            ]);

        Assert.False(configured);
    }

    [Fact]
    public void IsLocalAudioSourceConfigured_AcceptsEnabledDiscoveredDevice()
    {
        var configured = StudioViewModel.IsLocalAudioSourceConfigured(
            localAudioSourceEnabled: true,
            selectedLocalAudioCaptureDeviceId: "loopback-1",
            audioCaptureDevices:
            [
                AudioDevice("loopback-1")
            ]);

        Assert.True(configured);
    }

    [Fact]
    public void IsLocalAudioSourceConfigured_AcceptsSavedDefaultRenderLoopbackBeforeDiscovery()
    {
        var configured = StudioViewModel.IsLocalAudioSourceConfigured(
            localAudioSourceEnabled: true,
            selectedLocalAudioCaptureDeviceId: "default-render-loopback",
            audioCaptureDevices: []);

        Assert.True(configured);
    }

    [Fact]
    public void ResolveLocalAudioSourceDeviceId_PrefersDefaultLoopbackWhenSelectionIsEmpty()
    {
        var selected = StudioViewModel.ResolveLocalAudioSourceDeviceId(
            selectedDeviceId: "",
            [
                AudioDevice("mic-1", "wasapi-input", "Studio Mic", isDefault: true),
                AudioDevice("loopback-1", "wasapi-loopback", "Speakers"),
                AudioDevice("loopback-2", "wasapi-loopback", "Headphones", isDefault: true)
            ]);

        Assert.Equal("loopback-2", selected);
    }

    [Fact]
    public void ResolveLocalAudioSourceDeviceId_PreservesExplicitMicSelection()
    {
        var selected = StudioViewModel.ResolveLocalAudioSourceDeviceId(
            selectedDeviceId: "mic-1",
            [
                AudioDevice("mic-1", "wasapi-input", "Studio Mic"),
                AudioDevice("loopback-1", "wasapi-loopback", "Speakers")
            ]);

        Assert.Equal("mic-1", selected);
    }

    [Fact]
    public void ResolveLocalAudioSourceDeviceId_PreservesExplicitNonDefaultLoopback()
    {
        var selected = StudioViewModel.ResolveLocalAudioSourceDeviceId(
            selectedDeviceId: "chat",
            [
                AudioDevice("chat", "wasapi-loopback", "Chat"),
                AudioDevice("system", "wasapi-loopback", "System", isDefault: true)
            ]);

        Assert.Equal("chat", selected);
    }

    [Fact]
    public void ResolveAudioMonitorDeviceId_SelectsFirstAvailableRenderDevice()
    {
        var selected = StudioViewModel.ResolveAudioMonitorDeviceId(
            selectedDeviceId: "missing",
            [
                new AudioRenderDevice { Id = "render-2", NativeDeviceId = "native-render-2", Name = "Headphones B" },
                new AudioRenderDevice { Id = "render-1", NativeDeviceId = "native-render-1", Name = "Headphones A" }
            ]);

        Assert.Equal("render-1", selected);
    }

    [Fact]
    public void ResolveLocalAudioRoutingSourceLabel_UsesSelectedDeviceName()
    {
        var label = StudioViewModel.ResolveLocalAudioRoutingSourceLabel("Speakers (Realtek WASAPI loopback)");

        Assert.Equal("Local machine audio - Speakers (Realtek WASAPI loopback)", label);
    }

    [Fact]
    public void ResolveLocalAudioRoutingSourceLabel_ShowsMissingSelection()
    {
        var label = StudioViewModel.ResolveLocalAudioRoutingSourceLabel("");

        Assert.Equal("Local machine audio - No local audio input selected", label);
    }

    [Fact]
    public void FormatLocalAudioSourceStatus_WaitsForNativeEvidenceWhenSourceIsMissing()
    {
        var status = StudioViewModel.FormatLocalAudioSourceStatus(
            "loopback-game",
            "Game audio loopback",
            new NativeMediaCoreCaptureAudioSources
            {
                Status = "ready",
                Summary = "No local machine audio source yet."
            });

        Assert.Equal("Local source waiting for native PCM evidence - Game audio loopback", status);
    }

    [Fact]
    public void FormatLocalAudioSourceStatus_ShowsOpenStreamWithoutPcmFrames()
    {
        var status = StudioViewModel.FormatLocalAudioSourceStatus(
            "loopback-game",
            "Game audio loopback",
            new NativeMediaCoreCaptureAudioSources
            {
                Status = "warning",
                Summary = "1 source streaming, 0 PCM frames.",
                Sources =
                [
                    new NativeMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = "local-machine-audio",
                        AudioDeviceId = "loopback-game",
                        AudioDeviceName = "Game audio loopback",
                        AudioSourceKind = "wasapi-loopback",
                        CaptureStreaming = true,
                        CaptureFramesReceived = 0,
                        EmptyPacketPolls = 7,
                        CaptureSampleRate = 48000,
                        CaptureChannels = 2,
                        EndpointName = "Game output",
                        Warning = "Audio capture stream is open but no PCM frames have arrived."
                    }
                ],
                Warnings = ["local-machine-audio: Audio capture stream is open but no PCM frames have arrived."]
            });

        Assert.Contains("Local source Game audio loopback", status, StringComparison.Ordinal);
        Assert.Contains("loopback idle", status, StringComparison.Ordinal);
        Assert.Contains("7 silent polls", status, StringComparison.Ordinal);
        Assert.Contains("no PCM frames", status, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatLocalAudioSourceStatus_ShowsSilentPcmLevelEvidence()
    {
        var status = StudioViewModel.FormatLocalAudioSourceStatus(
            "loopback-game",
            "Game audio loopback",
            new NativeMediaCoreCaptureAudioSources
            {
                Status = "warning",
                Summary = "1 source streaming, silent PCM.",
                Sources =
                [
                    new NativeMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = "local-machine-audio",
                        AudioDeviceId = "loopback-game",
                        AudioDeviceName = "Game audio loopback",
                        AudioSourceKind = "wasapi-loopback",
                        CaptureStreaming = true,
                        CaptureFramesReceived = 960,
                        CaptureSampleRate = 48000,
                        CaptureChannels = 2,
                        PeakDbfs = -120,
                        RmsDbfs = -120,
                        SignalPresent = false,
                        EndpointName = "Game output",
                        Warning = "Audio capture is receiving silent PCM frames; check the selected endpoint or play audio through it."
                    }
                ],
                Warnings = ["local-machine-audio: Audio capture is receiving silent PCM frames."]
            });

        Assert.Contains("silent PCM", status, StringComparison.Ordinal);
        Assert.Contains("peak -120.0 dBFS", status, StringComparison.Ordinal);
        Assert.Contains("silent PCM from selected source", status, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatLocalAudioSourceRecommendation_NamesAlternateWhenLoopbackHasNoPackets()
    {
        var recommendation = StudioViewModel.FormatLocalAudioSourceRecommendation(
            "loopback-game",
            "Game audio loopback",
            [
                AudioDevice("loopback-game", "wasapi-loopback", "Game output"),
                AudioDevice("mic-1", "wasapi-input", "Studio Mic")
            ],
            new NativeMediaCoreCaptureAudioSources
            {
                Status = "warning",
                Summary = "1 source streaming, 0 PCM frames.",
                Sources =
                [
                    new NativeMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = "local-machine-audio",
                        AudioDeviceId = "loopback-game",
                        AudioDeviceName = "Game audio loopback",
                        AudioSourceKind = "wasapi-loopback",
                        CaptureStreaming = true,
                        CaptureFramesReceived = 0,
                        EndpointName = "Game output"
                    }
                ]
            });

        Assert.Contains("No loopback packets from Game output", recommendation, StringComparison.Ordinal);
        Assert.Contains("Play audio through that Windows output", recommendation, StringComparison.Ordinal);
        Assert.Contains("Try Studio Mic", recommendation, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatLocalAudioSourceRecommendation_ShowsSilentPcmAndAlternateSource()
    {
        var recommendation = StudioViewModel.FormatLocalAudioSourceRecommendation(
            "loopback-game",
            "Game audio loopback",
            [
                AudioDevice("loopback-game", "wasapi-loopback", "Game output"),
                AudioDevice("loopback-chat", "wasapi-loopback", "Chat output", isDefault: true)
            ],
            new NativeMediaCoreCaptureAudioSources
            {
                Status = "warning",
                Summary = "1 source streaming, silent PCM.",
                Sources =
                [
                    new NativeMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = "local-machine-audio",
                        AudioDeviceId = "loopback-game",
                        AudioDeviceName = "Game audio loopback",
                        AudioSourceKind = "wasapi-loopback",
                        CaptureStreaming = true,
                        CaptureFramesReceived = 960,
                        PeakDbfs = -120,
                        RmsDbfs = -120,
                        SignalPresent = false,
                        EndpointName = "Game output"
                    }
                ]
            });

        Assert.Contains("Selected source is producing silent PCM from Game output (-120.0 dBFS)", recommendation, StringComparison.Ordinal);
        Assert.Contains("Confirm Windows is playing to that endpoint", recommendation, StringComparison.Ordinal);
        Assert.Contains("Try Chat output", recommendation, StringComparison.Ordinal);
    }

    [Fact]
    public void FormatLocalAudioSourceRecommendation_ConfirmsLiveSignal()
    {
        var recommendation = StudioViewModel.FormatLocalAudioSourceRecommendation(
            "loopback-game",
            "Game audio loopback",
            [AudioDevice("loopback-game", "wasapi-loopback", "Game output")],
            new NativeMediaCoreCaptureAudioSources
            {
                Status = "ready",
                Summary = "1 source streaming, signal present.",
                Sources =
                [
                    new NativeMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = "local-machine-audio",
                        AudioDeviceId = "loopback-game",
                        AudioDeviceName = "Game audio loopback",
                        AudioSourceKind = "wasapi-loopback",
                        CaptureStreaming = true,
                        CaptureFramesReceived = 960,
                        PeakDbfs = -18.2,
                        RmsDbfs = -24,
                        SignalPresent = true,
                        EndpointName = "Game output"
                    }
                ]
            });

        Assert.Equal("Local audio is receiving signal from Game output at -18.2 dBFS.", recommendation);
    }

    [Fact]
    public void EnsureDefaultLocalAudioRoutingSends_AddsProgramAndMonitorBeforeMatrixRowExists()
    {
        var sends = StudioViewModel.EnsureDefaultLocalAudioRoutingSends(
            [],
            localAudioConfigured: true,
            localRoutingRowExists: false);

        Assert.Collection(
            sends,
            send => Assert.Equal(("local-machine-audio", "master", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-l", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-r", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "stream", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "mon", 0), (send.SourceId, send.BusId, send.GainDb)));
    }

    [Fact]
    public void EnsureDefaultLocalAudioRoutingSends_RestoresSilentLocalRowAfterMatrixRowExists()
    {
        var sends = StudioViewModel.EnsureDefaultLocalAudioRoutingSends(
            [],
            localAudioConfigured: true,
            localRoutingRowExists: true);

        Assert.Collection(
            sends,
            send => Assert.Equal(("local-machine-audio", "master", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-l", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-r", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "stream", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "mon", 0), (send.SourceId, send.BusId, send.GainDb)));
    }

    [Fact]
    public void EnsureDefaultLocalAudioRoutingSends_CompletesMissingProgramAndMonitorRoutes()
    {
        var sends = StudioViewModel.EnsureDefaultLocalAudioRoutingSends(
            [new MediaCoreAudioRoutingSendWire("local-machine-audio", "mon", -6)],
            localAudioConfigured: true,
            localRoutingRowExists: false);

        Assert.Collection(
            sends,
            send => Assert.Equal(("local-machine-audio", "mon", -6), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "master", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-l", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-r", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "stream", 0), (send.SourceId, send.BusId, send.GainDb)));
    }

    [Fact]
    public void EnsureDefaultLocalAudioRoutingSends_DoesNotDuplicateExistingLocalBusRoutes()
    {
        var sends = StudioViewModel.EnsureDefaultLocalAudioRoutingSends(
            [
                new MediaCoreAudioRoutingSendWire("local-machine-audio", "master", -3),
                new MediaCoreAudioRoutingSendWire("local-machine-audio", "pgm-l", -2),
                new MediaCoreAudioRoutingSendWire("local-machine-audio", "pgm-r", -2),
                new MediaCoreAudioRoutingSendWire("local-machine-audio", "stream", -1),
                new MediaCoreAudioRoutingSendWire("local-machine-audio", "mon", -6)
            ],
            localAudioConfigured: true,
            localRoutingRowExists: true);

        Assert.Collection(
            sends,
            send => Assert.Equal(("local-machine-audio", "master", -3), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-l", -2), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "pgm-r", -2), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "stream", -1), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("local-machine-audio", "mon", -6), (send.SourceId, send.BusId, send.GainDb)));
    }

    [Fact]
    public void EnsureDefaultZoomAudioRoutingSends_AddsAllBusesForEachParticipantAndPreservesOverrides()
    {
        var sends = StudioViewModel.EnsureDefaultZoomAudioRoutingSends(
            [new MediaCoreAudioRoutingSendWire("16778240", "master", -6)],
            ["16778240", "16785408", "16778240", ""]);

        // The operator's existing master override survives; every other bus is
        // filled in at unity for both (deduped) participants.
        Assert.Equal(10, sends.Count);
        Assert.Contains(sends, send => send.SourceId == "16778240" && send.BusId == "master" && send.GainDb == -6);
        foreach (var participantId in new[] { "16778240", "16785408" })
        {
            foreach (var busId in new[] { "pgm-l", "pgm-r", "stream", "mon" })
            {
                Assert.Contains(sends, send => send.SourceId == participantId && send.BusId == busId && send.GainDb == 0);
            }
        }
        Assert.Contains(sends, send => send.SourceId == "16785408" && send.BusId == "master" && send.GainDb == 0);
    }

    [Fact]
    public void EnsureDefaultZoomAudioRoutingSends_NoParticipantsLeavesSendsUntouched()
    {
        var existing = new List<MediaCoreAudioRoutingSendWire> { new("media", "master", 0) };
        var sends = StudioViewModel.EnsureDefaultZoomAudioRoutingSends(existing, []);
        Assert.Same(existing, sends);
    }

    [Fact]
    public void EnsureDefaultCaptureAudioRoutingSends_AddsProgramStreamAndMonitorRoutesForCaptureSources()
    {
        var sends = StudioViewModel.EnsureDefaultCaptureAudioRoutingSends(
            [],
            [
                new MediaCoreCaptureAudioSourceWire(
                    "uvc-01",
                    "mic-01",
                    "USB Mic",
                    0,
                    "wasapi-input",
                    @"\\\\?\\SWD#MMDEVAPI#mic-01",
                    "WASAPI",
                    false)
            ]);

        Assert.Collection(
            sends,
            send => Assert.Equal(("capture:uvc-01", "master", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("capture:uvc-01", "pgm-l", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("capture:uvc-01", "pgm-r", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("capture:uvc-01", "stream", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("capture:uvc-01", "mon", 0), (send.SourceId, send.BusId, send.GainDb)));
    }

    [Fact]
    public void EnsureDefaultCaptureAudioRoutingSends_PreservesExistingCaptureRoutesAndAddsMissingBuses()
    {
        var sends = StudioViewModel.EnsureDefaultCaptureAudioRoutingSends(
            [
                new MediaCoreAudioRoutingSendWire("capture:uvc-01", "master", -3),
                new MediaCoreAudioRoutingSendWire("local-machine-audio", "mon", -6)
            ],
            [
                new MediaCoreCaptureAudioSourceWire(
                    "uvc-01",
                    "mic-01",
                    "USB Mic",
                    0,
                    "wasapi-input",
                    @"\\\\?\\SWD#MMDEVAPI#mic-01",
                    "WASAPI",
                    false),
                new MediaCoreCaptureAudioSourceWire(
                    "local-machine-audio",
                    "loopback",
                    "System audio",
                    0,
                    "wasapi-loopback",
                    "default-render",
                    "WASAPI",
                    false)
            ]);

        Assert.Contains(sends, send => send.SourceId == "capture:uvc-01" && send.BusId == "master" && send.GainDb == -3);
        Assert.Contains(sends, send => send.SourceId == "capture:uvc-01" && send.BusId == "stream" && send.GainDb == 0);
        Assert.Contains(sends, send => send.SourceId == "capture:uvc-01" && send.BusId == "mon" && send.GainDb == 0);
        Assert.Contains(sends, send => send.SourceId == "local-machine-audio" && send.BusId == "mon" && send.GainDb == -6);
        Assert.Contains(sends, send => send.SourceId == "local-machine-audio" && send.BusId == "master" && send.GainDb == 0);
        Assert.Equal(10, sends.Count);
    }

    [Fact]
    public void EnsureDefaultMediaAudioRoutingSends_AddsProgramStreamAndMonitorRoutes()
    {
        var sends = StudioViewModel.EnsureDefaultMediaAudioRoutingSends(
            [new MediaCoreAudioRoutingSendWire("media", "mon", -6)],
            mediaAudioConfigured: true);

        Assert.Collection(
            sends,
            send => Assert.Equal(("media", "mon", -6), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("media", "master", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("media", "pgm-l", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("media", "pgm-r", 0), (send.SourceId, send.BusId, send.GainDb)),
            send => Assert.Equal(("media", "stream", 0), (send.SourceId, send.BusId, send.GainDb)));
    }

    [Fact]
    public void EnsureDefaultMediaAudioRoutingSends_LeavesMatrixUnchangedWithoutMedia()
    {
        var existing = new MediaCoreAudioRoutingSendWire("guest-1", "master", -3);

        var sends = StudioViewModel.EnsureDefaultMediaAudioRoutingSends(
            [existing],
            mediaAudioConfigured: false);

        Assert.Equal([existing], sends);
    }

    [Theory]
    [InlineData(-1, 0)]
    [InlineData(0.625, 0.63)]
    [InlineData(2, 1)]
    [InlineData(double.NaN, 0.75)]
    public void NormalizeAudioMonitorVolume_ClampsSavedOperatorVolume(double value, double expected)
    {
        Assert.Equal(expected, StudioViewModel.NormalizeAudioMonitorVolume(value));
    }

    [Fact]
    public void IsConfiguredCaptureAudioSource_RejectsPlaceholderSources()
    {
        var source = new MediaCoreCaptureAudioSourceWire(
            "uvc-01",
            AudioDeviceId: null,
            AudioDeviceName: null,
            AudioSyncOffsetMs: 0);

        Assert.False(StudioViewModel.IsConfiguredCaptureAudioSource(source));
    }

    [Theory]
    [InlineData("uvc-01", "mic-1", null, false)]
    [InlineData("uvc-01", null, @"\\?\SWD#MMDEVAPI#mic-1", false)]
    [InlineData("uvc-01", null, null, true)]
    [InlineData("local-machine-audio", null, null, false)]
    public void IsConfiguredCaptureAudioSource_AcceptsSourcesThatCanProducePcm(
        string captureDeviceId,
        string? audioDeviceId,
        string? nativeAudioDeviceId,
        bool embedded)
    {
        var source = new MediaCoreCaptureAudioSourceWire(
            captureDeviceId,
            audioDeviceId,
            AudioDeviceName: null,
            AudioSyncOffsetMs: 0,
            NativeAudioDeviceId: nativeAudioDeviceId,
            Embedded: embedded);

        Assert.True(StudioViewModel.IsConfiguredCaptureAudioSource(source));
    }

    [Fact]
    public void ResolveCaptureAudioAssignment_PreservesExistingValidAssignment()
    {
        var capture = BuildCaptureDevice("uvc-01", "Elgato HD60", "uvc");
        var assigned = new AudioCaptureDevice
        {
            Id = "mic-01",
            NativeDeviceId = @"\\?\SWD#MMDEVAPI#mic-01",
            Name = "USB Mic"
        };
        capture.AssignedAudioDeviceId = assigned.Id;

        var resolved = StudioViewModel.ResolveCaptureAudioAssignment(
            capture,
            new Dictionary<string, AudioCaptureDevice>(StringComparer.Ordinal) { [assigned.Id] = assigned },
            [assigned]);

        Assert.Same(assigned, resolved);
    }

    [Fact]
    public void ResolveCaptureAudioAssignment_AutoSelectsAvailableEmbeddedCaptureAudio()
    {
        var capture = BuildCaptureDevice("decklink-1", "DeckLink Mini Recorder", "blackmagic");
        var embedded = new AudioCaptureDevice
        {
            Id = "embedded-decklink-1",
            NativeDeviceId = "decklink-native",
            Name = "DeckLink Mini Recorder embedded audio",
            SourceKind = "embedded-capture-audio",
            DriverName = "Blackmagic DeckLink",
            LinkedCaptureDeviceId = capture.Id,
            IsAvailable = true
        };

        var resolved = StudioViewModel.ResolveCaptureAudioAssignment(
            capture,
            new Dictionary<string, AudioCaptureDevice>(StringComparer.Ordinal),
            [embedded]);

        Assert.Same(embedded, resolved);
    }

    [Fact]
    public void ResolveCaptureAudioAssignment_IgnoresUnavailableEmbeddedCaptureAudio()
    {
        var capture = BuildCaptureDevice("decklink-1", "DeckLink Mini Recorder", "blackmagic");
        var embedded = new AudioCaptureDevice
        {
            Id = "embedded-decklink-1",
            NativeDeviceId = "decklink-native",
            Name = "DeckLink Mini Recorder embedded audio",
            SourceKind = "embedded-capture-audio",
            DriverName = "Blackmagic DeckLink",
            LinkedCaptureDeviceId = capture.Id,
            IsAvailable = false
        };

        var resolved = StudioViewModel.ResolveCaptureAudioAssignment(
            capture,
            new Dictionary<string, AudioCaptureDevice>(StringComparer.Ordinal),
            [embedded]);

        Assert.Null(resolved);
    }

    [Theory]
    [InlineData(10, 50)]
    [InlineData(350.4, 350)]
    [InlineData(350.5, 351)]
    [InlineData(9999, 2000)]
    public void NormalizeLowerThirdTimingMs_ClampsOperatorTiming(double value, int expected)
    {
        Assert.Equal(expected, StudioViewModel.NormalizeLowerThirdTimingMs(value));
    }

    [Fact]
    public void NormalizeLowerThirdTimingMs_DefaultsNonFiniteOperatorTiming()
    {
        Assert.Equal(250, StudioViewModel.NormalizeLowerThirdTimingMs(double.NaN));
        Assert.Equal(250, StudioViewModel.NormalizeLowerThirdTimingMs(double.PositiveInfinity));
        Assert.Equal(250, StudioViewModel.NormalizeLowerThirdTimingMs(double.NegativeInfinity));
    }

    [Theory]
    [InlineData(0, 250, 250)]
    [InlineData(100, 250, 150)]
    [InlineData(249, 250, 1)]
    [InlineData(250, 250, 0)]
    [InlineData(500, 250, 0)]
    [InlineData(25, 10, 25)]
    [InlineData(25, 9999, 1975)]
    public void RemainingLowerThirdPhaseDelayMs_UsesCurrentNormalizedDuration(
        long elapsedMs,
        double currentDurationMs,
        int expected)
    {
        Assert.Equal(expected, StudioViewModel.RemainingLowerThirdPhaseDelayMs(elapsedMs, currentDurationMs));
    }

    [Theory]
    [InlineData(true, ProductionMode.Manual, false, false, false, true)]
    [InlineData(false, ProductionMode.SetAndForget, true, false, false, true)]
    [InlineData(false, ProductionMode.SetAndForget, true, true, false, false)]
    [InlineData(false, ProductionMode.SetAndForget, false, false, true, true)]
    [InlineData(false, ProductionMode.Manual, true, false, false, false)]
    public void ShouldEnableProgramLowerThird_LetsManualOutOverrideAutomation(
        bool manuallyEnabled,
        ProductionMode productionMode,
        bool automationLowerThirdsEnabled,
        bool automationSuppressed,
        bool graphicLowerThirdEnabled,
        bool expected)
    {
        Assert.Equal(
            expected,
            StudioViewModel.ShouldEnableProgramLowerThird(
                manuallyEnabled,
                productionMode,
                automationLowerThirdsEnabled,
                automationSuppressed,
                graphicLowerThirdEnabled));
    }

    [Theory]
    [InlineData(false, "LT in")]
    [InlineData(true, "LT out")]
    public void FormatStudioLowerThirdActionLabel_MakesFooterActionExplicit(bool isVisible, string expected)
    {
        Assert.Equal(expected, StudioViewModel.FormatStudioLowerThirdActionLabel(isVisible));
    }

    [Theory]
    [InlineData("building-in", "Building in")]
    [InlineData("building-out", "Building out")]
    [InlineData("on-air", "On")]
    [InlineData("", "On")]
    public void FormatLowerThirdPhaseLabel_UsesOperatorFacingLabels(string phase, string expected)
    {
        Assert.Equal(expected, StudioViewModel.FormatLowerThirdPhaseLabel(phase));
    }

    [Fact]
    public void FormatNativeLowerThirdStatus_UsesNativeOverlayEvidence()
    {
        var status = StudioViewModel.FormatNativeLowerThirdStatus(new NativeMediaCoreOverlayState
        {
            Status = "live",
            OverlayCount = 1,
            LowerThirdCount = 1,
            OnAirCount = 1,
            Summary = "1 lower-third overlay, 1 on-air, 0 building.",
            Overlays =
            [
                new NativeMediaCoreOverlayAssetState
                {
                    OverlayId = "key:lower-third",
                    Kind = "lower-third",
                    Position = "lower-third",
                    SourceId = "p2",
                    SourceName = "David Chen",
                    Title = "Chief Product Officer",
                    KeyPosition = "lower-left",
                    KeyPhase = "on-air",
                    Keyer = "downstream",
                    KeyProgress = 1,
                    BuildInMs = 350,
                    BuildOutMs = 275,
                    Visible = true
                }
            ]
        });

        Assert.Equal("Native: On for David Chen - Chief Product Officer; build 350 ms / out 275 ms.", status);
    }

    [Fact]
    public void FormatNativeMediaPlaybackStatus_ShowsProgramPlaybackKey()
    {
        var status = StudioViewModel.FormatNativeMediaPlaybackStatus(new NativeMediaCoreMediaPlaybackState
        {
            Status = "playing",
            MediaAssetId = "clip-intro",
            MediaAssetName = "Intro Sting",
            MediaAssetKind = "stinger",
            MediaAssetPath = @"C:\media\intro.mp4",
            MediaPlaybackKey = "program-take:3:media:clip-intro",
            Playing = true,
            Summary = "Playing Intro Sting with key program-take:3:media:clip-intro."
        });

        Assert.Equal("Native: Intro Sting playing; key program-take:3:media:clip-intro.", status);
    }

    [Fact]
    public void FormatNativeCoreRuntimeStatus_ShowsPathAndAudioCapabilities()
    {
        var status = StudioViewModel.FormatNativeCoreRuntimeStatus(
            @"C:\repo\native\build-dev\corevideo-native.exe",
            new NativeMediaCoreProfile
            {
                Name = "CoreVideo Native",
                Renderer = "d3d11",
                MaxProgramResolution = "1920x1080",
                MaxProgramFps = 60,
                MaxParticipantFeeds = 10,
                MaxIsoRecordings = 8,
                Capabilities = ["local-audio-capture", "audio-monitor-output"]
            });

        Assert.Contains(@"C:\repo\native\build-dev\corevideo-native.exe", status);
        Assert.Contains("CoreVideo Native", status);
        Assert.Contains("local audio on", status);
        Assert.Contains("monitor output on", status);
    }

    [Fact]
    public void FormatNativeCoreRuntimeStatus_ShowsHandshakePending()
    {
        var status = StudioViewModel.FormatNativeCoreRuntimeStatus(null, null);

        Assert.Equal("Native core: not resolved; profile waiting for handshake.", status);
    }

    [Fact]
    public void FormatNativeAudioRuntimeStatus_ShowsReadyWhenNativeAudioCapabilitiesExist()
    {
        var status = StudioViewModel.FormatNativeAudioRuntimeStatus(new NativeMediaCoreProfile
        {
            Name = "CoreVideo Native",
            Renderer = "d3d11",
            MaxProgramResolution = "1920x1080",
            MaxProgramFps = 60,
            MaxParticipantFeeds = 10,
            MaxIsoRecordings = 8,
            Capabilities = ["audio-mixer", "local-audio-capture", "audio-monitor-output"]
        });

        Assert.Equal(
            "Audio runtime: ready - native mixer, WASAPI capture, and monitor output are enabled.",
            status);
    }

    [Fact]
    public void FormatNativeAudioRuntimeStatus_NamesMissingAudioCapabilities()
    {
        var status = StudioViewModel.FormatNativeAudioRuntimeStatus(new NativeMediaCoreProfile
        {
            Name = "CoreVideo Native",
            Renderer = "d3d11",
            MaxProgramResolution = "1920x1080",
            MaxProgramFps = 60,
            MaxParticipantFeeds = 10,
            MaxIsoRecordings = 8,
            Capabilities = ["audio-mixer"]
        });

        Assert.Equal(
            "Audio runtime: blocked - native profile missing local-audio-capture, audio-monitor-output.",
            status);
    }

    [Fact]
    public void FormatNativeAudioRuntimeStatus_ShowsHandshakePending()
    {
        Assert.Equal(
            "Audio runtime: waiting for native profile handshake.",
            StudioViewModel.FormatNativeAudioRuntimeStatus(null));
    }

    [Theory]
    [InlineData(0, 0.5)]
    [InlineData(4.14, 4.1)]
    [InlineData(4.15, 4.2)]
    [InlineData(120, 80)]
    public void NormalizeStreamTargetBitrateMbps_ClampsOperatorBitrate(double value, double expected)
    {
        Assert.Equal(expected, StudioViewModel.NormalizeStreamTargetBitrateMbps(value));
    }

    [Theory]
    [InlineData(6, "6.0 Mbps (6000 kbps)")]
    [InlineData(4.15, "4.2 Mbps (4200 kbps)")]
    [InlineData(double.NaN, "8.2 Mbps (8200 kbps)")]
    public void FormatStreamBitrateSummary_ShowsOperatorMbpsAndKbps(double value, string expected)
    {
        Assert.Equal(expected, StudioViewModel.FormatStreamBitrateSummary(value));
    }

    [Theory]
    [InlineData(6, "6 Mbps")]
    [InlineData(4.15, "4.2 Mbps")]
    [InlineData(double.NaN, "8.2 Mbps")]
    public void FormatTransportStreamConfigLabel_ShowsIdleFooterBitrate(double value, string expected)
    {
        Assert.Equal(expected, StudioViewModel.FormatTransportStreamConfigLabel(value));
    }

    [Theory]
    [InlineData("mp4", 18, "MP4 18 Mbps")]
    [InlineData("mov", 4.15, "MOV 4.2 Mbps")]
    [InlineData("bad", double.NaN, "MP4 8.2 Mbps")]
    public void FormatTransportRecordingConfigLabel_ShowsIdleFooterFormatAndBitrate(
        string format,
        double value,
        string expected)
    {
        Assert.Equal(expected, StudioViewModel.FormatTransportRecordingConfigLabel(format, value));
    }

    private static NativeMediaCoreRecordingSession BuildRecordingSession(NativeMediaCoreRecordingProof? proof) =>
        new()
        {
            SessionId = "recording",
            Active = true,
            Status = "recording",
            WriterStatus = "writing",
            StartedAtMs = 1000,
            ElapsedMs = 2000,
            TargetFolder = "Recordings",
            FilenamePrefix = "show",
            Format = "mp4",
            Quality = "high",
            EstimatedDiskRateMBps = 4.99,
            ProgramPath = "Recordings/show-program-0.mp4",
            Proof = proof
        };

    private static CaptureDevice BuildCaptureDevice(string id, string name, string vendor) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Vendor = vendor,
            Name = name,
            Inputs = [new CaptureDeviceInput { Id = "default", Label = "Default" }],
            SelectedInputId = "default"
        };

    private static AudioCaptureDevice AudioDevice(
        string id,
        string sourceKind = "wasapi-loopback",
        string name = "Loopback",
        bool isDefault = false) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = name,
            SourceKind = sourceKind,
            IsDefault = isDefault
        };
}
