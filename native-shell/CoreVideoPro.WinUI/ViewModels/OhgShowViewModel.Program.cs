using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// Program panel commands and labels (Plan 7b Task 5) — the take/preview/look/box surface. Every
/// command is a thin translation to an <c>ohg.program.*</c>/<c>ohg.look.*</c> action id via
/// <see cref="OhgActionArgs"/>, invoked through <see cref="_invoker"/>, and reported through
/// <see cref="ReportResult"/> (defined on the core partial, shared with Tasks 4/6). The three
/// labels are computed PURELY from <see cref="Current"/> and re-raised at the end of every
/// <see cref="Apply"/> via the <see cref="OnApplied"/> hook — never notified from a command, since
/// a command's outcome is only known truth once the next snapshot lands.
/// </summary>
public sealed partial class OhgShowViewModel
{
    /// <summary>The look boxes for the currently cued look (Task 5) — empty when
    /// <c>Current.Look</c> is null. Diff-updated in place, keyed by box number, from
    /// <see cref="OnApplied"/>.</summary>
    public ObservableCollection<OhgBoxViewModel> Boxes { get; } = new();

    /// <summary>The look id the operator most recently asked for (set optimistically by
    /// <see cref="SetLookAsync"/>) — overwritten by the engine's actual current look at the end of
    /// EVERY <see cref="Apply"/>, because the snapshot is the truth and a refused
    /// <c>ohg.look.set</c> must not leave a stale optimistic value on screen.</summary>
    [ObservableProperty] private string? selectedLookId;

    // ── apply hook (Task 3's Apply calls this after every applied snapshot) ────────────

    partial void OnApplied(OhgSnapshotView view)
    {
        ObservableCollectionSync.Apply(
            Boxes, view.Look?.Boxes ?? Array.Empty<OhgBoxView>(),
            static row => row.Key, static incoming => incoming.Box,
            static incoming => new OhgBoxViewModel(incoming),
            static (row, incoming) => row.Update(incoming));

        SelectedLookId = view.Look?.LookId;

        OnPropertyChanged(nameof(ProgramLabel));
        OnPropertyChanged(nameof(PreviewLabel));
        OnPropertyChanged(nameof(CurrentSpeakerLabel));
        OnPropertyChanged(nameof(QueueLabel));
    }

    // ── labels (pure, computed over Current only) ──────────────────────────────────────

    /// <summary>The wire <c>ohg.program.program</c> source, formatted for the operator. See
    /// <see cref="FormatProgramSourceLabel"/> for the format rules.</summary>
    public string ProgramLabel => FormatProgramSourceLabel(Current?.Program.Program);

    /// <summary>The wire <c>ohg.program.preview</c> source, formatted for the operator.</summary>
    public string PreviewLabel => FormatProgramSourceLabel(Current?.Program.Preview);

    /// <summary>Formats a wire <c>ProgramSource</c> string (<c>black | gallery | activeSpeaker |
    /// look:&lt;id&gt; | slot:&lt;n&gt;</c>, per <c>OhgActionArgs</c>/<c>OhgSnapshotProjection</c>)
    /// into operator text:
    /// <list type="bullet">
    /// <item><c>black</c> → <c>"black"</c>, <c>gallery</c> → <c>"gallery"</c>,
    /// <c>activeSpeaker</c> → <c>"active speaker"</c>.</item>
    /// <item><c>look:&lt;id&gt;</c> → <c>"look: &lt;Label&gt; (page P/N)"</c> — the label comes
    /// from <see cref="Looks"/> (falling back to the raw id when unconfigured); the page suffix
    /// is appended ONLY when this is the CURRENT look (<c>Current.Look.LookId == id</c>), using
    /// <c>Current.Look.Page + 1</c> (the wire is 0-based) over <c>PageCount</c> — a look
    /// referenced by program/preview that is not the currently-cued one carries no page.</item>
    /// <item><c>slot:&lt;n&gt;</c> → <c>"slot n: &lt;DisplayName&gt;"</c>, resolved from
    /// <see cref="OhgSnapshotView.Slots"/>; an empty slot (no panelist) reads
    /// <c>"slot n: EMPTY"</c>.</item>
    /// </list>
    /// A missing/malformed source (no <see cref="Current"/>, or a shape none of the above match)
    /// falls back to <c>"black"</c> — the same neutral the projection itself uses.</summary>
    private string FormatProgramSourceLabel(string? source)
    {
        if (string.IsNullOrEmpty(source) || source == "black") return "black";
        if (source == "gallery") return "gallery";
        if (source == "activeSpeaker") return "active speaker";

        if (source.StartsWith("look:", StringComparison.Ordinal))
        {
            var lookId = source.Substring("look:".Length);
            var label = Looks.FirstOrDefault(l => l.Id == lookId)?.Label ?? lookId;

            if (Current?.Look is { } look && look.LookId == lookId)
            {
                return $"look: {label} (page {look.Page + 1}/{look.PageCount})";
            }

            return $"look: {label}";
        }

        if (source.StartsWith("slot:", StringComparison.Ordinal) &&
            int.TryParse(source.AsSpan("slot:".Length), out var slotNum))
        {
            var name = Current?.Slots.FirstOrDefault(s => s.Slot == slotNum)?.Panelist?.DisplayName;
            return string.IsNullOrEmpty(name) ? $"slot {slotNum}: EMPTY" : $"slot {slotNum}: {name}";
        }

        return "black";
    }

    /// <summary>The display name of the panelist whose participant id matches
    /// <c>Current.Program.ActiveSpeakerId</c> (checked in both <see cref="Panelists"/> and
    /// <see cref="Unseated"/> — an active speaker can be seated OR not), or <c>"—"</c> when there
    /// is no active speaker / no match.</summary>
    public string CurrentSpeakerLabel
    {
        get
        {
            var activeSpeakerId = Current?.Program.ActiveSpeakerId;
            if (string.IsNullOrEmpty(activeSpeakerId)) return "—";

            var match = Current!.Panelists.FirstOrDefault(p => p.ParticipantId == activeSpeakerId)
                ?? Current.Unseated.FirstOrDefault(p => p.ParticipantId == activeSpeakerId);

            return match?.DisplayName is { Length: > 0 } name ? name : "—";
        }
    }

    /// <summary>The hands queue, e.g. <c>"prev: 0042, 0017 · current: 0099 · next: 0003, 0120"</c>
    /// — the raw queue ids are shown verbatim (the queue carries opaque ids, not resolved names);
    /// an empty previous/current/next section reads <c>"—"</c>.</summary>
    public string QueueLabel
    {
        get
        {
            var queue = Current?.Queue;
            var previous = queue != null && queue.Previous.Count > 0 ? string.Join(", ", queue.Previous) : "—";
            var current = !string.IsNullOrEmpty(queue?.Current) ? queue!.Current : "—";
            var upcoming = queue != null && queue.Upcoming.Count > 0 ? string.Join(", ", queue.Upcoming) : "—";
            return $"prev: {previous} · current: {current} · next: {upcoming}";
        }
    }

    // ── program commands ───────────────────────────────────────────────────────────────

    [RelayCommand]
    private async Task PreviewAsync(string source)
    {
        var (id, args) = OhgActionArgs.ProgramPreview(source);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task CutAsync()
    {
        var (id, args) = OhgActionArgs.ProgramCut();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task AutoAsync()
    {
        var (id, args) = OhgActionArgs.ProgramAuto();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task DirectCutAsync(string source)
    {
        var (id, args) = OhgActionArgs.ProgramDirectCut(source);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task SetAsFollowAsync(bool on)
    {
        var (id, args) = OhgActionArgs.ProgramAsFollowSet(on);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    /// <summary>The <c>slot:&lt;n&gt;</c> wire-form convenience over <see cref="PreviewAsync"/> —
    /// calls straight through to the SAME command body (never a second invoke path) so
    /// <c>OhgActionArgs.SourceForSlot</c> is the ONE place the wire string is built.</summary>
    [RelayCommand]
    private Task PreviewSlotAsync(int slot) => PreviewAsync(OhgActionArgs.SourceForSlot(slot));

    // ── look commands ──────────────────────────────────────────────────────────────────

    /// <summary>Cues a look. <see cref="SelectedLookId"/> is set OPTIMISTICALLY before the invoke
    /// so the picker reflects the operator's choice immediately — <see cref="OnApplied"/>
    /// overwrites it with the engine's actual current look on the next snapshot regardless of
    /// whether this call succeeded, which is what makes a refused <c>ohg.look.set</c> self-correct
    /// rather than leave a stale picker selection.</summary>
    [RelayCommand]
    private async Task SetLookAsync(string lookId)
    {
        SelectedLookId = lookId;
        var (id, args) = OhgActionArgs.LookSet(lookId);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task NextGuestAsync()
    {
        var (id, args) = OhgActionArgs.LookNextGuest();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task PrevGuestAsync()
    {
        var (id, args) = OhgActionArgs.LookPrevGuest();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task AssignBoxAsync((int Box, int Slot) assignment)
    {
        var (id, args) = OhgActionArgs.LookBoxAssign(assignment.Box, assignment.Slot);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task ClearBoxAsync(int box)
    {
        var (id, args) = OhgActionArgs.LookBoxClear(box);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    /// <summary>Convenience over <see cref="AssignBoxAsync"/> for a box tile that just wants "put
    /// whatever slot is selected here" — refuses LOCALLY (no invoke) when nothing is selected,
    /// same shape as <see cref="AssignSelectedToSlotAsync"/> in the Panelists partial.</summary>
    [RelayCommand]
    private Task AssignSelectedSlotToBoxAsync(int box)
    {
        if (SelectedSlot is not int slot)
        {
            LastActionStatus = "Select a slot first";
            return Task.CompletedTask;
        }

        return AssignBoxAsync((box, slot));
    }
}
