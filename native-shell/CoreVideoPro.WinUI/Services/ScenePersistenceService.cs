using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Scene &lt;-&gt; persisted-DTO conversion for custom-scene persistence (scenes
/// redesign S2), plus the unique-name helper Duplicate uses. Pure and static
/// so the round-trip behavior is unit-testable without a StudioViewModel.
/// </summary>
public static class ScenePersistenceService
{
    public static PersistedScene ToPersisted(Scene scene, IReadOnlyList<SourceRoute> routes) =>
        new()
        {
            Id = scene.Id,
            Name = scene.Name,
            Layout = scene.Layout,
            DynamicGallery = scene.DynamicGallery is null ? null : ToPersisted(scene.DynamicGallery),
            Routes = routes.Select(ToPersisted).ToList()
        };

    public static PersistedDynamicGallerySettings ToPersisted(DynamicGallerySettings settings) =>
        new()
        {
            AutoFill = settings.AutoFill,
            MaxTiles = settings.MaxTiles,
            TileAspect = settings.TileAspect,
            CustomAspectRatio = settings.CustomAspectRatio,
            GutterPercent = settings.GutterPercent,
            MarginPercent = settings.MarginPercent,
            BorderShape = settings.BorderShape,
            BorderColor = settings.BorderColor,
            BorderThickness = settings.BorderThickness,
            CornerRadius = settings.CornerRadius,
            GlowColor = settings.GlowColor,
            GlowSize = settings.GlowSize,
            GlowIntensity = settings.GlowIntensity,
            GlowSoftness = settings.GlowSoftness,
            AnimateLayout = settings.AnimateLayout,
            AnimationDurationMs = settings.AnimationDurationMs
        };

    public static PersistedSceneRoute ToPersisted(SourceRoute route) =>
        new()
        {
            Id = route.Id,
            Mode = SceneRoutingService.ModeToWire(route.Mode),
            AudioRole = SceneRoutingService.AudioRoleToWire(route.AudioRole),
            ParticipantId = route.ParticipantId,
            CaptureDeviceId = route.CaptureDeviceId,
            ShowInputSlotNumber = route.ShowInputSlotNumber,
            ProductionRoleId = route.ProductionRoleId,
            RectX = route.CanvasRect?.X,
            RectY = route.CanvasRect?.Y,
            RectWidth = route.CanvasRect?.Width,
            RectHeight = route.CanvasRect?.Height,
            FitMode = route.FitMode,
            BorderStyle = route.BorderStyle,
            BorderColor = route.BorderColor,
            BorderThickness = route.BorderThickness,
            SourceScale = route.SourceScale,
            SourceOffsetX = route.SourceOffsetX,
            SourceOffsetY = route.SourceOffsetY,
            Opacity = route.Opacity,
            ZIndex = route.ZIndex
        };

    public static Scene SceneFromPersisted(PersistedScene persisted) =>
        new()
        {
            Id = persisted.Id,
            Name = persisted.Name,
            Layout = string.IsNullOrWhiteSpace(persisted.Layout) ? "host-focus" : persisted.Layout,
            Automation = persisted.DynamicGallery is null ? "Custom canvas" : "Auto-reflow Zoom gallery",
            DynamicGallery = persisted.DynamicGallery is null ? null : FromPersisted(persisted.DynamicGallery)
        };

    public static DynamicGallerySettings FromPersisted(PersistedDynamicGallerySettings persisted) =>
        new()
        {
            AutoFill = persisted.AutoFill,
            MaxTiles = Math.Clamp(persisted.MaxTiles, 1, 64),
            TileAspect = DynamicGalleryLayoutService.NormalizeAspectPreset(persisted.TileAspect),
            CustomAspectRatio = Math.Clamp(persisted.CustomAspectRatio, 0.25, 4),
            GutterPercent = Math.Clamp(persisted.GutterPercent, 0, 10),
            MarginPercent = Math.Clamp(persisted.MarginPercent, 0, 20),
            BorderShape = persisted.BorderShape is "rounded" ? "rounded" : "square",
            BorderColor = SceneRoutingService.NormalizeBorderColor(persisted.BorderColor),
            BorderThickness = Math.Clamp(persisted.BorderThickness, 0, 32),
            CornerRadius = Math.Clamp(persisted.CornerRadius, 0, 100),
            GlowColor = SceneRoutingService.NormalizeBorderColor(persisted.GlowColor),
            GlowSize = Math.Clamp(persisted.GlowSize, 0, 64),
            GlowIntensity = Math.Clamp(persisted.GlowIntensity, 0, 100),
            GlowSoftness = Math.Clamp(persisted.GlowSoftness, 0, 100),
            AnimateLayout = persisted.AnimateLayout,
            AnimationDurationMs = Math.Clamp(persisted.AnimationDurationMs, 100, 2000)
        };

    public static SourceRoute FromPersisted(PersistedSceneRoute persisted)
    {
        var route = new SourceRoute
        {
            Id = persisted.Id,
            Mode = SceneRoutingService.ModeFromWire(persisted.Mode),
            AudioRole = SceneRoutingService.AudioRoleFromWire(persisted.AudioRole),
            ParticipantId = string.IsNullOrWhiteSpace(persisted.ParticipantId) ? null : persisted.ParticipantId,
            CaptureDeviceId = string.IsNullOrWhiteSpace(persisted.CaptureDeviceId) ? null : persisted.CaptureDeviceId,
            ShowInputSlotNumber = persisted.ShowInputSlotNumber,
            ProductionRoleId = string.IsNullOrWhiteSpace(persisted.ProductionRoleId) ? null : persisted.ProductionRoleId,
            FitMode = SceneRoutingService.NormalizeFitMode(persisted.FitMode),
            // Route borders were retired from the feeds (owner rule, 2026-07-31:
            // borders separate multiview tiles only, never composite into
            // program/preview) — stale persisted styles load as "none" so shell
            // previews match what the core actually renders.
            BorderStyle = "none",
            BorderColor = SceneRoutingService.NormalizeBorderColor(persisted.BorderColor),
            BorderThickness = Math.Clamp(persisted.BorderThickness, 0, 12),
            SourceScale = SceneRoutingService.NormalizeSourceScale(persisted.SourceScale),
            SourceOffsetX = SceneRoutingService.NormalizeSourceOffset(persisted.SourceOffsetX),
            SourceOffsetY = SceneRoutingService.NormalizeSourceOffset(persisted.SourceOffsetY),
            Opacity = Math.Clamp(persisted.Opacity, 0.1, 1.0),
            ZIndex = Math.Max(0, persisted.ZIndex)
        };

        if (persisted is { RectX: not null, RectY: not null, RectWidth: not null, RectHeight: not null })
        {
            route.CanvasRect = new NormalizedCanvasRect
            {
                X = persisted.RectX.Value,
                Y = persisted.RectY.Value,
                Width = persisted.RectWidth.Value,
                Height = persisted.RectHeight.Value
            };
            route.CanvasRect.Clamp();
        }

        route.SourceFramingModified = SceneRoutingService.HasModifiedSourceFraming(
            route.FitMode,
            route.SourceScale,
            route.SourceOffsetX,
            route.SourceOffsetY);
        return route;
    }

    /// <summary>"Name copy", then "Name copy 2", "Name copy 3", ... (case-insensitive).</summary>
    public static string MakeUniqueSceneName(string baseName, IEnumerable<string> existingNames)
    {
        var taken = new HashSet<string>(existingNames, StringComparer.OrdinalIgnoreCase);
        if (!taken.Contains(baseName))
        {
            return baseName;
        }

        for (var suffix = 2; ; suffix++)
        {
            var candidate = $"{baseName} {suffix}";
            if (!taken.Contains(candidate))
            {
                return candidate;
            }
        }
    }
}
