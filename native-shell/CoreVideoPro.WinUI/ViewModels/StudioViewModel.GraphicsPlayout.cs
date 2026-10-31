using System.Collections.Specialized;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Graphics playout (issue #1076), strangler style: the ViewModel only wires the
/// real session/observe hooks into <see cref="GraphicsPlayoutService"/> and projects the
/// roster into guest entries. All playout logic lives in the service.</summary>
public sealed partial class StudioViewModel
{
    private GraphicsPlayoutService? _graphicsPlayout;
    private string _guestEntriesSignature = "";

    public GraphicsPlayoutService GraphicsPlayout => _graphicsPlayout ??= CreateGraphicsPlayout();
    public GraphicsStripState GraphicsStrip => GraphicsPlayout.Strip;
    public string GraphicsPlayoutStatus => GraphicsPlayout.Status;
    public bool GraphicsPlayoutBusy => GraphicsPlayout.Busy || GraphicsLibrary.Busy;
    /// <summary>One entry per bound roster slot of every guest row, with the Sources-page text resolved.</summary>
    public IReadOnlyList<GraphicsPlaylistEntry> GuestEntries { get; private set; } = [];
    public GraphicsPlaylistEntry? GuestEntryFor(string? sourceId) => sourceId is null ? null : GuestEntries.FirstOrDefault(e => e.SourceId == sourceId);

    private GraphicsPlayoutService CreateGraphicsPlayout()
    {
        var service = new GraphicsPlayoutService(GraphicsLibrary, InvokeGraphicsActionAsync, () => _bridge.LastSnapshot?.Graphics);
        service.Changed += (_, _) => {
            OnPropertyChanged(nameof(GraphicsPlayoutStatus)); OnPropertyChanged(nameof(GraphicsPlayoutBusy));
            RefreshGuestEntries(service);
        };
        service.StripChanged += (_, _) => OnPropertyChanged(nameof(GraphicsStrip));
        ShowInputEditors.CollectionChanged += OnShowInputEditorsChangedForPlayout;
        _ = GraphicsLibrary.LoadAsync();
        service.Observe(_bridge.LastSnapshot?.Graphics);
        RefreshGuestEntries(service);
        return service;
    }

    private void OnShowInputEditorsChangedForPlayout(object? sender, NotifyCollectionChangedEventArgs e) => RefreshGuestEntries(_graphicsPlayout);

    // ---- shot layouts (#1076 slice 2) -----------------------------------------------------
    /// <summary>The scene cue path a shot layout uses: the same SelectScene the Scenes page and
    /// scene.select / scene.cueWithRoutes go through. False when the scene is not in this show
    /// or did not land on Preview, so the layout is refused with a reason instead of skipping it.</summary>
    internal Task<bool> SelectSceneForLayoutAsync(string sceneId, CancellationToken cancellation)
    {
        if (Scenes.All(scene => !string.Equals(scene.Id, sceneId, StringComparison.Ordinal))) return Task.FromResult(false);
        SelectSceneCommand.Execute(sceneId);
        return Task.FromResult(string.Equals(PreviewSceneId, sceneId, StringComparison.Ordinal));
    }

    /// <summary>Guest entries bound to the slots a scene routes (Show Input slot routes and fixed
    /// Zoom participant routes), in route order. Empty when the scene routes nothing the roster
    /// knows; the layout then falls back to the Program subject.</summary>
    internal IReadOnlyList<GraphicsPlaylistEntry> GuestEntriesRoutedIn(string sceneId)
    {
        if (sceneId.Length == 0) return [];
        var result = new List<GraphicsPlaylistEntry>(); var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var route in GetMutableRoutes(sceneId)) {
            if (route.Mode == SourceRouteMode.None || !route.Visible) continue;
            var sourceId = route.ShowInputSlotNumber is { } slot ? ShowInputEditors.FirstOrDefault(e => e.SlotNumber == slot)?.SourceId
                : route.Mode == SourceRouteMode.Fixed && route.ParticipantId is { Length: > 0 } pid ? "zoom:" + pid : null;
            if (sourceId is not { Length: > 0 } || !seen.Add(sourceId)) continue;
            result.AddRange(GuestEntries.Where(e => e.SourceId == sourceId));
        }
        return result;
    }

    /// <summary>graphics.layout.cue: scene to Preview, then the layout's rows to Preview; one Take carries both.</summary>
    public Task<NativeGraphicsOperationResult> CueLayoutAsync(string layoutId, CancellationToken cancellation = default) =>
        GraphicsPlayout.CueLayoutAsync(layoutId, SelectSceneForLayoutAsync, ProgramSubjectGuestEntry, GuestEntriesRoutedIn(GraphicsPlayout.Layout(layoutId)?.SceneId ?? ""), cancellation);

    /// <summary>graphics.layout.take: CueLayoutAsync then the ordinary Take (TransportCoordinator.TakeAsync).</summary>
    public Task<NativeGraphicsOperationResult> CueAndTakeLayoutAsync(string layoutId, CancellationToken cancellation = default) =>
        GraphicsPlayout.CueAndTakeLayoutAsync(layoutId, SelectSceneForLayoutAsync, ProgramSubjectGuestEntry, GuestEntriesRoutedIn(GraphicsPlayout.Layout(layoutId)?.SceneId ?? ""), async _ => {
            if (!TakeCommand.CanExecute(null)) return "Take is unavailable. Queue a valid Preview scene or a changed draft and wait for any current Take to finish.";
            try { var taken = await TakeForControlAsync(); return taken.Succeeded ? null : taken.Error ?? "Take was not confirmed by the media core."; }
            catch (Exception ex) when (ex is not OutOfMemoryException) { return $"Take failed: {ex.Message}"; }
        }, cancellation);

    /// <summary>Called from ObserveGraphicsState for every native snapshot; cheap when nothing changed.</summary>
    private void ObserveGraphicsPlayout(NativeGraphicsState? state)
    {
        if (_graphicsPlayout is null) return;
        _graphicsPlayout.Observe(state);
        RefreshGuestEntries(_graphicsPlayout);
    }

    private void RefreshGuestEntries(GraphicsPlayoutService? service)
    {
        if (service is null) return;
        var slots = ShowInputEditors.Select(e => new GraphicsRosterSlot(e.SlotNumber, e.SourceId, e.DefaultDisplayName)).ToArray();
        var entries = service.GuestEntries(slots, GraphicsSourceText);
        var signature = string.Join('|', entries.Select(e => $"{e.RowId}:{e.SourceId}:{e.Label}:{e.Title}"));
        if (signature == _guestEntriesSignature) return;
        _guestEntriesSignature = signature; GuestEntries = entries;
        OnPropertyChanged(nameof(GuestEntries));
    }
}
