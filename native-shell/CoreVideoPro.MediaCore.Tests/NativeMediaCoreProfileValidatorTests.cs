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
            MaxIsoRecordings = 4,
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
            MaxIsoRecordings = 4,
            Capabilities = NativeMediaCoreProfileValidator.RequiredMvpCapabilities.ToList()
        });

        Assert.False(validation.Ready);
        Assert.Contains(validation.Warnings, warning => warning.Contains("Software rendering"));
    }
}