using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// POS-2 "bug" / undersized-asset placement (sources-redesign-spec §B2): preset rect
/// math (corners + safe-area margin + aspect lock), the draft-list insertion contract,
/// flyout-parameter parsing, and persistence round-trip of the new overlay layer.
/// </summary>
public sealed class OverlayLayerServiceTests
{
    private const int Precision = 10;

    // ==== Preset rect math ====

    [Fact]
    public void ComputeOverlayRect_FullCanvasCoversProgramWithoutSafeAreaInset()
    {
        var rect = OverlayLayerService.ComputeOverlayRect(OverlayPlacement.FullCanvas);

        Assert.Equal(0, rect.X, Precision);
        Assert.Equal(0, rect.Y, Precision);
        Assert.Equal(1, rect.Width, Precision);
        Assert.Equal(1, rect.Height, Precision);
    }

    [Fact]
    public void ComputeOverlayRect_DefaultIsFifteenPercentWideWithFivePercentMargin()
    {
        var rect = OverlayLayerService.ComputeOverlayRect(OverlayPlacement.BottomRight);

        // 16:9 fallback aspect on the 16:9 canvas -> normalized height == width.
        Assert.Equal(0.15, rect.Width, Precision);
        Assert.Equal(0.15, rect.Height, Precision);
        Assert.Equal(1 - 0.05 - 0.15, rect.X, Precision);
        Assert.Equal(1 - 0.05 - 0.15, rect.Y, Precision);
    }

    [Theory]
    [InlineData("top-left", 0.05, 0.05)]
    [InlineData("top-right", 0.80, 0.05)]
    [InlineData("bottom-left", 0.05, 0.80)]
    [InlineData("bottom-right", 0.80, 0.80)]
    public void ComputeOverlayRect_CornerPresetsSitInsideTheSafeAreaMargin(
        string placementWire, double expectedX, double expectedY)
    {
        var placement = OverlayLayerService.PlacementFromWire(placementWire);
        Assert.NotNull(placement);

        var rect = OverlayLayerService.ComputeOverlayRect(placement.Value);

        Assert.Equal(expectedX, rect.X, Precision);
        Assert.Equal(expectedY, rect.Y, Precision);
    }

    [Theory]
    [InlineData("center")]
    [InlineData("free")]
    public void ComputeOverlayRect_CenterAndFreeAreCentered(string placementWire)
    {
        var placement = OverlayLayerService.PlacementFromWire(placementWire);
        Assert.NotNull(placement);

        var rect = OverlayLayerService.ComputeOverlayRect(placement.Value);

        Assert.Equal((1 - rect.Width) / 2, rect.X, Precision);
        Assert.Equal((1 - rect.Height) / 2, rect.Y, Precision);
    }

    [Fact]
    public void ComputeOverlayRect_SquareLogoDerivesHeightFromNativeAspect()
    {
        var aspect = OverlayLayerService.AspectFromNaturalSize(400, 400);
        Assert.Equal(1.0, aspect!.Value, Precision);

        var rect = OverlayLayerService.ComputeOverlayRect(OverlayPlacement.BottomRight, aspect);

        // h = w * canvasAspect / aspect = 0.15 * (16/9) / 1.
        Assert.Equal(0.15, rect.Width, Precision);
        Assert.Equal(0.15 * (16.0 / 9.0), rect.Height, Precision);
        // Anchored to the BOTTOM edge: taller box moves up, not off-canvas.
        Assert.Equal(1 - 0.05 - 0.15 * (16.0 / 9.0), rect.Y, Precision);
    }

    [Fact]
    public void ComputeOverlayRect_WideBannerKeepsAspectAtTheMinimumHeightFloor()
    {
        // 1000x100 banner (aspect 10): 15% width would need h ~= 0.0267, below the
        // canvas editor's 0.08 floor -> the box grows, keeping the native aspect.
        var aspect = OverlayLayerService.AspectFromNaturalSize(1000, 100);
        var rect = OverlayLayerService.ComputeOverlayRect(OverlayPlacement.BottomRight, aspect);

        Assert.Equal(OverlayLayerService.MinNormalizedDimension, rect.Height, Precision);
        Assert.Equal(0.08 * 10 / (16.0 / 9.0), rect.Width, Precision);
        Assert.Equal(1 - 0.05 - rect.Width, rect.X, Precision);
    }

    [Fact]
    public void ComputeOverlayRect_DegenerateTallSourceIsCappedInsideTheSafeArea()
    {
        // Aspect 0.1 would want a 2.67-high box; it caps at the safe area instead of
        // going off-canvas (the one case where the aspect lock yields).
        var rect = OverlayLayerService.ComputeOverlayRect(OverlayPlacement.Center, 0.1);

        Assert.True(rect.Height <= 1 - 2 * OverlayLayerService.DefaultSafeAreaMargin + 1e-9);
        Assert.True(rect.Width >= OverlayLayerService.MinNormalizedDimension);
    }

    [Fact]
    public void ComputeOverlayRect_TinyWidthFractionIsRaisedToTheEditorFloor()
    {
        var rect = OverlayLayerService.ComputeOverlayRect(
            OverlayPlacement.TopLeft, sourceAspect: 16.0 / 9.0, widthFraction: 0.01);

        Assert.Equal(OverlayLayerService.MinNormalizedDimension, rect.Width, Precision);
    }

    [Fact]
    public void ComputeOverlayRect_SurvivesTheClampAppliedOnThePersistenceLoadPath()
    {
        // FromPersisted calls NormalizedCanvasRect.Clamp() — the computed rect must be
        // a fixed point of it or a saved bug would shift on reload.
        foreach (var placementOption in OverlayLayerService.PlacementOptions)
        {
            var placement = OverlayLayerService.PlacementFromWire(placementOption.Value)!.Value;
            foreach (var aspect in new double?[] { null, 1.0, 10.0, 0.5 })
            {
                var rect = OverlayLayerService.ComputeOverlayRect(placement, aspect);
                var clamped = rect.Clone();
                clamped.Clamp();

                Assert.Equal(rect.X, clamped.X, Precision);
                Assert.Equal(rect.Y, clamped.Y, Precision);
                Assert.Equal(rect.Width, clamped.Width, Precision);
                Assert.Equal(rect.Height, clamped.Height, Precision);
            }
        }
    }

    // ==== Flyout parameter parsing ====

    [Fact]
    public void TryParseAddOverlayParameter_ParsesMediaAssetEntries()
    {
        var ok = OverlayLayerService.TryParseAddOverlayParameter(
            "bottom-right|media:asset-7", out var placement, out var option, out var assetId);

        Assert.True(ok);
        Assert.Equal(OverlayPlacement.BottomRight, placement);
        Assert.Equal("media", option);
        Assert.Equal("asset-7", assetId);
    }

    [Theory]
    [InlineData("center|input-03", "input-03")]
    [InlineData("free|role:host", "role:host")]
    [InlineData("top-left|active-speaker", "active-speaker")]
    [InlineData("full-canvas|capture:browser:1", "capture:browser:1")]
    public void TryParseAddOverlayParameter_ParsesAddSourceOptionEntries(string parameter, string expectedOption)
    {
        var ok = OverlayLayerService.TryParseAddOverlayParameter(
            parameter, out _, out var option, out var assetId);

        Assert.True(ok);
        Assert.Equal(expectedOption, option);
        Assert.Null(assetId);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("bottom-right")]
    [InlineData("bottom-right|")]
    [InlineData("|input-01")]
    [InlineData("bogus|input-01")]
    [InlineData("top-left|media:")]
    public void TryParseAddOverlayParameter_RejectsMalformedParameters(string? parameter)
    {
        Assert.False(OverlayLayerService.TryParseAddOverlayParameter(parameter, out _, out _, out _));
    }

    // ==== Draft-list insertion ====

    [Fact]
    public void AppendOverlayRoute_AppendsTopMostSmallAspectLockedRoute()
    {
        var routes = new List<SourceRoute>
        {
            new() { Id = "scene-1-1", Mode = SourceRouteMode.Fixed, ParticipantId = "p1",
                    CanvasRect = new NormalizedCanvasRect { X = 0, Y = 0, Width = 1, Height = 1 } }
        };

        var route = OverlayLayerService.AppendOverlayRoute(
            routes, "scene-1", "media", "asset-7", sourceAspect: 1.0,
            OverlayPlacement.BottomRight, layoutHint: "host-focus", participants: []);

        Assert.Equal(2, routes.Count);
        Assert.Same(route, routes[^1]);
        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal("media:asset-7", route.ParticipantId);
        Assert.Equal("fit", route.FitMode);
        Assert.True(route.SourceFramingModified);

        // Small, not canvas-filling; matches the pure rect math.
        var expected = OverlayLayerService.ComputeOverlayRect(OverlayPlacement.BottomRight, 1.0);
        Assert.NotNull(route.CanvasRect);
        Assert.Equal(expected.X, route.CanvasRect!.X, Precision);
        Assert.Equal(expected.Y, route.CanvasRect.Y, Precision);
        Assert.Equal(expected.Width, route.CanvasRect.Width, Precision);
        Assert.Equal(expected.Height, route.CanvasRect.Height, Precision);

        // Top-most: list order is layer order and zIndex mirrors the index.
        Assert.Equal([0, 1], routes.Select(r => r.ZIndex));
        Assert.Equal(routes.Count - 1, route.ZIndex);
    }

    [Fact]
    public void AppendOverlayRoute_GivesExistingRoutesTheirPresetRectsBeforeTheBugJoins()
    {
        // EnsureCanvasRects re-applies the preset to EVERY route when any rect is
        // missing. If the bug were added first, the next refresh would stomp its rect
        // back to a preset cell — so existing routes must be seeded BEFORE the append.
        var routes = new List<SourceRoute>
        {
            new() { Id = "scene-1-1", Mode = SourceRouteMode.Fixed, ParticipantId = "p1" },
            new() { Id = "scene-1-2", Mode = SourceRouteMode.ActiveSpeaker }
        };

        var route = OverlayLayerService.AppendOverlayRoute(
            routes, "scene-1", "input-04", mediaAssetId: null, sourceAspect: null,
            OverlayPlacement.TopLeft, layoutHint: "two-up", participants: []);

        Assert.All(routes, r => Assert.NotNull(r.CanvasRect));

        // The overlay kept its bug rect (a preset re-apply would have made it a cell).
        var expected = OverlayLayerService.ComputeOverlayRect(OverlayPlacement.TopLeft);
        Assert.Equal(expected.X, route.CanvasRect!.X, Precision);
        Assert.Equal(expected.Width, route.CanvasRect.Width, Precision);

        // A later EnsureCanvasRects is now a no-op — nothing left without a rect.
        var snapshot = routes.Select(r => r.CanvasRect!.Clone()).ToList();
        SceneCanvasLayoutService.EnsureCanvasRects(routes, "two-up");
        for (var index = 0; index < routes.Count; index++)
        {
            Assert.Equal(snapshot[index].X, routes[index].CanvasRect!.X, Precision);
            Assert.Equal(snapshot[index].Width, routes[index].CanvasRect!.Width, Precision);
        }

        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal(4, route.ShowInputSlotNumber);
    }

    [Fact]
    public void AppendOverlayRoute_DoesNotDisturbExistingRects()
    {
        var existingRect = new NormalizedCanvasRect { X = 0.1, Y = 0.2, Width = 0.5, Height = 0.4 };
        var routes = new List<SourceRoute>
        {
            new() { Id = "scene-1-1", Mode = SourceRouteMode.Fixed, ParticipantId = "p1",
                    CanvasRect = existingRect.Clone() }
        };

        OverlayLayerService.AppendOverlayRoute(
            routes, "scene-1", "role:reader", mediaAssetId: null, sourceAspect: null,
            OverlayPlacement.Center, layoutHint: "host-focus", participants: []);

        Assert.Equal(existingRect.X, routes[0].CanvasRect!.X, Precision);
        Assert.Equal(existingRect.Y, routes[0].CanvasRect!.Y, Precision);
        Assert.Equal(existingRect.Width, routes[0].CanvasRect!.Width, Precision);
        Assert.Equal(existingRect.Height, routes[0].CanvasRect!.Height, Precision);
        Assert.Equal("reader", routes[^1].ProductionRoleId);
    }

    [Fact]
    public void AppendOverlayRoute_BrowserCaptureIsDirectFullCanvasTopLayer()
    {
        var routes = new List<SourceRoute>
        {
            new() { Id = "scene-1-1", Mode = SourceRouteMode.Fixed, ParticipantId = "p1",
                    CanvasRect = new NormalizedCanvasRect { X = 0, Y = 0, Width = 1, Height = 1 } }
        };

        var route = OverlayLayerService.AppendOverlayRoute(
            routes, "scene-1", "capture:browser:1", mediaAssetId: null, sourceAspect: 16.0 / 9.0,
            OverlayPlacement.FullCanvas, layoutHint: "host-focus", participants: []);

        Assert.Equal(SourceRouteMode.CaptureDevice, route.Mode);
        Assert.Equal("browser:1", route.CaptureDeviceId);
        Assert.Null(route.ShowInputSlotNumber);
        Assert.Equal(0, route.CanvasRect!.X, Precision);
        Assert.Equal(0, route.CanvasRect.Y, Precision);
        Assert.Equal(1, route.CanvasRect.Width, Precision);
        Assert.Equal(1, route.CanvasRect.Height, Precision);
        Assert.Equal(routes.Count - 1, route.ZIndex);
        Assert.Equal("fit", route.FitMode);
    }

    [Fact]
    public void AppendOverlayRoute_MutatesOnlyTheGivenDraftList()
    {
        // Models the S2b contract the ViewModel relies on: the command inserts into
        // GetPreviewEditableRoutes' DRAFT list; the stored (program) list is untouched
        // until Update/Take copies the draft over.
        var programRoutes = new List<SourceRoute>
        {
            new() { Id = "scene-1-1", Mode = SourceRouteMode.Fixed, ParticipantId = "p1",
                    CanvasRect = new NormalizedCanvasRect { X = 0, Y = 0, Width = 1, Height = 1 } }
        };
        var draftRoutes = programRoutes.Select(r => r.Clone()).ToList();

        OverlayLayerService.AppendOverlayRoute(
            draftRoutes, "scene-1", "media", "asset-9", sourceAspect: null,
            OverlayPlacement.BottomLeft, layoutHint: "host-focus", participants: []);

        Assert.Equal(2, draftRoutes.Count);
        Assert.Single(programRoutes);
        Assert.Equal(1, programRoutes[0].CanvasRect!.Width, Precision);
    }

    // ==== Persistence round-trip ====

    [Fact]
    public void OverlayRoute_RoundTripsThroughScenePersistence()
    {
        var routes = new List<SourceRoute>();
        var route = OverlayLayerService.AppendOverlayRoute(
            routes, "scene-1", "media", "asset-7", sourceAspect: 1.0,
            OverlayPlacement.BottomRight, layoutHint: "host-focus", participants: []);

        var restored = ScenePersistenceService.FromPersisted(ScenePersistenceService.ToPersisted(route));

        Assert.Equal(route.Id, restored.Id);
        Assert.Equal(SourceRouteMode.Fixed, restored.Mode);
        Assert.Equal("media:asset-7", restored.ParticipantId);
        Assert.Equal("fit", restored.FitMode);
        Assert.True(restored.SourceFramingModified);
        Assert.Equal(route.ZIndex, restored.ZIndex);
        Assert.Equal(1.0, restored.Opacity, Precision);

        Assert.NotNull(restored.CanvasRect);
        Assert.Equal(route.CanvasRect!.X, restored.CanvasRect!.X, Precision);
        Assert.Equal(route.CanvasRect.Y, restored.CanvasRect.Y, Precision);
        Assert.Equal(route.CanvasRect.Width, restored.CanvasRect.Width, Precision);
        Assert.Equal(route.CanvasRect.Height, restored.CanvasRect.Height, Precision);
    }

    [Fact]
    public void PlacementOptions_CoverEveryWirePlacement()
    {
        Assert.Equal(7, OverlayLayerService.PlacementOptions.Count);
        Assert.All(
            OverlayLayerService.PlacementOptions,
            option => Assert.NotNull(OverlayLayerService.PlacementFromWire(option.Value)));
    }
}
