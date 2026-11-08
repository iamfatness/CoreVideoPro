using System.Text.Json;
using System.Text.RegularExpressions;
using CoreVideoPro.Control;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private static readonly Regex VoltClipKey = new(@"\A[a-z0-9-]{1,72}\z", RegexOptions.CultureInvariant);

    private MediaAsset? VoltRundownClip(string key)
    {
        if (!VoltClipKey.IsMatch(key))
        {
            return null;
        }
        var relative = Path.Combine(MediaBinClassifier.VoltRundownFolder, key + ".mp4");
        var expected = Path.Combine(MediaBinClassifier.DefaultMediaRoot, relative);
        var asset = FindMediaAsset(MediaBinClassifier.BuildAssetId(relative));
        return asset is not null && string.Equals(asset.FilePath, expected, StringComparison.OrdinalIgnoreCase)
            && File.Exists(expected) ? asset : null;
    }

    internal ControlInvokeResult LoadVoltRundownClip(string key)
    {
        if (!VoltClipKey.IsMatch(key))
        {
            return ControlInvokeResult.Fail("Invalid staged clip key.");
        }
        RefreshMediaBin();
        var clip = VoltRundownClip(key);
        return clip is null
            ? ControlInvokeResult.Fail("The staged clip is missing from the local Volt Rundown bin.")
            : ControlInvokeResult.WithData(JsonSerializer.Serialize(new { sourceId = "media:" + clip.Id }));
    }

    internal ControlInvokeResult PreviewVoltRundownClip(string key, string scene, string routeId)
    {
        var clip = VoltRundownClip(key);
        if (clip is null)
        {
            return ControlInvokeResult.Fail("The staged clip is missing. Send it from Volt again.");
        }
        if (scene == ActiveSceneId || VoltClipTakeBusy)
        {
            return ControlInvokeResult.Fail("The clip scene must be off Program before cueing another clip.");
        }
        if (!Scenes.Any(candidate => candidate.Id == scene) || FindVoltClipRoute(scene, routeId) is not { } route)
        {
            return ControlInvokeResult.Fail("The clip scene or its editable video route does not exist.");
        }
        SelectSceneCommand.Execute(scene);
        var routes = GetMutableRoutes(scene);
        if (!ReferenceEquals(WorkspaceRoutes(), routes))
        {
            return ControlInvokeResult.Fail("Save or discard the clip scene's draft before cueing from Volt.");
        }
        if (PreviewSceneId != scene || !routes.Contains(route))
        {
            return ControlInvokeResult.Fail("The clip route is unavailable on Preview.");
        }
        route.Mode = SourceRouteMode.Fixed;
        route.ParticipantId = "media:" + clip.Id;
        route.ShowInputSlotNumber = null;
        route.CaptureDeviceId = null;
        route.ProductionRoleId = null;
        route.SpotlightIndex = null;
        WorkspaceRefresh();
        return ControlInvokeResult.AppliedPreview(scene);
    }

    internal string? CheckVoltRundownPreview(string key, string scene, string routeId)
    {
        var clip = VoltRundownClip(key);
        if (clip is null || PreviewSceneId != scene || ActiveSceneId == scene || VoltClipTakeBusy ||
            !ReferenceEquals(WorkspaceRoutes(), GetMutableRoutes(scene)) || FindVoltClipRoute(scene, routeId) is not { } route ||
            route.Mode != SourceRouteMode.Fixed || route.ShowInputSlotNumber is not null || route.ParticipantId != "media:" + clip.Id)
        {
            return "The staged Volt clip must be cued on Preview before Take.";
        }
        var source = LatestMediaSources.FirstOrDefault(candidate => candidate.MediaAssetId == clip.Id);
        if (source?.OnProgram == true)
        {
            return "The staged clip is already on Program. Take it off Program before replaying it.";
        }
        if (source is null || !source.OnPreview || source.DecodedVideoFrames is not > 0)
        {
            return "Wait for the staged clip to decode on Preview before Take.";
        }
        return null;
    }

    private bool VoltClipTakeBusy => _transportCoordinator.TakeInFlight || _transportCoordinator.GuardedTakePending ||
        _graphicsPlayout?.LayoutPreparing == true || _graphicsPlayout?.PreparedLayout is not null;

    private SourceRoute? FindVoltClipRoute(string scene, string routeId)
    {
        var routes = GetMutableRoutes(scene)
            .Where(route => route.Visible && route.Opacity > 0 && !route.EditorLocked && route.Graphic is null &&
                (routeId.Length == 0 || route.Id == routeId)).Take(2).ToArray();
        return routes.Length == 1 ? routes[0] : null;
    }
}
