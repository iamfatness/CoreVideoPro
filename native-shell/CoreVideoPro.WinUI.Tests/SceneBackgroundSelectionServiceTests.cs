using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SceneBackgroundSelectionServiceTests
{
    [Fact]
    public void ResolveSelectedAssetId_ReturnsNoneWhenSceneHasNoBackground()
    {
        var selected = SceneBackgroundSelectionService.ResolveSelectedAssetId(
            "scene-a",
            new Dictionary<string, string>(),
            Find,
            IsVisual);

        Assert.Equal(SceneBackgroundSelectionService.NoBackgroundValue, selected);
    }

    [Fact]
    public void ApplySelection_StoresVisualBackgroundForScene()
    {
        var backgrounds = new Dictionary<string, string>();

        var result = SceneBackgroundSelectionService.ApplySelection(
            "scene-a",
            "video-bg",
            backgrounds,
            Find,
            IsVisual);

        Assert.True(result.HasBackground);
        Assert.Equal("video-bg", backgrounds["scene-a"]);
        Assert.Equal("Video Background", result.AssetName);
    }

    [Theory]
    [InlineData("")]
    [InlineData(SceneBackgroundSelectionService.NoBackgroundValue)]
    [InlineData("audio-bed")]
    [InlineData("missing")]
    public void ApplySelection_ClearsInvalidOrNoneSelection(string selectedAssetId)
    {
        var backgrounds = new Dictionary<string, string>
        {
            ["scene-a"] = "video-bg"
        };

        var result = SceneBackgroundSelectionService.ApplySelection(
            "scene-a",
            selectedAssetId,
            backgrounds,
            Find,
            IsVisual);

        Assert.False(result.HasBackground);
        Assert.False(backgrounds.ContainsKey("scene-a"));
    }

    [Fact]
    public void PruneMissingBackgrounds_RemovesMissingAndNonVisualAssignments()
    {
        var backgrounds = new Dictionary<string, string>
        {
            ["scene-a"] = "video-bg",
            ["scene-b"] = "audio-bed",
            ["scene-c"] = "missing"
        };

        SceneBackgroundSelectionService.PruneMissingBackgrounds(backgrounds, Find, IsVisual);

        Assert.Equal("video-bg", backgrounds["scene-a"]);
        Assert.False(backgrounds.ContainsKey("scene-b"));
        Assert.False(backgrounds.ContainsKey("scene-c"));
    }

    private static MediaAsset? Find(string assetId) => assetId switch
    {
        "video-bg" => new MediaAsset
        {
            Id = "video-bg",
            Name = "Video Background",
            Kind = "video",
            FilePath = @"C:\media\background.mp4"
        },
        "audio-bed" => new MediaAsset
        {
            Id = "audio-bed",
            Name = "Audio Bed",
            Kind = "audio-bed",
            FilePath = @"C:\media\bed.wav"
        },
        _ => null
    };

    private static bool IsVisual(MediaAsset asset) =>
        asset.FilePath.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase) ||
        asset.FilePath.EndsWith(".png", StringComparison.OrdinalIgnoreCase);
}
