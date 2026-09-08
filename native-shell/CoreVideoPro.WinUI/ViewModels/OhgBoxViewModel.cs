using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>One look box (Plan 7b Task 5), mirroring <see cref="OhgBoxView"/>. Present only while
/// <c>Current.Look</c> is non-null (empty otherwise) — diff-updated in place via
/// <see cref="ObservableCollectionSync"/>, the same technique every other keyed collection on this
/// VM uses (never <c>Clear()</c>+<c>Add</c> — see CLAUDE.md's CoreMessagingXP 0xc000027b class).</summary>
public sealed partial class OhgBoxViewModel : ObservableObject
{
    public OhgBoxViewModel(OhgBoxView row)
    {
        Key = row.Box;
        Update(row);
    }

    /// <summary>The sync key: the box number. Immutable for the row's lifetime.</summary>
    public int Key { get; }

    [ObservableProperty] private int box;
    [ObservableProperty] private int? slot;
    [ObservableProperty] private string? displayName;
    [ObservableProperty] private bool isSelected;

    public void Update(OhgBoxView row)
    {
        Box = row.Box;
        Slot = row.Slot;
        DisplayName = row.DisplayName;
    }
}
