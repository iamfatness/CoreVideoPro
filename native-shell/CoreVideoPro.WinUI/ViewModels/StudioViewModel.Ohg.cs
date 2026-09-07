using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// The ONE ViewModel entry point the OHG show engine needs that no existing operator path
/// already exposes (Plan 7a Task 11, spec §8's <c>applyLook</c> / <c>setPreview</c> rows).
///
/// Everything else the engine asks for already has an operator-equivalent entry point that the
/// control surface uses — <c>SelectSceneCommand</c>, <c>ShowInputEditors[n]</c>,
/// <c>TakeForControlAsync</c>, <c>SetTakeTransitionCommand</c>, <c>CaptionText</c>. Pointing a
/// NAMED route at a Show Input slot did not: the canvas editor does it one layer at a time
/// through <c>SceneCanvasLayerViewModel.ApplyRoute</c> (by layer INDEX, driven by a picker
/// string), and spec D11 is explicit that looks address routes by ID precisely so a reordered
/// layer cannot silently swap guests.
///
/// It is kept move-minimal: it edits the SAME preview working routes the canvas editor edits
/// (<c>GetPreviewEditableRoutes</c> — the S2b draft, so a scene that is live on PROGRAM is
/// untouched until Take/Update) and republishes through the SAME path
/// (<c>SyncPreviewCanvasLayers</c> + <c>PublishPreviewCompositionState</c> +
/// <c>SchedulePreviewRoutingRefresh</c> + <c>SyncLiveSceneEditIfNeeded</c>). The per-route
/// rewrite itself lives in <see cref="OhgRouteSlotWriter"/>, pure and unit-tested.
/// </summary>
public sealed partial class StudioViewModel
{
    /// <summary>The media-core bridge, exposed ONLY so app startup can subscribe the OHG roster
    /// publisher to the same snapshot stream the ViewModel consumes. Read-only handle; nothing
    /// outside the ViewModel may command the core through it.</summary>
    internal IMediaCoreBridge MediaCoreBridge => _bridge;

    /// <summary>The OHG show workspace view model, or <c>null</c> when OHG is not configured on
    /// this machine. Assigned by <c>MainWindow</c> at startup (Task 10) — the ViewModel never
    /// constructs it, because whether the show engine runs at all is an app-composition decision
    /// (config present + host resolvable), not a workspace one. The page binds this directly and
    /// renders its setup surface while it is null.</summary>
    [ObservableProperty] private OhgShowViewModel? _ohgShow;

    /// <summary>Opens the production settings window on the OHG section. The section itself is
    /// registered in Task 10; until then <c>ShowSection("ohg")</c> is a no-op that still opens the
    /// window, which is strictly better than a button that does nothing at all.</summary>
    [RelayCommand]
    private void OpenOhgSettings() => OpenProductionSettingsSection("ohg");

    /// <summary>
    /// Point the named PREVIEW routes at Show Input slots. Semantics (the Task 10 adapter
    /// contract):
    /// <list type="bullet">
    /// <item>a route id present with a slot ⇒ that route carries Show Input <c>slot</c>;</item>
    /// <item>a route id present with <c>null</c> ⇒ that route carries NOTHING
    /// (<c>SourceRouteMode.None</c>) — an explicitly empty box;</item>
    /// <item>a route id ABSENT from the dictionary is left exactly as it was — "not this look's
    /// business" (which is why an EMPTY dictionary rewrites nothing at all).</item>
    /// </list>
    /// Returns the requested route ids that the preview scene does not contain, so the caller can
    /// report a partly-wired look rather than applying it silently.
    /// <para>MUST be called on the UI thread — it mutates bound collections and raises
    /// PropertyChanged.</para>
    /// </summary>
    internal IReadOnlyList<string> SetPreviewRouteSlots(IReadOnlyDictionary<string, int?> routeSlots)
    {
        if (routeSlots is null || routeSlots.Count == 0)
        {
            return System.Array.Empty<string>();
        }

        var routes = GetPreviewEditableRoutes();
        var missing = new List<string>();
        var changed = false;

        foreach (var (routeId, slot) in routeSlots)
        {
            var route = routes.FirstOrDefault(candidate => string.Equals(candidate.Id, routeId, StringComparison.Ordinal));
            if (route is null)
            {
                missing.Add(routeId);
                continue;
            }

            // The roster slot is resolved HERE (not at publish time) so an unassigned slot still
            // clears the route's previous source — publish-time resolution returns early on an
            // unassigned slot and would leave the last guest on air.
            var resolvedSlot = slot is { } slotNumber
                ? ShowInputs.FirstOrDefault(candidate => candidate.SlotNumber == slotNumber)
                : null;
            OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, slot, resolvedSlot);
            changed = true;
        }

        if (!changed)
        {
            return missing;
        }

        // Exactly the republish the canvas editor does after a layer edit (AddSourceLayer /
        // AddOverlayLayer): resolve slot -> concrete source for the preview bus, refresh the
        // canvas layer view models IN PLACE, and let the coalesced routing refresh carry it to
        // the core. Never a frame-rate rebuild of a bound collection.
        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(PreviewScene, routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
        return missing;
    }
}
