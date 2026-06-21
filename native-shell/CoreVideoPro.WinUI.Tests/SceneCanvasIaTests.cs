using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Covers the §4 IA cleanup for the Scenes tab: the per-layer Route(Mode)/AudioRole
/// operator controls were removed so the Routing tab matrices are the single source of
/// truth, while an explicit "Add source" affordance lists the Inputs.
/// </summary>
public sealed class SceneCanvasIaTests
{
    [Fact]
    public void AddSourceOptions_ListsInputsOneThroughTenPlusAutoRoles()
    {
        var options = SceneRoutingService.AddSourceOptions;

        for (var slot = 1; slot <= ShowInputRosterService.MaxShowInputs; slot++)
        {
            var value = $"input-{slot:00}";
            Assert.Contains(options, option => option.Value == value && option.Label == $"Input {slot}");
        }

        Assert.Contains(options, option => option.Value == "active-speaker");
        Assert.Contains(options, option => option.Value == "screen-share");
        Assert.Contains(options, option => option.Value == "media");

        // Inputs 1-10 plus the three auto roles.
        Assert.Equal(ShowInputRosterService.MaxShowInputs + 3, options.Count);
    }

    [Fact]
    public void GetRouteDefaults_PreservesOperatorAddedSourcesBeyondLayoutSlotCount()
    {
        // "host-focus" maps to a single-slot layout, but an operator can append sources via
        // the "Add source" affordance; reconciliation must not truncate them away.
        var scene = new Scene { Id = "scene-1", Name = "Solo", Layout = "host-focus" };
        var participants = new List<Participant>
        {
            new() { Id = "p1", Name = "Host" }
        };

        var existing = new List<SourceRoute>
        {
            new() { Id = "scene-1-1", Mode = SourceRouteMode.Fixed, ParticipantId = "p1" },
            new() { Id = "scene-1-2", Mode = SourceRouteMode.Fixed, ShowInputSlotNumber = 2 },
            new() { Id = "scene-1-3", Mode = SourceRouteMode.ScreenShare }
        };

        var defaults = SceneRoutingService.GetRouteDefaults(scene, existing, participants);

        Assert.Equal(existing.Count, defaults.Count);
    }

    [Fact]
    public void CanvasPresetOptions_ExposeSingleThroughEightUpTemplates()
    {
        var values = SceneCanvasLayoutService.PresetOptions.Select(option => option.Value).ToList();

        Assert.Contains("single", values);
        Assert.Contains("two-up", values);
        Assert.Contains("three-up", values);
        Assert.Contains("four-up", values);
        Assert.Contains("five-up", values);
        Assert.Contains("six-up", values);
        Assert.Contains("seven-up", values);
        Assert.Contains("eight-up", values);
    }

    [Theory]
    [InlineData("single", 1)]
    [InlineData("two-up", 2)]
    [InlineData("three-up", 3)]
    [InlineData("four-up", 4)]
    [InlineData("five-up", 5)]
    [InlineData("six-up", 6)]
    [InlineData("seven-up", 7)]
    [InlineData("eight-up", 8)]
    public void CanvasPresetTemplates_DeclareExactSlotCounts(string preset, int expectedCount)
    {
        Assert.Equal(expectedCount, SceneCanvasLayoutService.TemplateSlotCount(preset));
        Assert.Equal(expectedCount, SceneCanvasLayoutService.BuildPresetRects(
            SceneCanvasLayoutService.PresetFromWire(preset),
            layerCount: expectedCount).Count);
    }

    [Fact]
    public void EightUpPreset_UsesFourByTwoGrid()
    {
        var rects = SceneCanvasLayoutService.BuildPresetRects(SceneCanvasPreset.EightUp, 8);

        Assert.Equal(8, rects.Count);
        Assert.Equal(4, rects.Select(rect => rect.X).Distinct().Count());
        Assert.Equal(2, rects.Select(rect => rect.Y).Distinct().Count());
        Assert.All(rects, rect =>
        {
            Assert.Equal(0.235, rect.Width, precision: 3);
            Assert.Equal(0.49, rect.Height, precision: 3);
        });
    }

    [Theory]
    [InlineData("single", 1)]
    [InlineData("two-up", 2)]
    [InlineData("three-up", 3)]
    [InlineData("four-up", 4)]
    [InlineData("five-up", 5)]
    [InlineData("six-up", 6)]
    [InlineData("seven-up", 7)]
    [InlineData("eight-up", 8)]
    public void GetRouteDefaults_TemplateLayoutsCreateExpectedRouteCapacity(string layout, int expectedCount)
    {
        var scene = new Scene { Id = $"scene-{layout}", Name = layout, Layout = layout };

        var defaults = SceneRoutingService.GetRouteDefaults(scene, existingRoutes: null, participants: []);

        Assert.Equal(expectedCount, defaults.Count);
    }

    [Theory]
    [InlineData("single", 1)]
    [InlineData("two-up", 2)]
    [InlineData("three-up", 3)]
    [InlineData("four-up", 4)]
    [InlineData("five-up", 5)]
    [InlineData("six-up", 6)]
    [InlineData("seven-up", 7)]
    [InlineData("eight-up", 8)]
    public void ApplyInputSlotTemplate_CreatesExactInputSlotsForCanvasPreset(string preset, int expectedCount)
    {
        var routes = Enumerable.Range(0, 10)
            .Select(index =>
            {
                var route = SceneRoutingService.BuildAddedSourceRoute("scene-1", index, $"input-{index + 1}");
                route.SourceScale = 1.5;
                route.SourceOffsetX = index % 2 == 0 ? -1 : 1;
                route.SourceOffsetY = index % 2 == 0 ? 1 : -1;
                return route;
            })
            .ToList();

        SceneRoutingService.ApplyInputSlotTemplate(routes, "scene-1", expectedCount);
        SceneCanvasLayoutService.ApplyPreset(preset, routes);

        Assert.Equal(expectedCount, routes.Count);
        for (var index = 0; index < routes.Count; index++)
        {
            Assert.Equal(SourceRouteMode.Fixed, routes[index].Mode);
            Assert.Equal(index + 1, routes[index].ShowInputSlotNumber);
            Assert.Null(routes[index].ParticipantId);
            Assert.Null(routes[index].CaptureDeviceId);
            Assert.Null(routes[index].SpotlightIndex);
            Assert.Equal(SourceAudioRole.Mix, routes[index].AudioRole);
            Assert.Equal(index, routes[index].ZIndex);
            Assert.Equal(SourceRouteVisualDefaults.SourceScale, routes[index].SourceScale);
            Assert.Equal(SourceRouteVisualDefaults.SourceOffsetX, routes[index].SourceOffsetX);
            Assert.Equal(SourceRouteVisualDefaults.SourceOffsetY, routes[index].SourceOffsetY);
            Assert.NotNull(routes[index].CanvasRect);
        }
    }

    [Fact]
    public void ApplyPreset_RecentersExistingSources()
    {
        var routes = new[]
        {
            SceneRoutingService.BuildAddedSourceRoute("scene-1", 0, "input-01"),
            SceneRoutingService.BuildAddedSourceRoute("scene-1", 1, "input-02")
        };
        routes[0].FitMode = "fit";
        routes[0].SourceScale = 2;
        routes[0].SourceOffsetX = -1;
        routes[0].SourceOffsetY = 1;
        routes[1].FitMode = "stretch";
        routes[1].SourceScale = 0.5;
        routes[1].SourceOffsetX = 1;
        routes[1].SourceOffsetY = -1;

        SceneCanvasLayoutService.ApplyPreset("speaker-slides", routes);

        foreach (var route in routes)
        {
            Assert.Equal(SourceRouteVisualDefaults.FitMode, route.FitMode);
            Assert.Equal(SourceRouteVisualDefaults.SourceScale, route.SourceScale);
            Assert.Equal(SourceRouteVisualDefaults.SourceOffsetX, route.SourceOffsetX);
            Assert.Equal(SourceRouteVisualDefaults.SourceOffsetY, route.SourceOffsetY);
        }
    }

    [Fact]
    public void BuildAddedSourceRoute_MediaWithoutSelectionStaysParked()
    {
        var route = SceneRoutingService.BuildAddedSourceRoute("scene-1", 2, "media");

        Assert.Equal(SourceRouteMode.None, route.Mode);
        Assert.Null(route.ParticipantId);
        Assert.Equal(2, route.ZIndex);
    }

    [Fact]
    public void BuildAddedSourceRoute_MediaSelectionCreatesFixedMediaRoute()
    {
        var route = SceneRoutingService.BuildAddedSourceRoute("scene-1", 2, "media", "asset-7");

        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal("media:asset-7", route.ParticipantId);
        Assert.Equal(SourceAudioRole.Mix, route.AudioRole);
        Assert.Equal(2, route.ZIndex);
        Assert.NotNull(route.CanvasRect);
        Assert.Equal(1, route.CanvasRect.Width);
        Assert.Equal(1, route.CanvasRect.Height);
    }

    [Theory]
    [InlineData("input-01")]
    [InlineData("active-speaker")]
    [InlineData("screen-share")]
    [InlineData("media")]
    public void BuildAddedSourceRoute_DefaultsVideoFramingToCenteredSuperSourceLayer(string optionValue)
    {
        var route = SceneRoutingService.BuildAddedSourceRoute("scene-1", 2, optionValue, "asset-7");

        Assert.Equal(SourceRouteVisualDefaults.FitMode, route.FitMode);
        Assert.Equal(SourceRouteVisualDefaults.SourceScale, route.SourceScale);
        Assert.Equal(0, route.SourceOffsetX);
        Assert.Equal(0, route.SourceOffsetY);
    }

    [Fact]
    public void SourceFraming_FillPanMovesFullSourceInsideCroppedBox()
    {
        var centered = SourceFramingLayoutService.Resolve(
            viewportWidth: 900,
            viewportHeight: 900,
            sourceWidth: 1600,
            sourceHeight: 900,
            fitMode: "fill",
            sourceScale: 1,
            sourceOffsetX: 0,
            sourceOffsetY: 0);
        var left = SourceFramingLayoutService.Resolve(900, 900, 1600, 900, "fill", 1, -1, 0);
        var right = SourceFramingLayoutService.Resolve(900, 900, 1600, 900, "fill", 1, 1, 0);

        Assert.Equal(1600, centered.Width, precision: 3);
        Assert.Equal(900, centered.Height, precision: 3);
        Assert.Equal(-350, centered.TranslateX, precision: 3);
        Assert.Equal(0, left.TranslateX, precision: 3);
        Assert.Equal(-700, right.TranslateX, precision: 3);
    }

    [Fact]
    public void SourceFraming_FillCentersWideSourceInTallSuperSourceTile()
    {
        var centered = SourceFramingLayoutService.Resolve(
            viewportWidth: 376,
            viewportHeight: 441,
            sourceWidth: 1920,
            sourceHeight: 1080,
            fitMode: "fill",
            sourceScale: 1,
            sourceOffsetX: 0,
            sourceOffsetY: 0);

        Assert.True(centered.Width > 376);
        Assert.Equal(441, centered.Height, precision: 3);
        Assert.True(centered.TranslateX < 0);
        Assert.True(centered.TranslateX + centered.Width > 376);
        Assert.Equal((376 - centered.Width) * 0.5, centered.TranslateX, precision: 3);
    }

    [Fact]
    public void VideoSurfaceState_FramingSourceUsesNaturalSourceDimensions()
    {
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "participant:p1", "Guest")
            .WithPreviewPixels(
                bgra: [0, 0, 0, 255],
                width: 400,
                height: 225,
                naturalSourceWidth: 1920,
                naturalSourceHeight: 1080);

        Assert.Equal(400, surface.PreviewWidth);
        Assert.Equal(225, surface.PreviewHeight);
        Assert.Equal(1920, surface.FramingSourceWidth);
        Assert.Equal(1080, surface.FramingSourceHeight);
    }

    [Fact]
    public void SourceFraming_DownscaledPreviewStillPansAgainstNaturalSourceAspect()
    {
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "participant:p1", "Guest")
            .WithPreviewPixels(
                bgra: [0, 0, 0, 255],
                width: 400,
                height: 400,
                naturalSourceWidth: 1600,
                naturalSourceHeight: 900);

        var right = SourceFramingLayoutService.Resolve(
            viewportWidth: 900,
            viewportHeight: 900,
            sourceWidth: surface.FramingSourceWidth,
            sourceHeight: surface.FramingSourceHeight,
            fitMode: "fill",
            sourceScale: 1,
            sourceOffsetX: 1,
            sourceOffsetY: 0);

        Assert.Equal(1600, right.Width, precision: 3);
        Assert.Equal(900, right.Height, precision: 3);
        Assert.Equal(-700, right.TranslateX, precision: 3);
    }

    [Fact]
    public void SourceFraming_ZoomedFillPanMovesOriginalSourceUnderBox()
    {
        var centered = SourceFramingLayoutService.Resolve(900, 900, 1600, 900, "fill", 2, 0, 0);
        var left = SourceFramingLayoutService.Resolve(900, 900, 1600, 900, "fill", 2, -1, 0);
        var right = SourceFramingLayoutService.Resolve(900, 900, 1600, 900, "fill", 2, 1, 0);

        Assert.Equal(3200, centered.Width, precision: 3);
        Assert.Equal(1800, centered.Height, precision: 3);
        Assert.Equal(0, left.TranslateX, precision: 3);
        Assert.Equal(-2300, right.TranslateX, precision: 3);
    }

    [Fact]
    public void SourceFraming_DragPanUsesActualFillTravelNotBoxSize()
    {
        var offset = SourceFramingLayoutService.ResolveOffsetAfterDrag(
            viewportWidth: 900,
            viewportHeight: 900,
            sourceWidth: 1600,
            sourceHeight: 900,
            fitMode: "fill",
            sourceScale: 1,
            startOffsetX: 0,
            startOffsetY: 0,
            dragDeltaX: 350,
            dragDeltaY: 0);

        Assert.Equal(-1, offset.X, precision: 3);
        Assert.Equal(0, offset.Y, precision: 3);
    }

    [Fact]
    public void SourceFraming_DragPanSlowsDownWhenZoomCreatesMoreTravel()
    {
        var offset = SourceFramingLayoutService.ResolveOffsetAfterDrag(
            viewportWidth: 900,
            viewportHeight: 900,
            sourceWidth: 1600,
            sourceHeight: 900,
            fitMode: "fill",
            sourceScale: 2,
            startOffsetX: 0,
            startOffsetY: 0,
            dragDeltaX: 350,
            dragDeltaY: 0);

        Assert.Equal(-0.3, offset.X, precision: 2);
        Assert.Equal(0, offset.Y, precision: 3);
    }

    [Fact]
    public void SourceFraming_DragPanPositionsDownscaledSourceInsideBox()
    {
        var offset = SourceFramingLayoutService.ResolveOffsetAfterDrag(
            viewportWidth: 900,
            viewportHeight: 900,
            sourceWidth: 900,
            sourceHeight: 900,
            fitMode: "stretch",
            sourceScale: 0.5,
            startOffsetX: 0,
            startOffsetY: 0,
            dragDeltaX: 225,
            dragDeltaY: 225);

        Assert.Equal(1, offset.X, precision: 3);
        Assert.Equal(1, offset.Y, precision: 3);
    }

    [Fact]
    public void SourceFraming_FitModeCentersLetterboxedSourceAndStillAllowsZoomPan()
    {
        var centered = SourceFramingLayoutService.Resolve(900, 900, 1600, 900, "fit", 1, 0, 0);
        var zoomedDown = SourceFramingLayoutService.Resolve(900, 900, 1600, 900, "fit", 2, 0, 1);

        Assert.Equal(900, centered.Width, precision: 3);
        Assert.Equal(506.25, centered.Height, precision: 3);
        Assert.Equal(196.875, centered.TranslateY, precision: 3);
        Assert.Equal(1012.5, zoomedDown.Height, precision: 3);
        Assert.True(zoomedDown.TranslateY < centered.TranslateY);
    }

    [Fact]
    public void SourceFraming_ScaleBelowOneCanBePositionedInsideSourceBox()
    {
        var centered = SourceFramingLayoutService.Resolve(900, 900, 900, 900, "stretch", 0.5, 0, 0);
        var lowerRight = SourceFramingLayoutService.Resolve(900, 900, 900, 900, "stretch", 0.5, 1, 1);

        Assert.Equal(450, centered.Width, precision: 3);
        Assert.Equal(450, centered.Height, precision: 3);
        Assert.Equal(225, centered.TranslateX, precision: 3);
        Assert.Equal(225, centered.TranslateY, precision: 3);
        Assert.Equal(450, lowerRight.TranslateX, precision: 3);
        Assert.Equal(450, lowerRight.TranslateY, precision: 3);
    }

    [Fact]
    public void Layer_SourceOffsetPanDoesNotMoveSceneBox()
    {
        var route = new SourceRoute
        {
            Id = "scene-1-1",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = "p1",
            CanvasRect = new NormalizedCanvasRect { X = 0.2, Y = 0.1, Width = 0.4, Height = 0.5 },
            SourceOffsetX = 0,
            SourceOffsetY = 0
        };
        var layer = new SceneCanvasLayerViewModel(
            0,
            route,
            participants: [new Participant { Id = "p1", Name = "Host" }],
            captureDevices: [],
            showInputs: [],
            mediaAssets: [],
            _ => { });

        layer.SetSourceOffset(2, -2);

        Assert.Equal(0.2, route.CanvasRect.X, precision: 3);
        Assert.Equal(0.1, route.CanvasRect.Y, precision: 3);
        Assert.Equal(0.4, route.CanvasRect.Width, precision: 3);
        Assert.Equal(0.5, route.CanvasRect.Height, precision: 3);
        Assert.Equal(1, route.SourceOffsetX, precision: 3);
        Assert.Equal(-1, route.SourceOffsetY, precision: 3);
    }

    [Fact]
    public void Layer_SourceChange_DoesNotIndependentlyDriveAudioRole()
    {
        // The per-layer AudioRole dropdown is gone: changing the source picker must keep the
        // route's audio role at its initialized default (audio is matrix-governed).
        var route = new SourceRoute
        {
            Id = "scene-1-1",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = "p1",
            AudioRole = SourceAudioRole.Mix
        };

        var participants = new List<Participant>
        {
            new() { Id = "p1", Name = "Host" },
            new() { Id = "p2", Name = "Guest" }
        };

        var changes = 0;
        var layer = new SceneCanvasLayerViewModel(
            0,
            route,
            participants,
            captureDevices: [],
            showInputs: [],
            mediaAssets: [],
            _ => changes++);

        layer.ParticipantId = "p2";

        Assert.True(changes > 0);
        Assert.Equal(SourceAudioRole.Mix, route.AudioRole);
    }

    [Fact]
    public void Layer_SourcePickerListsMediaAssetsAsCanvasSources()
    {
        var route = new SourceRoute
        {
            Id = "scene-1-1",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = "p1",
            CanvasRect = new NormalizedCanvasRect { X = 0.25, Y = 0.2, Width = 0.4, Height = 0.3 },
            AudioRole = SourceAudioRole.Mix
        };

        var changes = 0;
        var layer = new SceneCanvasLayerViewModel(
            0,
            route,
            participants: [new Participant { Id = "p1", Name = "Host" }],
            captureDevices: [],
            showInputs: [],
            mediaAssets:
            [
                new MediaAsset { Id = "intro", Name = "Intro Clip", Kind = "video", FilePath = "C:\\media\\intro.mp4" },
                new MediaAsset { Id = "notes", Name = "Run Sheet", Kind = "pdf", FilePath = "C:\\media\\run.pdf" }
            ],
            _ => changes++);

        Assert.Contains(layer.ParticipantOptions, option => option.Value == "media:intro" && option.Label == "Media - Intro Clip");
        Assert.DoesNotContain(layer.ParticipantOptions, option => option.Value == "media:notes");

        layer.ParticipantId = "media:intro";

        Assert.True(changes > 0);
        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal("media:intro", route.ParticipantId);
        Assert.Null(route.CaptureDeviceId);
        Assert.Null(route.ShowInputSlotNumber);
        Assert.NotNull(route.CanvasRect);
        Assert.Equal(0.25, route.CanvasRect.X, precision: 3);
        Assert.Equal(0.2, route.CanvasRect.Y, precision: 3);
        Assert.Equal(0.4, route.CanvasRect.Width, precision: 3);
        Assert.Equal(0.3, route.CanvasRect.Height, precision: 3);
    }

    [Fact]
    public void IsoAudioAuthority_IsTheVideoRoutingMatrix_NotPerLayerAudioRole()
    {
        // BuildIsoParticipantTargets prefers iso- matrix crosspoints. Confirm the matrix can
        // independently designate an iso destination (the single audio source of truth).
        var matrix = new VideoRoutingMatrixViewModel();
        matrix.Build(
            [
                new RoutingSource("input-01", "Input 1"),
                new RoutingSource("input-02", "Input 2")
            ]);

        var iso = matrix.Rows
            .Single(row => row.SourceId == "input-01")
            .Cells
            .Single(cell => cell.Destination.Id == "iso-1");

        matrix.SelectCrosspointCommand.Execute(iso);

        Assert.True(iso.IsRouted);
    }
}
