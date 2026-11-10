using System;
using System.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels.Comms;
using Microsoft.UI.Dispatching;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// Comms (Zoom talkback, docs/reference/comms-talkback.md): the thin host wiring for
/// <see cref="CommsViewModel"/>. All behaviour lives in the sub-VM (strangler rule); this
/// file only feeds it the meeting phase and the core's comms facts on the UI thread, renews
/// held-key leases, and releases every key when the operator leaves the tab or the shell
/// shuts down.
/// </summary>
public sealed partial class StudioViewModel
{
    private DispatcherQueueTimer? _commsLeaseTimer;
    private bool _restoredCommsPrivate;  // preferences load before InitializeComms

    public CommsViewModel Comms { get; private set; } = null!;

    public bool IsCommsTab => ActiveTab == StudioTab.Comms;

    public TabChrome CommsTabChrome => ChromeFor(ActiveTab, StudioTab.Comms);

    private void InitializeComms()
    {
        Comms = new CommsViewModel(_bridge);
        Comms.RestorePrivatePreference(_restoredCommsPrivate);
        Comms.PrivatePreferenceChanged += _ => SaveProductionOutputPreferences();
        Comms.ApplyMeeting(Settings.IsInMeeting, CommsViewModel.MeetingLabelFor(Settings.JoinMeetingUrl));
        Settings.PropertyChanged += OnSettingsChangedForComms;
        _bridge.ZoomCommsStateReceived += OnZoomCommsStateReceived;
        // Held keys are leased in the engine (1.5 s); renew every 500 ms while held.
        _commsLeaseTimer = _dispatcher.CreateTimer();
        _commsLeaseTimer.Interval = CommsViewModel.RenewEvery;
        _commsLeaseTimer.Tick += (_, _) =>
        {
            if (Comms.AnyHeld) _ = Comms.RenewLeasesAsync();
        };
        _commsLeaseTimer.Start();
    }

    // The core's stdout reader thread: hop to the UI thread (bound state).
    private void OnZoomCommsStateReceived(ZoomCommsUpdate update) =>
        _dispatcher.TryEnqueue(() => UiDispatch.Run(_dispatcher, () => Comms.ApplyComms(update), "Comms.ApplyComms"));

    private void OnSettingsChangedForComms(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is not (nameof(SettingsViewModel.MeetingState) or nameof(SettingsViewModel.JoinMeetingUrl))) return;
        RunOnUiThread(() => Comms.ApplyMeeting(Settings.IsInMeeting, CommsViewModel.MeetingLabelFor(Settings.JoinMeetingUrl)));
    }

    // Navigating away from Comms: every held key goes up (panel-scoped keys).
    private void OnActiveTabChangedForComms(StudioTab value)
    {
        if (value != StudioTab.Comms && Comms is { AnyHeld: true }) _ = Comms.ReleaseAllAsync();
    }

    private void RestoreCommsPrivate(bool on)
    {
        _restoredCommsPrivate = on;
        Comms?.RestorePrivatePreference(on);
    }

    private bool CommsPrivateForSave() => Comms?.PrivatePreference ?? _restoredCommsPrivate;

    private void ShutdownComms()
    {
        _commsLeaseTimer?.Stop();
        _bridge.ZoomCommsStateReceived -= OnZoomCommsStateReceived;
        Settings.PropertyChanged -= OnSettingsChangedForComms;
        // The control client is going away: nobody can release a held key after this.
        _ = Comms.SendReleaseAllAsync();
    }
}
