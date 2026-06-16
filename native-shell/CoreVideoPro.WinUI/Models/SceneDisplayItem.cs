using CommunityToolkit.Mvvm.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Models;

public sealed class SceneDisplayItem
{
    private static readonly SolidColorBrush DefaultBorder =
        new(Windows.UI.Color.FromArgb(26, 189, 207, 196));

    private static readonly SolidColorBrush PreviewBorder =
        new(Windows.UI.Color.FromArgb(204, 68, 193, 161));

    private static readonly SolidColorBrush PreviewBackground =
        new(Windows.UI.Color.FromArgb(31, 68, 193, 161));

    private static readonly SolidColorBrush DefaultBackground =
        new(Windows.UI.Color.FromArgb(138, 10, 13, 12));
    public Scene Scene { get; init; } = new();

    public string Id { get; init; } = string.Empty;

    public IRelayCommand<string>? SelectCommand { get; init; }

    public string Name => Scene.Name;

    public string Automation => Scene.Automation;

    public string DurationLabel => Scene.DurationLabel;

    public bool IsOnProgram { get; init; }

    public bool IsOnPreview { get; init; }

    public string StatusBadge =>
        IsOnProgram ? "Program" : IsOnPreview ? "Preview" : string.Empty;

    public bool HasStatusBadge => StatusBadge.Length > 0;

    public bool IsSelected => IsOnPreview;

    public Brush ItemBorderBrush => IsOnPreview ? PreviewBorder : DefaultBorder;

    public Brush ItemBackgroundBrush => IsOnPreview ? PreviewBackground : DefaultBackground;

    public Visibility ProgramAccentVisibility =>
        IsOnProgram ? Visibility.Visible : Visibility.Collapsed;

    public Visibility PreviewOutlineVisibility =>
        IsOnPreview ? Visibility.Visible : Visibility.Collapsed;

    public Visibility StatusBadgeVisibility =>
        HasStatusBadge ? Visibility.Visible : Visibility.Collapsed;
}