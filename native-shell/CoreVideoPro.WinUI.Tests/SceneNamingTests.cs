using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SceneNamingTests
{
    private static readonly Scene[] Scenes = [new() { Id = "intro", Name = "Intro" }, new() { Id = "panel", Name = "Panel" }];

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData(" \t\r\n ")]
    [InlineData("  INTRO  ")]
    public void CreationRejectsBlankAndCaseInsensitiveDuplicateNames(string? name) =>
        Assert.NotNull(SceneNamingPolicy.Validate(Scenes, null, name));

    [Fact]
    public void RenameAllowsItsOwnNameAndUnicodeButRejectsAnotherScenesName()
    {
        Assert.Null(SceneNamingPolicy.Validate(Scenes, "intro", "  INTRO  "));
        Assert.Null(SceneNamingPolicy.Validate(Scenes, "intro", "  主題 – Montréal  "));
        Assert.NotNull(SceneNamingPolicy.Validate(Scenes, "intro", "  panel "));
    }

    [Fact]
    public void LegacyPreferencesWithoutNameOverridesKeepExistingCustomNames()
    {
        var restored = ProductionOutputPreferencesSerializer.Deserialize("""
            {"Version":16,"CustomScenes":[{"Id":"custom-one","Name":"Existing show","Layout":"two-up","Routes":[]}]}
            """, out var migrated)!;
        Assert.True(migrated);
        Assert.Equal(17, restored.Version);
        Assert.Empty(restored.SceneNames);
        Assert.Equal("Existing show", Assert.Single(restored.CustomScenes).Name);
    }

    [Fact]
    public void CatalogAndCustomNameMetadataRoundTripsWithoutChangingSceneIdentity()
    {
        var preferences = new ProductionOutputPreferences
        {
            SceneNames = new() { ["intro"] = "Welcome desk", ["custom-one"] = "主題 – Montréal" },
            CustomScenes = [new() { Id = "custom-one", Name = "主題 – Montréal", Layout = "two-up" }]
        };
        var restored = ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(preferences))!;
        Assert.Equal("Welcome desk", restored.SceneNames["intro"]);
        Assert.Equal("主題 – Montréal", restored.SceneNames["custom-one"]);
        Assert.Equal("custom-one", Assert.Single(restored.CustomScenes).Id);
        Assert.Equal("two-up", restored.CustomScenes[0].Layout);
    }
}
