using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Models;

public sealed class TabChrome
{
    public required Brush Background { get; init; }
    public required Brush BorderBrush { get; init; }
    public required Brush Foreground { get; init; }
}