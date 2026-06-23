using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
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

    private static AudioCaptureDevice AudioDevice(string id) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = "Loopback"
        };
}
