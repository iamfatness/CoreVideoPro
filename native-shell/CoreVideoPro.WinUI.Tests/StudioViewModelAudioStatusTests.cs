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
    public void FormatStreamingFailureStatus_LeadsWithFfmpegActionAndRemovesMediaCorePrefix()
    {
        var status = StudioViewModel.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("media-core sync failed: start-program-output failed. ffmpeg.exe was not found in C:\\ffmpeg\\bin."));

        Assert.StartsWith(
            "Streaming start failed: FFmpeg is not ready. Choose the FFmpeg bin folder in Settings > Streaming.",
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
    [InlineData(0, 0.5)]
    [InlineData(4.14, 4.1)]
    [InlineData(4.15, 4.2)]
    [InlineData(120, 80)]
    public void NormalizeStreamTargetBitrateMbps_ClampsOperatorBitrate(double value, double expected)
    {
        Assert.Equal(expected, StudioViewModel.NormalizeStreamTargetBitrateMbps(value));
    }

    private static AudioCaptureDevice AudioDevice(string id) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = "Loopback"
        };
}
