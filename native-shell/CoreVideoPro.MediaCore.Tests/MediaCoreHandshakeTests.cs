using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreHandshakeTests
{
    [Fact]
    public void IsUnsolicitedBootstrapHandshake_AcceptsOnlyStartupLine()
    {
        using var bootstrap = JsonDocument.Parse(
            """{"id":"handshake","ok":true,"type":"handshake","profile":{"name":"CoreVideo Pro Native Media Core"}}""");
        using var explicitHandshake = JsonDocument.Parse(
            """{"id":"core-1","ok":true,"type":"handshake","profile":{"name":"CoreVideo Pro Native Media Core"}}""");

        Assert.True(MediaCoreHandshakeRules.IsUnsolicitedBootstrapHandshake(bootstrap.RootElement));
        Assert.False(MediaCoreHandshakeRules.IsUnsolicitedBootstrapHandshake(explicitHandshake.RootElement));
    }
}