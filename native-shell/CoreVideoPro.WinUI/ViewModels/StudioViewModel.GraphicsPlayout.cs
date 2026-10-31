using System.Collections.Specialized;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

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
