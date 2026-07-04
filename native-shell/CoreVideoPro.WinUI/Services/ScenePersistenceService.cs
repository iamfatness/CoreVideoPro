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
            Routes = routes.Select(ToPersisted).ToList()
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
            Automation = "Custom canvas"
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
            FitMode = SceneRoutingService.NormalizeFitMode(persisted.FitMode),
            BorderStyle = SceneRoutingService.NormalizeBorderStyle(persisted.BorderStyle),
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
