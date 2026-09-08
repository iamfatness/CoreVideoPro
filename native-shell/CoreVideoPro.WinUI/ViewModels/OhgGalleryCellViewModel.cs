using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>One gallery cell (fixed 16), mirroring <see cref="OhgGalleryCellRow"/>.
/// <c>Slot == 0</c> is the engine's BLANK cell — kept as a live row rather than an absent one so
/// the 4x4 grid never restructures (see <see cref="ObservableCollectionSync"/>).</summary>
public sealed partial class OhgGalleryCellViewModel : ObservableObject
{
    public OhgGalleryCellViewModel(OhgGalleryCellRow row)
    {
        Key = row.Cell;
        Update(row);
    }

    /// <summary>The sync key: the cell number. Immutable for the row's lifetime.</summary>
    public int Key { get; }

    [ObservableProperty] private int cell;
    [ObservableProperty] private int slot;
    [ObservableProperty] private string? displayName;
    [ObservableProperty] private bool isBlank;

    public void Update(OhgGalleryCellRow row)
    {
        Cell = row.Cell;
        Slot = row.Slot;
        DisplayName = row.DisplayName;
        IsBlank = row.Slot == 0;
    }
}
