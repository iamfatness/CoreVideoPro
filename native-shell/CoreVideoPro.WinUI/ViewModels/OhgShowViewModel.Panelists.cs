using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.Control;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// Panelist board commands (Plan 7b Task 4) — the operator actions on the seat grid / roster.
/// Every command is a thin translation to an <c>ohg.panelist.*</c> action id via
/// <see cref="OhgActionArgs"/>, invoked through <see cref="_invoker"/>, and reported through
/// <see cref="ReportResult"/> (defined on the core partial, shared with Tasks 5/6). NONE of these
/// commands touch local state directly — the next snapshot is what renders the outcome, per the
/// class's revision-gated apply.
/// </summary>
public sealed partial class OhgShowViewModel
{
    /// <summary>The five roles the show engine recognizes (<c>show-engine/src/actions.ts</c>'s
    /// role enum), for a role picker.</summary>
    public IReadOnlyList<string> Roles { get; } = new[]
    {
        "panelist", "host", "reader", "aslpanelist", "aslinterpreter",
    };

    /// <summary>Assigns the currently selected panelist to <paramref name="slot"/>. Whether that
    /// lands as <c>ohg.panelist.add</c> or <c>ohg.panelist.replace</c> is read from the LAST
    /// PROJECTED snapshot (<see cref="Current"/>) — never guessed locally — because that is the
    /// only place "is this slot occupied" is known to be true as of the engine's last word.
    ///
    /// <para><b>The seat tap is one gesture with two meanings, and the order matters.</b> The seat
    /// button both selects the seat (its Click handler) and runs this command. With NO panelist
    /// selected the tap is a pure SELECT — it must not write a status line, because the operator
    /// asked for nothing. With a panelist selected it seats them AND CLEARS the selection: leaving
    /// it set turned the next seat tap into an unasked-for <c>ohg.panelist.replace</c> on air (tap
    /// seat 4, then tap seat 2 to look at it, and seat 2's guest was replaced by seat 4's).</para></summary>
    [RelayCommand]
    private async Task AssignSelectedToSlotAsync(int slot)
    {
        if (SelectedParticipantId is not string participantId)
        {
            // Select-only. No refusal text: "Select a panelist first" is guidance for the EXPLICIT
            // assign affordances, where the operator did ask to assign.
            _marshal(() => SelectedSlot = slot);
            return;
        }

        var occupied = Current?.Slots.FirstOrDefault(s => s.Slot == slot)?.Panelist != null;

        var (id, args) = occupied
            ? OhgActionArgs.PanelistReplace(slot, participantId)
            : OhgActionArgs.PanelistAdd(participantId, slot);

        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
        ClearSelectedParticipantOn(result);
    }

    /// <summary>A successful seating CONSUMES the selection — see the remarks above. Marshaled: the
    /// invoker's continuation may resume off the UI thread, exactly like
    /// <see cref="ReportResult"/>.</summary>
    private void ClearSelectedParticipantOn(ControlInvokeResult result)
    {
        if (!result.Ok) return;
        _marshal(() => SelectedParticipantId = null);
    }

    /// <summary>Adds the currently selected panelist to whichever empty slot the engine picks
    /// (the one-arg form of <c>ohg.panelist.add</c> — no slot argument at all).</summary>
    [RelayCommand]
    private async Task AddSelectedToFirstEmptyAsync()
    {
        if (SelectedParticipantId is not string participantId)
        {
            LastActionStatus = "Select a panelist first";
            return;
        }

        var (id, args) = OhgActionArgs.PanelistAdd(participantId);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
        ClearSelectedParticipantOn(result);
    }

    /// <summary>Removes whoever is seated in <paramref name="slot"/> (a no-op on the engine side
    /// if the slot is already empty — this command does not pre-check).</summary>
    [RelayCommand]
    private async Task RemoveSlotAsync(int slot)
    {
        var (id, args) = OhgActionArgs.PanelistRemove(slot);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    /// <summary>Sets a panelist's role by PIN. A panelist with no PIN (no Mukana registration) has
    /// nothing for <c>ohg.panelist.role.set</c> to key off — refused LOCALLY, before any invoke,
    /// rather than sending an empty PIN the engine would just refuse anyway.</summary>
    [RelayCommand]
    private async Task SetRoleAsync((string Pin, string Role) arg)
    {
        if (string.IsNullOrEmpty(arg.Pin))
        {
            LastActionStatus = "Panelist has no PIN; set a Mukana override instead";
            return;
        }

        var (id, args) = OhgActionArgs.PanelistRoleSet(arg.Pin, arg.Role);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    /// <summary>Re-syncs every seated panelist's Mukana-derived fields (name/location/role) from
    /// the roster, e.g. after a bulk override edit.</summary>
    [RelayCommand]
    private async Task SyncAllAsync()
    {
        var (id, args) = OhgActionArgs.PanelistSyncAll();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }
}
