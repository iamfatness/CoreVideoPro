using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
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

        Assert.Equal("Stream settings failed", StudioViewModel.FormatOutputStatusBrief(fullStatus));
        Assert.True(StudioViewModel.ShouldShowOutputStatusDetails(fullStatus));
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

        Assert.Equal("Monitor 480 playback frames - Studio Headphones - playback via mixer monitor bus; no routed MON bus", status);
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

        Assert.Equal("960 mixed frames - monitor 480 playback frames - playback via mixer monitor bus; no routed MON bus", status);
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

        Assert.Equal("sources 1/1 | PCM none | mix none | PGM none | MON none | playback off | check source PCM - Desk Mix (loopback, streaming, no frames)", status);
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

        Assert.Equal("Meters: Desk Mix 64% peak -7.4 dBFS; monitor listens to MON bus.", status);
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
            send => Assert.Equal(("local-machine-audio", "mon", 0), (send.SourceId, send.BusId, send.GainDb)));
    }

    [Fact]
    public void EnsureDefaultLocalAudioRoutingSends_DoesNotDuplicateExplicitLocalRoutes()
    {
        var sends = StudioViewModel.EnsureDefaultLocalAudioRoutingSends(
            [new MediaCoreAudioRoutingSendWire("local-machine-audio", "mon", -6)],
            localAudioConfigured: true,
            localRoutingRowExists: false);

        Assert.Single(sends);
        Assert.Equal("mon", sends[0].BusId);
        Assert.Equal(-6, sends[0].GainDb);
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
