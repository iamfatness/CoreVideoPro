using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>The Studio sidebar's Hide chip sent the instance id as the ONLY argument and the
/// surface read it from position 2, so every sidebar Hide was refused as "That playlist row does
/// not exist" (silently: the compact control has no status line). Both spellings must hide.</summary>
public sealed class StudioControlSurfacePlayOutArgsTests
{
    [Fact]
    public void InstanceIdAsTheOnlyArgumentIsAnInstanceHide()
        => Assert.Equal("playlist-guest-0123456789ab", StudioControlSurface.PlayOutInstanceId(new object?[] { "playlist-guest-0123456789ab" }));

    [Fact]
    public void InstanceIdInTheRegistrysThirdSlotStillWins()
        => Assert.Equal("playlist-title", StudioControlSurface.PlayOutInstanceId(new object?[] { "guest", "media:x", "playlist-title" }));

    [Theory]
    [InlineData("guest")]
    [InlineData("title")]
    public void ARowIdIsNotAnInstanceId(string rowId)
        => Assert.Equal(string.Empty, StudioControlSurface.PlayOutInstanceId(new object?[] { rowId, "media:media-83843756123d" }));

    [Fact]
    public void NoArgumentsIsNotAnInstanceId()
        => Assert.Equal(string.Empty, StudioControlSurface.PlayOutInstanceId(System.Array.Empty<object?>()));
}
