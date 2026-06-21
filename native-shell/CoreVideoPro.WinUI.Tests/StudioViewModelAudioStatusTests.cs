using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioViewModelAudioStatusTests
{
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
}
