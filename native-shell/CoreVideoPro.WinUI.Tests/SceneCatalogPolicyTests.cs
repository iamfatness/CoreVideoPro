using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// #1070: catalog scenes (Intro through Closing) are deletable and restorable.
public sealed class SceneCatalogPolicyTests
{
    private static List<Scene> Catalog() => ProductionCatalog.Scenes.ToList();

    [Theory]
    [InlineData("intro")]
    [InlineData("closing")]
    [InlineData("custom-abc12345")]
    public void AnyIdleSceneIsRemovableWhenAnotherVisibleSceneRemains(string sceneId)
    {
        Assert.Null(SceneCatalogPolicy.RemovalRefusal(sceneId, activeSceneId: "speaker-slides", visibleSceneCount: 5, isInternalScene: false));
    }

    [Fact]
    public void TheOnAirSceneIsNeverRemovable()
    {
        Assert.Equal(SceneCatalogPolicy.OnAirRefusal,
            SceneCatalogPolicy.RemovalRefusal("intro", activeSceneId: "intro", visibleSceneCount: 5, isInternalScene: false));
    }

    [Fact]
    public void TheLastVisibleSceneIsNeverRemovable()
    {
        Assert.Equal(SceneCatalogPolicy.LastSceneRefusal,
            SceneCatalogPolicy.RemovalRefusal("custom-only", activeSceneId: "multiview-solo-a", visibleSceneCount: 1, isInternalScene: false));
        // Hidden multiview-solo scenes do not count and stay removable regardless.
        Assert.Null(SceneCatalogPolicy.RemovalRefusal("multiview-solo-a", activeSceneId: "intro", visibleSceneCount: 1, isInternalScene: true));
    }

    [Fact]
    public void RemovedCatalogIdsAreTheCatalogEntriesMissingFromTheRail()
    {
        var scenes = Catalog();
        scenes.RemoveAll(scene => scene.Id is "intro" or "closing");
        scenes.Add(new Scene { Id = "custom-1", Name = "Mine", Layout = "host-focus" });

        Assert.Equal(["intro", "closing"], SceneCatalogPolicy.RemovedCatalogSceneIds(scenes));
        Assert.Empty(SceneCatalogPolicy.RemovedCatalogSceneIds(Catalog()));
    }

    [Fact]
    public void PersistedRemovalsApplyInCatalogOrderAndIgnoreUnknownIds()
    {
        var removals = SceneCatalogPolicy.RemovalsToApply(Catalog(), ["closing", "nope", "", "intro"], _ => false);
        Assert.Equal(["intro", "closing"], removals);
    }

    [Fact]
    public void PersistedRemovalsNeverEmptyTheVisibleRail()
    {
        var all = ProductionCatalog.Scenes.Select(scene => scene.Id).ToList();
        var removals = SceneCatalogPolicy.RemovalsToApply(Catalog(), all, _ => false);
        Assert.Equal(4, removals.Count);
        Assert.DoesNotContain("closing", removals);

        // A persisted custom scene counts as a survivor, so all five catalog scenes may go.
        var withCustom = Catalog();
        withCustom.Add(new Scene { Id = "custom-1", Name = "Mine", Layout = "host-focus" });
        Assert.Equal(5, SceneCatalogPolicy.RemovalsToApply(withCustom, all, _ => false).Count);

        // Internal scenes are not survivors.
        var withInternal = Catalog();
        withInternal.Add(new Scene { Id = "multiview-solo-a", Name = "Solo", Layout = "host-focus" });
        Assert.Equal(4, SceneCatalogPolicy.RemovalsToApply(withInternal, all, id => id.StartsWith("multiview-solo-", StringComparison.Ordinal)).Count);
    }

    [Fact]
    public void MissingCatalogScenesComeBackAtTheirCatalogPosition()
    {
        var scenes = Catalog();
        scenes.RemoveAll(scene => scene.Id is "interview" or "closing");
        scenes.Add(new Scene { Id = "custom-1", Name = "Mine", Layout = "host-focus" });

        var missing = SceneCatalogPolicy.MissingCatalogScenes(scenes);
        foreach (var (scene, index) in missing)
        {
            scenes.Insert(index, scene);
        }

        Assert.Equal(["intro", "interview", "speaker-slides", "panel", "closing", "custom-1"], scenes.Select(scene => scene.Id));
        Assert.Empty(SceneCatalogPolicy.MissingCatalogScenes(Catalog()));
    }

    [Fact]
    public void RecommendedSceneFallsBackToTheFirstSceneWhenItsCatalogSceneIsGone()
    {
        var scenes = Catalog();
        scenes.RemoveAll(scene => scene.Id == "panel");
        Assert.Equal("intro", SceneCatalogPolicy.ResolveRecommendedSceneId(scenes, "panel"));
        Assert.Equal("interview", SceneCatalogPolicy.ResolveRecommendedSceneId(scenes, "interview"));
        Assert.Equal("panel", SceneCatalogPolicy.ResolveRecommendedSceneId([], "panel"));
    }
}
