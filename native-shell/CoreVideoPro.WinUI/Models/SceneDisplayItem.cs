using CommunityToolkit.Mvvm.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Models;

public sealed class SceneDisplayItem
{
    private static readonly SolidColorBrush DefaultBorder =
        new(Windows.UI.Color.FromArgb(255, 42, 52, 60));

    private static readonly SolidColorBrush PreviewBorder =
        new(Windows.UI.Color.FromArgb(255, 61, 220, 151));

    private static readonly SolidColorBrush ProgramBorder =
        new(Windows.UI.Color.FromArgb(255, 240, 168, 92));

    private static readonly SolidColorBrush DefaultBackground =
        new(Windows.UI.Color.FromArgb(255, 16, 22, 30));

    private static readonly SolidColorBrush PreviewBackground =
        new(Windows.UI.Color.FromArgb(255, 18, 36, 30));

    private static readonly SolidColorBrush ProgramBackground =
        new(Windows.UI.Color.FromArgb(255, 28, 24, 18));

    private static readonly SolidColorBrush SlotIdleBrush =
        new(Windows.UI.Color.FromArgb(255, 30, 40, 48));

    private static readonly SolidColorBrush SlotProgramBrush =
        new(Windows.UI.Color.FromArgb(255, 240, 168, 92));

    private static readonly SolidColorBrush SlotPreviewBrush =
        new(Windows.UI.Color.FromArgb(255, 61, 220, 151));

    public Scene Scene { get; init; } = new();

    public string Id { get; init; } = string.Empty;

    public int SlotNumber { get; init; }

    public IRelayCommand<string>? SelectCommand { get; init; }

    public IRelayCommand<string>? RemoveCommand { get; init; }

    // Scenes redesign S2: any scene can be duplicated into a new custom scene.
    public IRelayCommand<string>? DuplicateCommand { get; init; }

    public string Name => Scene.Name;

    public string LayoutLabel => string.IsNullOrWhiteSpace(Scene.Layout) ? "Custom" : Scene.Layout.Replace('-', ' ');

    public string Automation => Scene.Automation;

    public string DurationLabel => Scene.DurationLabel;

    public string MetaLabel => $"{LayoutLabel} · {DurationLabel}";

    public string StatusLabel =>
        IsOnProgram ? "PGM" : IsOnPreview ? "PVW" : "IDLE";

    public Brush StatusBackgroundBrush =>
        IsOnProgram
            ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 240, 168, 92))
            : IsOnPreview
                ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 61, 220, 151))
                : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 30, 40, 48));

    public Brush StatusForegroundBrush =>
        IsOnProgram || IsOnPreview
            ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 8, 14, 12))
            : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 184, 200, 192));

    public bool IsOnProgram { get; init; }

    public bool IsOnPreview { get; init; }

    public bool IsIdle => !IsOnProgram && !IsOnPreview;

    public bool CanRemove { get; init; }

    public Visibility RemoveButtonVisibility =>
        CanRemove ? Visibility.Visible : Visibility.Collapsed;

    public Brush ItemBorderBrush =>
        IsOnPreview ? PreviewBorder : IsOnProgram ? ProgramBorder : DefaultBorder;

    public Brush ItemBackgroundBrush =>
        IsOnPreview ? PreviewBackground : IsOnProgram ? ProgramBackground : DefaultBackground;

    public Brush SlotBadgeBackground =>
        IsOnProgram ? SlotProgramBrush : IsOnPreview ? SlotPreviewBrush : SlotIdleBrush;

    public Brush SlotBadgeForeground =>
        IsOnProgram || IsOnPreview
            ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 8, 14, 12))
            : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 184, 200, 192));

    public Visibility ProgramPillVisibility =>
        IsOnProgram ? Visibility.Visible : Visibility.Collapsed;

    public Visibility PreviewPillVisibility =>
        IsOnPreview ? Visibility.Visible : Visibility.Collapsed;

    public Visibility IdlePillVisibility =>
        IsIdle ? Visibility.Visible : Visibility.Collapsed;

    public Thickness ItemPadding => new Thickness(10, 8, 10, 8);
}
