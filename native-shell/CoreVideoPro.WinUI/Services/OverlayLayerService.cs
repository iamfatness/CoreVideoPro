using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Preset spots for a small overlay ("bug") layer on the 16:9 canvas.</summary>
public enum OverlayPlacement
{
    FullCanvas,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Center,
    /// <summary>Same starting rect as Center; the operator drags it wherever they want.</summary>
    Free
}

/// <summary>
/// POS-2 ("bug" / undersized-asset placement, sources-redesign-spec §B2): builds a NEW
/// top-most scene route at a SMALL aspect-locked rect — a logo bug in a corner, not a
/// canvas-filling source. Pure and static so the rect math, the draft-list insertion,
/// and the flyout-parameter parsing are unit-testable without a StudioViewModel.
/// The route itself is a plain <see cref="SourceRoute"/>, so it persists with the scene
/// and is editable with all the existing canvas machinery (grips/nudge/numeric fields).
/// </summary>
public static class OverlayLayerService
{
    /// <summary>Default bug width as a fraction of canvas width (~15% per spec §B2).</summary>
    public const double DefaultWidthFraction = 0.15;

    /// <summary>
    /// Safe-area margin from the canvas edges (normalized, both axes). POS-1 shipped
    /// without its settings increment — there is no "default bug margin %" setting yet —
    /// so this constant (~5%, the classic title-safe inset) is the default until that
    /// setting lands; route it through here when it does.
    /// </summary>
    public const double DefaultSafeAreaMargin = 0.05;

    /// <summary>The program canvas is 16:9 (NormalizedCanvasRect is defined against it).</summary>
    public const double CanvasAspect = 16.0 / 9.0;

    /// <summary>Used when a source's native aspect is unknown (spec: fall back to 16:9).</summary>
    public const double FallbackSourceAspect = 16.0 / 9.0;

    /// <summary>
    /// Mirrors <see cref="NormalizedCanvasRect.Clamp"/>'s floor so a computed bug rect
    /// round-trips losslessly through persistence (which clamps on load).
    /// </summary>
    public const double MinNormalizedDimension = 0.08;

    /// <summary>Placement picker entries, in menu order.</summary>
    public static IReadOnlyList<RouteSelectOption> PlacementOptions { get; } =
    [
        new() { Value = "full-canvas", Label = "Full canvas (transparent overlay)" },
        new() { Value = "top-left", Label = "Top left" },
        new() { Value = "top-right", Label = "Top right" },
        new() { Value = "bottom-left", Label = "Bottom left" },
        new() { Value = "bottom-right", Label = "Bottom right" },
        new() { Value = "center", Label = "Center" },
        new() { Value = "free", Label = "Free (drag to place)" }
    ];

    public static OverlayPlacement? PlacementFromWire(string? wire) => wire switch
    {
        "full-canvas" => OverlayPlacement.FullCanvas,
        "top-left" => OverlayPlacement.TopLeft,
        "top-right" => OverlayPlacement.TopRight,
        "bottom-left" => OverlayPlacement.BottomLeft,
        "bottom-right" => OverlayPlacement.BottomRight,
        "center" => OverlayPlacement.Center,
        "free" => OverlayPlacement.Free,
        _ => null
    };

    public static string PlacementLabel(OverlayPlacement placement) => placement switch
    {
        OverlayPlacement.FullCanvas => "Full canvas",
        OverlayPlacement.TopLeft => "Top left",
        OverlayPlacement.TopRight => "Top right",
        OverlayPlacement.BottomLeft => "Bottom left",
        OverlayPlacement.BottomRight => "Bottom right",
        OverlayPlacement.Center => "Center",
        _ => "Free"
    };

    /// <summary>Native aspect (w/h) from a media asset's natural size; null when unknown.</summary>
    public static double? AspectFromNaturalSize(int? width, int? height) =>
        width is > 0 && height is > 0 ? width.Value / (double)height.Value : null;

    /// <summary>
    /// Computes the bug rect: <paramref name="widthFraction"/> of the canvas width,
    /// height derived from the SOURCE aspect (so the box is aspect-locked to the asset;
    /// 16:9 fallback when unknown), snapped to the requested placement inside the
    /// safe-area margin. Center/Free both center — Free is just the "I'll drag it" entry.
    /// </summary>
    public static NormalizedCanvasRect ComputeOverlayRect(
        OverlayPlacement placement,
        double? sourceAspect = null,
        double widthFraction = DefaultWidthFraction,
        double margin = DefaultSafeAreaMargin)
    {
        if (placement == OverlayPlacement.FullCanvas)
        {
            return new NormalizedCanvasRect { X = 0, Y = 0, Width = 1, Height = 1 };
        }

        var aspect = sourceAspect is { } a && double.IsFinite(a) && a > 0 ? a : FallbackSourceAspect;
        margin = double.IsFinite(margin) ? Math.Clamp(margin, 0, 0.2) : DefaultSafeAreaMargin;
        var width = double.IsFinite(widthFraction)
            ? Math.Clamp(widthFraction, MinNormalizedDimension, 1 - 2 * margin)
            : DefaultWidthFraction;

        // Normalized height for a pixel-aspect-locked box: h = w * canvasAspect / aspect.
        var height = width * CanvasAspect / aspect;

        // Enforce the canvas editor's floor while keeping the source aspect (a wide
        // banner at 15% width would otherwise collapse below the minimum box height
        // and get distorted by NormalizedCanvasRect.Clamp on the persistence path).
        if (height < MinNormalizedDimension)
        {
            height = MinNormalizedDimension;
            width = height * aspect / CanvasAspect;
        }

        // Degenerate (extremely tall/wide) sources: cap inside the safe area. This is
        // the only case where the aspect lock yields — the operator still gets a
        // draggable, grip-editable box instead of an off-canvas one.
        var maxDimension = 1 - 2 * margin;
        width = Math.Min(width, maxDimension);
        height = Math.Min(height, maxDimension);

        var x = placement switch
        {
            OverlayPlacement.TopLeft or OverlayPlacement.BottomLeft => margin,
            OverlayPlacement.TopRight or OverlayPlacement.BottomRight => 1 - margin - width,
            _ => (1 - width) / 2
        };
        var y = placement switch
        {
            OverlayPlacement.TopLeft or OverlayPlacement.TopRight => margin,
            OverlayPlacement.BottomLeft or OverlayPlacement.BottomRight => 1 - margin - height,
            _ => (1 - height) / 2
        };

        var rect = new NormalizedCanvasRect { X = x, Y = y, Width = width, Height = height };
        rect.Clamp();
        return rect;
    }

    /// <summary>
    /// Appends a new top-most overlay route to <paramref name="routes"/> — which MUST be
    /// the S2b-draft-aware list (StudioViewModel.GetPreviewEditableRoutes), never the
    /// live program routes. Returns the new route.
    /// </summary>
    public static SourceRoute AppendOverlayRoute(
        List<SourceRoute> routes,
        string sceneId,
        string optionValue,
        string? mediaAssetId,
        double? sourceAspect,
        OverlayPlacement placement,
        string? layoutHint,
        IReadOnlyList<Participant> participants)
    {
        // EnsureCanvasRects re-applies the scene's layout preset to EVERY route whenever
        // ANY rect is missing — run it BEFORE the overlay joins the list, or the next
        // composition refresh would stomp the freshly computed bug rect back to a preset
        // cell. After this, every route has a rect and EnsureCanvasRects stays a no-op.
        SceneCanvasLayoutService.EnsureCanvasRects(routes, layoutHint);

        var route = SceneRoutingService.BuildAddedSourceRoute(sceneId, routes.Count, optionValue, mediaAssetId);
        SceneRoutingService.ApplyNormalizeRouteUpdate(route, participants);

        route.CanvasRect = ComputeOverlayRect(placement, sourceAspect);
        // "fit" shows the whole asset inside the aspect-locked box: identical to "fill"
        // when the native aspect was known, letterboxes instead of cropping when the
        // 16:9 fallback was used. SourceFramingModified must be set or normalization
        // resets the fit mode back to the "fill" default on the next reconcile.
        route.FitMode = "fit";
        route.SourceFramingModified = true;
        routes.Add(route);

        // List order IS layer order (later = drawn on top): the overlay is last, so it
        // is the top-most layer; reindex so zIndex mirrors the list through the wire.
        for (var index = 0; index < routes.Count; index++)
        {
            routes[index].ZIndex = index;
        }

        return route;
    }

    /// <summary>
    /// Parses an Add-overlay flyout command parameter: "&lt;placement&gt;|&lt;option&gt;"
    /// where option is either an AddSourceOptions value (input-NN / active-speaker /
    /// screen-share / role:*) or "media:&lt;assetId&gt;" for a specific media asset.
    /// </summary>
    public static bool TryParseAddOverlayParameter(
        string? parameter,
        out OverlayPlacement placement,
        out string optionValue,
        out string? mediaAssetId)
    {
        placement = OverlayPlacement.Free;
        optionValue = string.Empty;
        mediaAssetId = null;

        if (string.IsNullOrWhiteSpace(parameter))
        {
            return false;
        }

        var separator = parameter.IndexOf('|', StringComparison.Ordinal);
        if (separator <= 0 || separator >= parameter.Length - 1)
        {
            return false;
        }

        if (PlacementFromWire(parameter[..separator]) is not { } parsedPlacement)
        {
            return false;
        }

        placement = parsedPlacement;
        var option = parameter[(separator + 1)..];
        if (option.StartsWith(ShowInputRosterService.MediaSourcePrefix, StringComparison.Ordinal))
        {
            if (!ShowInputRosterService.TryGetMediaAssetId(option, out var assetId) ||
                string.IsNullOrWhiteSpace(assetId))
            {
                // "media:" with no asset id is malformed, not a generic option.
                return false;
            }

            optionValue = "media";
            mediaAssetId = assetId;
            return true;
        }

        optionValue = option;
        return true;
    }
}
