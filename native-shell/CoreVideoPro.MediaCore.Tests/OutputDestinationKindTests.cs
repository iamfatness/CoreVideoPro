using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class OutputDestinationKindTests
{
    [Theory]
    [InlineData("ndi", true)]
    [InlineData("ndi-graphics", true)]
    [InlineData("rtmp", false)]
    [InlineData(null, false)]
    public void IsNdi(string? id, bool expected) => Assert.Equal(expected, OutputDestinationKind.IsNdi(id));

    [Theory]
    [InlineData("rtmp", true)]
    [InlineData("srt", true)]
    [InlineData("hls", true)]
    [InlineData("ndi", false)]
    [InlineData("ndi-graphics", false)]
    [InlineData("recording", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void IsStream(string? id, bool expected) => Assert.Equal(expected, OutputDestinationKind.IsStream(id));
}
