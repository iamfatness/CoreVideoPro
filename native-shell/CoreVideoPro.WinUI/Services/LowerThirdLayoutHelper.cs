using Microsoft.UI.Xaml;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Lower-third anchor vocabulary and its XAML alignment. (Was CaptionStyleHelper; the
/// caption font/colour/opacity/position members went with the caption style controls in #767 -
/// they only ever produced a summary string the core stored and never rendered.)</summary>
public static class LowerThirdLayoutHelper
{
    public static IReadOnlyList<string> LowerThirdPositionOptions { get; } = ["lower-left", "lower-right", "upper-left", "upper-right"];

    public static (HorizontalAlignment Horizontal, VerticalAlignment Vertical) LowerThirdAlignment(string position) =>
        (position.EndsWith("right",StringComparison.Ordinal) ? HorizontalAlignment.Right : HorizontalAlignment.Left,
         position.StartsWith("upper",StringComparison.Ordinal) ? VerticalAlignment.Top : VerticalAlignment.Bottom);
}
