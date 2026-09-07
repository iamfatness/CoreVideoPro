using System.Threading.Tasks;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// Gallery panel commands (Plan 7b Task 6) — the operator actions on the gallery grid. Every
/// command is a thin translation to an <c>ohg.gallery.*</c> action id via
/// <see cref="OhgActionArgs"/>, invoked through <see cref="_invoker"/>, and reported through
/// <see cref="ReportResult"/> (defined on the core partial, shared with Tasks 4/5). NONE of these
/// commands touch local state directly — the next snapshot is what renders the outcome.
/// </summary>
public sealed partial class OhgShowViewModel
{
    /// <summary>Cell order is a UI-only concept the show engine tracks; the underlying compositor
    /// (Tiles) does not yet honour a reordered gallery — surfaced here so the panel can show it
    /// rather than imply an effect that isn't real yet.</summary>
    public string GalleryNote { get; } = "Cell order is not yet applied to Tiles (carried to a core change)";

    /// <summary>Puts the currently selected slot into <paramref name="cell"/>. Refused LOCALLY
    /// (no invoke) when nothing is selected — same shape as
    /// <see cref="OhgShowViewModel.AssignSelectedToSlotAsync"/> in the Panelists partial.</summary>
    [RelayCommand]
    private async Task ReplaceCellWithSelectedSlotAsync(int cell)
    {
        if (SelectedSlot is not int slot)
        {
            LastActionStatus = "Select a slot first";
            return;
        }

        var (id, args) = OhgActionArgs.GalleryReplace(cell, slot);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task RemoveCellAsync(int cell)
    {
        var (id, args) = OhgActionArgs.GalleryRemove(cell);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task ResetGalleryFromSlotsAsync()
    {
        var (id, args) = OhgActionArgs.GalleryResetFromSlots();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task EmptyGalleryAsync()
    {
        var (id, args) = OhgActionArgs.GalleryEmpty();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task SetSmartGalleryAsync(bool on)
    {
        var (id, args) = OhgActionArgs.GallerySmartSet(on);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }
}
