using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ScenePersistenceServiceTests
{
    [Fact]
    public void SavedTilesWithoutSettingsRestoresWallDespiteStaleActiveSpeakerRoute()
    {
        var preferences = ProductionOutputPreferencesSerializer.Deserialize("""
            {"Version":11,"CustomScenes":[{"Id":"custom-74d5e390","Name":"CoreVideo Tiles",
            "Layout":"dynamic-gallery","DynamicGallery":null,
            "Routes":[{"Id":"stale-route","Mode":"active-speaker","AudioRole":"mix"}]}]}
            """);
        var persisted = Assert.Single(preferences!.CustomScenes);
        var restored = ScenePersistenceService.SceneFromPersisted(persisted);
        Assert.NotNull(restored.DynamicGallery);
        Assert.Equal(16, restored.DynamicGallery.MaxTiles);
        Assert.Equal("Auto-reflow Zoom gallery", restored.Automation);
        var payload = TilesLayerPayloadBuilder.Build(restored,
            [new Participant { Id = "42", Name = "Guest", Health = FeedHealth.Live }]);
        Assert.NotNull(payload);
        Assert.Equal(["zoom:42"], payload.Members);
        // Restoration does not mutate the saved DTO or pretend its stale route
        // is a wall member. Production/preview wire construction suppresses routes
        // whenever the restored scene has DynamicGallery settings.
        Assert.Null(persisted.DynamicGallery);
        Assert.Single(persisted.Routes);
    }

    [Theory]
    [InlineData("host-focus")]
    [InlineData("full")]
    public void OrdinaryLayoutWithoutGallerySettingsRemainsOrdinary(string layout)
    {
        var restored = ScenePersistenceService.SceneFromPersisted(new PersistedScene
            { Id = "custom-normal", Name = "Normal", Layout = layout });
        Assert.Null(restored.DynamicGallery);
        Assert.Equal("Custom canvas", restored.Automation);
        Assert.Null(TilesLayerPayloadBuilder.Build(restored, []));
    }

    [Fact]
    public void SceneRoundTripsThroughPersistedDtoAndJson()
    {
        var scene = new Scene
        {
            Id = "custom-abc12345",
            Name = "Interview wide",
            Layout = "dynamic-gallery",
            DynamicGallery = new DynamicGallerySettings
            {
                MaxTiles = 8,
                AutoFill = false,
                ManualSlots = ["zoom:42", null, "zoom:99"],
                ExcludedSourceIds = ["zoom:17"],
                TileAspect = "4:3",
                BorderShape = "rounded",
                BorderColor = "#FF8800",
                BorderThickness = 4,
                GlowSize = 12
            }
        };
        var routes = new List<SourceRoute>
        {
            new()
            {
                Id = "custom-abc12345-route-0",
                Mode = SourceRouteMode.Fixed,
                AudioRole = SourceAudioRole.Mix,
                ShowInputSlotNumber = 2,
                CanvasRect = new NormalizedCanvasRect { X = 0.1, Y = 0.2, Width = 0.4, Height = 0.5 },
                FitMode = "fit",
                BorderStyle = "solid",
                BorderColor = "#FF8800",
                BorderThickness = 4,
                SourceScale = 1.5,
                SourceOffsetX = 0.25,
                SourceOffsetY = -0.1,
                Opacity = 0.6,
                ZIndex = 1
            },
            new()
            {
                Id = "custom-abc12345-route-1",
                Mode = SourceRouteMode.CaptureDevice,
                AudioRole = SourceAudioRole.Isolated,
                CaptureDeviceId = "uvc-01",
                ZIndex = 0
            }
        };

        var persisted = ScenePersistenceService.ToPersisted(scene, routes);
        // Through JSON, exactly as the preferences store writes/reads it.
        var json = ProductionOutputPreferencesSerializer.Serialize(
            new ProductionOutputPreferences { CustomScenes = [persisted] });
        var reloaded = ProductionOutputPreferencesSerializer.Deserialize(json);

        Assert.NotNull(reloaded);
        var restoredScene = Assert.Single(reloaded!.CustomScenes);
        Assert.Equal("custom-abc12345", restoredScene.Id);
        Assert.Equal("Interview wide", restoredScene.Name);
        var restoredGallery = ScenePersistenceService.SceneFromPersisted(restoredScene).DynamicGallery;
        Assert.NotNull(restoredGallery);
        Assert.Equal(8, restoredGallery!.MaxTiles);
        Assert.False(restoredGallery.AutoFill);
        Assert.Equal(new string?[] { "zoom:42", null, "zoom:99" }, restoredGallery.ManualSlots);
        Assert.Equal(new[] { "zoom:17" }, restoredGallery.ExcludedSourceIds);
        Assert.Equal("4:3", restoredGallery.TileAspect);
        Assert.Equal("rounded", restoredGallery.BorderShape);
        Assert.Equal(12, restoredGallery.GlowSize, 3);

        var restored = restoredScene.Routes.Select(ScenePersistenceService.FromPersisted).ToList();
        Assert.Equal(2, restored.Count);
        var first = restored[0];
        Assert.Equal(SourceRouteMode.Fixed, first.Mode);
        Assert.Equal(2, first.ShowInputSlotNumber);
        Assert.NotNull(first.CanvasRect);
        Assert.Equal(0.1, first.CanvasRect!.X, 3);
        Assert.Equal(0.4, first.CanvasRect.Width, 3);
        Assert.Equal("fit", first.FitMode);
        Assert.Equal("#FF8800", first.BorderColor);
        Assert.Equal(1.5, first.SourceScale, 3);
        Assert.Equal(0.6, first.Opacity, 3);
        Assert.Equal(1, first.ZIndex);
        Assert.True(first.SourceFramingModified);

        var second = restored[1];
        Assert.Equal(SourceRouteMode.CaptureDevice, second.Mode);
        Assert.Equal("uvc-01", second.CaptureDeviceId);
        Assert.Null(second.CanvasRect);
    }

    [Fact]
    public void FromPersistedClampsHostileValues()
    {
        var restored = ScenePersistenceService.FromPersisted(new PersistedSceneRoute
        {
            Id = "r",
            Mode = "teleport",
            AudioRole = "shout",
            BorderThickness = 99,
            SourceScale = 100,
            SourceOffsetX = -9,
            Opacity = 7,
            ZIndex = -3
        });

        Assert.Equal(12, restored.BorderThickness, 3);
        Assert.Equal(4, restored.SourceScale, 3);
        Assert.Equal(-1, restored.SourceOffsetX, 3);
        Assert.Equal(1.0, restored.Opacity, 3);
        Assert.Equal(0, restored.ZIndex);
    }

    [Fact]
    public void MakeUniqueSceneNameAppendsCountersCaseInsensitively()
    {
        string[] existing = ["Interview", "interview copy", "Interview copy 2"];
        Assert.Equal("Interview copy 3", ScenePersistenceService.MakeUniqueSceneName("Interview copy", existing));
        Assert.Equal("Solo copy", ScenePersistenceService.MakeUniqueSceneName("Solo copy", existing));
    }

    // Route borders never reach the feeds (owner rule, 2026-07-31: borders
    // separate multiview tiles only — the old "accent" default baked a
    // studio-green frame into the composed program, which the virtual camera,
    // recordings, and streams all inherit). A fresh route carries no border,
    // normalization of a missing/unknown style resolves to "none", and stale
    // persisted styles are retired to "none" on load so shell previews match
    // what the core actually renders.
    [Fact]
    public void DefaultRouteBorderIsNone()
    {
        Assert.Equal("none", SourceRouteVisualDefaults.BorderStyle);
        Assert.Equal("none", new SourceRoute { Id = "route-1" }.BorderStyle);
        Assert.Equal("none", SceneRoutingService.NormalizeBorderStyle(null));
        Assert.Equal("none", SceneRoutingService.NormalizeBorderStyle("bogus"));

        var staleAccent = ScenePersistenceService.FromPersisted(new PersistedSceneRoute
        {
            Id = "legacy-route",
            Mode = "fixed",
            AudioRole = "mix",
            BorderStyle = "accent"
        });
        Assert.Equal("none", staleAccent.BorderStyle);
    }
}
