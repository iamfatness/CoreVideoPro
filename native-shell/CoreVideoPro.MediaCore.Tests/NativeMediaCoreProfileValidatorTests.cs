using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeMediaCoreProfileValidatorTests
{
    [Fact]
    public void ValidatesProductionProfileAsReady()
    {
        var validation = NativeMediaCoreProfileValidator.Validate(new NativeMediaCoreProfile
        {
            Name = "Production",
            Renderer = "direct3d11",
            MaxProgramResolution = "3840x2160",
            MaxProgramFps = 60,
            MaxParticipantFeeds = 8,
            MaxIsoRecordings = 8,
            Capabilities = NativeMediaCoreProfileValidator.RequiredMvpCapabilities.ToList()
        });

        Assert.True(validation.Ready);
        Assert.Empty(validation.MissingCapabilities);
    }

    [Fact]
    public void FlagsSoftwareRendererAsNotReady()
    {
        var validation = NativeMediaCoreProfileValidator.Validate(new NativeMediaCoreProfile
        {
            Name = "Software",
            Renderer = "software",
            MaxProgramResolution = "3840x2160",
            MaxProgramFps = 60,
            MaxParticipantFeeds = 8,
            MaxIsoRecordings = 8,
            Capabilities = NativeMediaCoreProfileValidator.RequiredMvpCapabilities.ToList()
        });

        Assert.False(validation.Ready);
        Assert.Contains(validation.Warnings, warning => warning.Contains("Software rendering"));
    }

    [Fact]
    public void FlagsMissingLocalAudioCapabilitiesAsNotReady()
    {
        var capabilities = NativeMediaCoreProfileValidator.RequiredMvpCapabilities
            .Where(capability => capability is not "local-audio-capture" and not "audio-monitor-output")
            .ToList();

        var validation = NativeMediaCoreProfileValidator.Validate(new NativeMediaCoreProfile
        {
            Name = "No WASAPI",
            Renderer = "direct3d11",
            MaxProgramResolution = "3840x2160",
            MaxProgramFps = 60,
            MaxParticipantFeeds = 8,
            MaxIsoRecordings = 8,
            Capabilities = capabilities
        });

        Assert.False(validation.Ready);
        Assert.Contains("local-audio-capture", validation.MissingCapabilities);
        Assert.Contains("audio-monitor-output", validation.MissingCapabilities);
    }
}
