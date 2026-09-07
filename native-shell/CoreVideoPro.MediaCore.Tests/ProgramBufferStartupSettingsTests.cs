using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ProgramBufferStartupSettingsTests
{
    [Theory]
    [InlineData(2, "2")]
    [InlineData(3, "3")]
    [InlineData(0, "3")]
    [InlineData(-1, "3")]
    [InlineData(4, "3")]
    public void StartupExportsOnlySupportedDepthAndOverridesInheritedValue(int frames, string expected)
    {
        var startup = new ProgramBufferStartupSettings();
        startup.Configure(frames);
        var environment = new Dictionary<string, string> { [ProgramBufferPreference.EnvironmentVariable] = "999" };
        startup.ApplyTo(environment);
        Assert.Equal(expected, environment[ProgramBufferPreference.EnvironmentVariable]);
    }

    [Fact]
    public void DefaultStartupUsesThreeFrames()
    {
        var environment = new Dictionary<string, string>();
        new ProgramBufferStartupSettings().ApplyTo(environment);
        Assert.Equal("3", environment[ProgramBufferPreference.EnvironmentVariable]);
    }

    [Fact]
    public void RecoveryRetainsFrozenStartupDepthAndRejectsLiveReconfiguration()
    {
        var startup = new ProgramBufferStartupSettings();
        startup.Configure(2);
        startup.ApplyTo(new Dictionary<string, string>());
        Assert.Throws<InvalidOperationException>(() => startup.Configure(3));
        var recoveryEnvironment = new Dictionary<string, string>();
        startup.ApplyTo(recoveryEnvironment);
        Assert.Equal("2", recoveryEnvironment[ProgramBufferPreference.EnvironmentVariable]);
    }
}
