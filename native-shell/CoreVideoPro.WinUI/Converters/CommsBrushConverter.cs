using CoreVideoPro.WinUI.ViewModels.Comms;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Converters;

/// <summary>
/// Comms page colours, resolved from Pro's own theme tokens (App.xaml / StudioTheme.xaml);
/// no raw colours. On air follows Pro's existing semantics: StudioAirBrush is "air red -
/// on-air, record", so a key that is keyed with a confirmed listener is filled air red
/// (text in StudioBackgroundBrush, ~4.9:1). Keyed-but-nobody-there is the warning amber
/// outline; ready is primary text; idle is secondary text; unreachable is dim.
/// The value is a <see cref="CommsTone"/> or a bool (lamps, toggles); the parameter names
/// the part being painted.
/// </summary>
public sealed class CommsBrushConverter : IValueConverter
{
    public object? Convert(object value, Type targetType, object parameter, string language)
    {
        var part = parameter as string ?? "";
        var key = value switch
        {
            CommsTone tone => ForTone(tone, part),
            bool on => ForBool(on, part),
            _ => "StudioDimBrush",
        };
        return Resolve(key);
    }

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();

    /// <summary>The token key for a tone and part (pure; unit-tested).</summary>
    public static string ForTone(CommsTone tone, string part) => part switch
    {
        "cell-bg" => tone switch
        {
            CommsTone.Hot => "StudioAirBrush",
            CommsTone.Off => "StudioFieldBrush",
            _ => "StudioSurfaceBrush",
        },
        "cell-border" => tone switch
        {
            CommsTone.Hot => "StudioAirBrush",
            CommsTone.Armed => "StudioWarningBrush",
            CommsTone.Off => "StudioLine2Brush",
            _ => "StudioBorderBrush",
        },
        "cell-fg" => tone switch
        {
            CommsTone.Hot => "StudioBackgroundBrush",
            CommsTone.Armed => "StudioWarningBrush",
            CommsTone.Ready => "StudioTextBrush",
            CommsTone.Off => "StudioDimBrush",
            _ => "StudioTextSecondaryBrush",
        },
        "meter" => tone switch
        {
            CommsTone.Hot => "StudioAirBrush",
            CommsTone.Armed => "StudioWarningBrush",
            _ => "StudioLiveBrush",
        },
        _ => "StudioDimBrush",
    };

    public static string ForBool(bool on, string part) => part switch
    {
        "lamp" => on ? "StudioLiveBrush" : "StudioDimBrush",
        "lamp-text" => on ? "StudioTextBrush" : "StudioDimBrush",
        "toggle-border" => on ? "StudioAccentBrush" : "StudioBorderBrush",
        "toggle-bg" => on ? "StudioSurfaceRaisedBrush" : "StudioSurfaceBrush",
        "chip-bg" => on ? "StudioAccentBrush" : "StudioSurfaceBrush",
        "chip-fg" => on ? "StudioBackgroundBrush" : "StudioTextBrush",
        "busy-border" => on ? "StudioWarningBrush" : "StudioBorderBrush",
        "warn-text" => on ? "StudioWarningBrush" : "StudioTextBrush",
        "meter-lit" => on ? "StudioTextBrush" : "StudioDimBrush",
        _ => "StudioDimBrush",
    };

    private static Brush? Resolve(string key) =>
        Application.Current?.Resources.TryGetValue(key, out var brush) == true ? brush as Brush : null;
}
