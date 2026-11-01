using Xunit;

namespace CoreVideoPro.Control.Tests;

// #1070 review: scene.select used to answer ok for an unknown id while the shell only wrote a
// status line. It is now refuse-closed like scene.cueWithRoutes.
public sealed class SceneSelectArgsTests
{
    private static readonly string[] Known = ["intro", "interview", "custom-1a2b3c4d"];

    [Theory]
    [InlineData("intro")]
    [InlineData("custom-1a2b3c4d")]
    public void KnownSceneIsAccepted(string sceneId)
    {
        Assert.Null(SceneSelectArgs.Refuse(sceneId, Known));
    }

    [Theory]
    [InlineData("closing")]
    [InlineData("Intro")]
    [InlineData("")]
    [InlineData(" ")]
    [InlineData(null)]
    public void UnknownBlankOrDifferentlyCasedSceneIsRefused(string? sceneId)
    {
        var error = SceneSelectArgs.Refuse(sceneId, Known);
        Assert.NotNull(error);
        Assert.StartsWith("Unknown scene '", error);
    }
}
