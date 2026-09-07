using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.Control;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// The OHG show workspace's view model (Plan 7b Task 3) — the threading-sensitive heart of the
/// page. It ingests the show engine's snapshot/health/log events, which ALL arrive on the engine's
/// reader thread, and projects them into bound collections and status-strip scalars.
///
/// Two laws govern this class:
///
/// 1. <b>Every ingest path goes through <c>marshal</c>.</b> <see cref="OnSnapshot"/>,
///    <see cref="OnHealth"/> and <see cref="OnLog"/> are called from the bridge's reader thread;
///    none of them touches state directly. The production marshal is <c>UiDispatch.Run</c>, which
///    runs INLINE when already on the dispatcher — so routing through it even from the UI thread
///    costs nothing and keeps the rule uniform (there is no "this one is already safe" exception to
///    get wrong later). Tests pass <c>a =&gt; a()</c>.
/// 2. <b>Bound collections are diff-updated in place, never replaced.</b> See CLAUDE.md, "The
///    crash class you WILL hit: CoreMessagingXP 0xc000027b" — a <c>Clear()</c>+<c>Add</c> at
///    snapshot rate is the documented fail-fast signature. All keyed collections go through
///    <see cref="ObservableCollectionSync"/>.
///
/// The class is NOT constructed with a <c>DispatcherQueue</c>: everything it needs arrives as
/// plain data or delegates, which is what makes the whole ingestion path unit-testable.
/// </summary>
public sealed partial class OhgShowViewModel : ObservableObject, IDisposable
{
    private readonly IOhgActionInvoker _invoker;
    private readonly Action<Action> _marshal;

    private ShowEngineBridge? _bridge;
    private EventHandler<ShowEngineSnapshot>? _snapshotHandler;
    private EventHandler<ShowEngineHealth>? _healthHandler;
    private EventHandler<ShowEngineLogLine>? _logHandler;

    /// <summary>The revision the bound state currently reflects. <c>_hasApplied</c> is separate so
    /// revision 0 (a fresh engine) is still applied once.</summary>
    private long _appliedRevision;
    private bool _hasApplied;

    /// <summary>The generation the bound state currently reflects. Paired with
    /// <see cref="_appliedRevision"/> in the envelope gate in <see cref="OnSnapshot"/> — a
    /// snapshot from a NEW generation (e.g. a respawned engine that happens to start back at the
    /// same revision number) must always be applied, never mistaken for a repeat.</summary>
    private int _appliedGeneration;

    /// <summary>Deduplicates the projection's own "malformed wire node" warnings so a persistently
    /// bad snapshot names itself ONCE in the strip rather than every tick.</summary>
    private string _lastProjectionWarningSignature = "";

    private const int MaxRecentRefusals = 10;

    public OhgShowViewModel(
        IOhgActionInvoker invoker,
        Action<Action> marshal,
        Func<ShowEngineSnapshot?> latest,
        Func<ShowEngineHealth> health,
        IReadOnlyList<OhgLookOption> looks,
        bool driveHost)
    {
        _invoker = invoker ?? throw new ArgumentNullException(nameof(invoker));
        _marshal = marshal ?? throw new ArgumentNullException(nameof(marshal));
        ArgumentNullException.ThrowIfNull(latest);
        ArgumentNullException.ThrowIfNull(health);
        Looks = looks ?? Array.Empty<OhgLookOption>();

        isShadowMode = !driveHost;

        // Apply what the bridge already holds BEFORE the first event, so the page never renders a
        // blank frame while a perfectly good snapshot sits in the bridge.
        var current = latest();
        if (current != null) OnSnapshot(current);
        OnHealth(health());
    }

    // ── configured data ───────────────────────────────────────────────────────────────

    /// <summary>The configured looks for the picker (Task 5). From config, not the snapshot.</summary>
    public IReadOnlyList<OhgLookOption> Looks { get; }

    /// <summary>The last projected view — the other partials (Tasks 4/5) and the tests read the
    /// full snapshot shape from here rather than re-projecting.</summary>
    public OhgSnapshotView? Current { get; private set; }

    // ── bound collections (diff-updated in place — NEVER replaced) ────────────────────

    public ObservableCollection<OhgPanelistRowViewModel> Panelists { get; } = new();
    public ObservableCollection<OhgSlotRowViewModel> Slots { get; } = new();
    public ObservableCollection<OhgGalleryCellViewModel> Gallery { get; } = new();
    public ObservableCollection<OhgPanelistRowViewModel> Unseated { get; } = new();

    /// <summary>The engine's <c>restoreWarnings</c> — a state restore that could not be fully
    /// honoured is operator-visible, never silent.</summary>
    public ObservableCollection<string> RestoreWarnings { get; } = new();

    /// <summary>The last <see cref="MaxRecentRefusals"/> warn/error log lines, newest first.</summary>
    public ObservableCollection<string> RecentRefusals { get; } = new();

    // ── status strip + page state ─────────────────────────────────────────────────────

    [ObservableProperty] private string engineState = "stopped";
    [ObservableProperty] private string engineDetail = "";
    [ObservableProperty] private bool isShadowMode;
    [ObservableProperty] private string shadowLastCommand = "";
    [ObservableProperty] private string pagingRefused = "";
    [ObservableProperty] private long revision;

    /// <summary>The result text of the last operator action — <c>""</c> on success. Defined here
    /// because it is the ONE status line the whole workspace writes to (Task 4's action commands
    /// reuse it).</summary>
    [ObservableProperty] private string lastActionStatus = "";

    /// <summary>Page selection. Untouched by <see cref="Apply"/> except when the selected key no
    /// longer exists in the new view, in which case it is cleared rather than left dangling.</summary>
    [ObservableProperty] private string? selectedParticipantId;
    [ObservableProperty] private int? selectedSlot;

    /// <summary>Fed by the control surface when shadow mode swallows a host command.</summary>
    public void SetShadowLastCommand(string command) => _marshal(() => ShadowLastCommand = command ?? "");

    // ── bridge wiring (production only; tests drive the ingest methods directly) ──────

    /// <summary>Subscribes to the bridge's three event streams. Call from the UI thread;
    /// <see cref="Dispose"/> unsubscribes. The handlers themselves are thread-safe by construction
    /// (they marshal), so the events keep firing on the engine's reader thread as they always do.</summary>
    public void Attach(ShowEngineBridge bridge)
    {
        ArgumentNullException.ThrowIfNull(bridge);
        Detach();

        _bridge = bridge;
        _snapshotHandler = (_, s) => OnSnapshot(s);
        _healthHandler = (_, h) => OnHealth(h);
        _logHandler = (_, l) => OnLog(l);

        bridge.SnapshotChanged += _snapshotHandler;
        bridge.HealthChanged += _healthHandler;
        bridge.Log += _logHandler;
    }

    public void Dispose() => Detach();

    private void Detach()
    {
        if (_bridge == null) return;
        if (_snapshotHandler != null) _bridge.SnapshotChanged -= _snapshotHandler;
        if (_healthHandler != null) _bridge.HealthChanged -= _healthHandler;
        if (_logHandler != null) _bridge.Log -= _logHandler;
        _bridge = null;
        _snapshotHandler = null;
        _healthHandler = null;
        _logHandler = null;
    }

    // ── ingestion (any thread) ────────────────────────────────────────────────────────

    /// <summary>May be called from any thread — marshals. Projection happens INSIDE the marshal so
    /// the whole snapshot→view→collections hop is one unit of UI work.</summary>
    internal void OnSnapshot(ShowEngineSnapshot snapshot)
    {
        if (snapshot == null) return;
        _marshal(() =>
        {
            // Envelope gate BEFORE the JSON walk: the engine republishes on a cadence, and a
            // re-published (generation, revision) pair is pure churn. Compare generation too — a
            // respawned engine can legitimately restart its revision counter, and a
            // revision-only gate would wrongly swallow that first post-respawn snapshot.
            if (_hasApplied && snapshot.Generation == _appliedGeneration && snapshot.Revision == _appliedRevision)
            {
                return;
            }

            var view = OhgSnapshotProjection.Project(snapshot.Snapshot, out var warnings);
            NoteProjectionWarnings(warnings);
            Apply(view);
            _appliedGeneration = snapshot.Generation;
        });
    }

    /// <summary>May be called from any thread — marshals.</summary>
    internal void OnHealth(ShowEngineHealth health)
    {
        if (health == null) return;
        _marshal(() =>
        {
            EngineState = health.State.ToString().ToLowerInvariant();
            EngineDetail = health.LastError ?? $"gen {health.Generation}, {health.RestartCount} restarts";
        });
    }

    /// <summary>May be called from any thread — marshals. Only warn/error lines reach the strip:
    /// info is the engine's ordinary chatter and would push the refusals off the top.</summary>
    internal void OnLog(ShowEngineLogLine line)
    {
        if (line == null) return;
        _marshal(() =>
        {
            if (!IsRefusalLevel(line.Level)) return;
            PushRefusal($"{line.Level}: {line.Message}");
        });
    }

    private static bool IsRefusalLevel(string? level)
        => string.Equals(level, "warn", StringComparison.OrdinalIgnoreCase)
           || string.Equals(level, "error", StringComparison.OrdinalIgnoreCase);

    private void PushRefusal(string text)
    {
        RecentRefusals.Insert(0, text);
        while (RecentRefusals.Count > MaxRecentRefusals)
        {
            RecentRefusals.RemoveAt(RecentRefusals.Count - 1);
        }
    }

    /// <summary>A malformed wire node is a real defect (the projection is total, so it would
    /// otherwise vanish into a neutral value). Surfaced once per distinct warning set.</summary>
    private void NoteProjectionWarnings(IReadOnlyList<string> warnings)
    {
        if (warnings == null || warnings.Count == 0)
        {
            _lastProjectionWarningSignature = "";
            return;
        }

        var signature = string.Join("|", warnings);
        if (signature == _lastProjectionWarningSignature) return;
        _lastProjectionWarningSignature = signature;
        PushRefusal($"warn: snapshot projection — {warnings[0]}" +
                    (warnings.Count > 1 ? $" (+{warnings.Count - 1} more)" : ""));
    }

    // ── apply (always on the marshal's thread) ────────────────────────────────────────

    /// <summary>Revision-gated: the engine republishes on a cadence, and re-running the diff for a
    /// revision already on screen is pure churn (which is the thing that fail-fasts WinUI).</summary>
    private void Apply(OhgSnapshotView view)
    {
        // The envelope gate in OnSnapshot already refused a genuine repeat before this runs;
        // Apply always does the work when called.
        _hasApplied = true;
        _appliedRevision = view.Revision;

        ObservableCollectionSync.Apply(
            Panelists, view.Panelists,
            static row => row.Key, static incoming => incoming.ParticipantId,
            static incoming => new OhgPanelistRowViewModel(incoming),
            static (row, incoming) => row.Update(incoming));

        ObservableCollectionSync.Apply(
            Slots, view.Slots,
            static row => row.Key, static incoming => incoming.Slot,
            static incoming => new OhgSlotRowViewModel(incoming),
            static (row, incoming) => row.Update(incoming));

        ObservableCollectionSync.Apply(
            Gallery, view.Gallery,
            static row => row.Key, static incoming => incoming.Cell,
            static incoming => new OhgGalleryCellViewModel(incoming),
            static (row, incoming) => row.Update(incoming));

        ObservableCollectionSync.Apply(
            Unseated, view.Unseated,
            static row => row.Key, static incoming => incoming.ParticipantId,
            static incoming => new OhgPanelistRowViewModel(incoming),
            static (row, incoming) => row.Update(incoming));

        Revision = view.Revision;
        PagingRefused = view.PagingRefused ?? "";
        ObservableCollectionSync.ApplyStrings(RestoreWarnings, view.RestoreWarnings);

        Current = view;

        ClearDepartedSelection(view);

        // Rows are diff-updated in place (never replaced), so a newly INSERTED row (a newcomer)
        // starts with IsSelected=false regardless of the page's current selection — reassert it
        // here rather than relying on the property-changed hooks below, which only fire on a
        // NEW selection, not on every snapshot.
        ApplyParticipantSelectionFlag();
        ApplySlotSelectionFlag();
    }

    /// <summary>Selection is PAGE state and survives ingestion — a snapshot must never yank the
    /// operator's cursor. The one exception: a key that no longer exists cannot be acted on, so it
    /// is cleared instead of silently pointing at nothing.</summary>
    private void ClearDepartedSelection(OhgSnapshotView view)
    {
        if (SelectedParticipantId is string participantId &&
            !view.Panelists.Any(p => p.ParticipantId == participantId) &&
            !view.Unseated.Any(p => p.ParticipantId == participantId))
        {
            SelectedParticipantId = null;
        }

        if (SelectedSlot is int slot && !view.Slots.Any(s => s.Slot == slot))
        {
            SelectedSlot = null;
        }
    }

    // ── selection → row IsSelected (Task 7's templates bind IsSelected) ───────────────

    /// <summary>Generated CommunityToolkit.Mvvm hook — fires on every real change to
    /// <see cref="SelectedParticipantId"/>.</summary>
    partial void OnSelectedParticipantIdChanged(string? oldValue, string? newValue) => ApplyParticipantSelectionFlag();

    /// <summary>Generated CommunityToolkit.Mvvm hook — fires on every real change to
    /// <see cref="SelectedSlot"/>.</summary>
    partial void OnSelectedSlotChanged(int? oldValue, int? newValue) => ApplySlotSelectionFlag();

    /// <summary>Exactly the row whose key matches <see cref="SelectedParticipantId"/> (in either
    /// <see cref="Panelists"/> or <see cref="Unseated"/>) carries <c>IsSelected=true</c>; every
    /// other row is cleared. Each row's generated setter no-ops when the value does not change, so
    /// this costs no PropertyChanged traffic on an unaffected row.</summary>
    private void ApplyParticipantSelectionFlag()
    {
        foreach (var row in Panelists) row.IsSelected = row.Key == SelectedParticipantId;
        foreach (var row in Unseated) row.IsSelected = row.Key == SelectedParticipantId;
    }

    /// <summary>Exactly the row whose key matches <see cref="SelectedSlot"/> carries
    /// <c>IsSelected=true</c>; every other row is cleared.</summary>
    private void ApplySlotSelectionFlag()
    {
        foreach (var row in Slots) row.IsSelected = row.Key == SelectedSlot;
    }

    // ── commands ──────────────────────────────────────────────────────────────────────

    /// <summary>Restarts the show engine. The invoker never throws; a failure lands in
    /// <see cref="LastActionStatus"/>, and the continuation writes it through the marshal because
    /// it may resume off the UI thread.</summary>
    [RelayCommand]
    private async Task RestartEngineAsync()
    {
        var result = await _invoker.RestartEngineAsync().ConfigureAwait(false);
        var status = result.Ok ? "" : (result.Error ?? "Restart failed.");
        _marshal(() => LastActionStatus = status);
    }

    /// <summary>Shared result-reporting for every <c>ohg.*</c> action command (Task 4/5/6):
    /// <see cref="LastActionStatus"/> becomes <c>""</c> on success, or the invoker's error text on
    /// failure — and a failure is ALSO pushed onto <see cref="RecentRefusals"/> so a refused
    /// operator action shows up in the same strip as an engine-side warn/error log line. Routed
    /// through <see cref="_marshal"/> because the invoker's continuation may resume off the UI
    /// thread.</summary>
    internal void ReportResult(ControlInvokeResult result)
    {
        var status = result.Ok ? "" : (result.Error ?? "Action failed.");
        _marshal(() =>
        {
            LastActionStatus = status;
            if (!result.Ok) PushRefusal(status);
        });
    }
}
