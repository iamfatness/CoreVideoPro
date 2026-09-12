using Windows.UI;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// #476 / T3.5. Hex text &lt;-&gt; <see cref="Color"/>, in ONE place.
///
/// Owner, 2026-09-11: "the Glow Color and Border color need a color picker.
/// Whose gonna know which hex code to use, we can have an option to enter the
/// hex code but most of our people are going to be color pickers."
///
/// These rules were private statics on OverlaysViewModel, which is why the
/// brand colours had a picker and the Tiles colours did not. Extracted rather
/// than copied: two implementations of "what does #GGG mean" is how one screen
/// silently accepts what another rejects.
/// </summary>
public static class HexColor
{
    /// <summary>Parses #RRGGBB (with or without the #). Alpha is always opaque:
    /// none of these fields has ever carried transparency, and silently
    /// accepting an 8-digit value would set one the compositor ignores.</summary>
    public static bool TryParse(string? value, out Color color)
    {
        color = default;
        if (string.IsNullOrWhiteSpace(value))
        {
            return false;
        }

        var hex = value.Trim();
        if (hex.StartsWith('#'))
        {
            hex = hex[1..];
        }

        if (hex.Length != 6 ||
            !byte.TryParse(hex[..2], System.Globalization.NumberStyles.HexNumber, null, out var r) ||
            !byte.TryParse(hex.Substring(2, 2), System.Globalization.NumberStyles.HexNumber, null, out var g) ||
            !byte.TryParse(hex.Substring(4, 2), System.Globalization.NumberStyles.HexNumber, null, out var b))
        {
            return false;
        }

        color = Color.FromArgb(255, r, g, b);
        return true;
    }

    public static Color ParseOrDefault(string? value, Color fallback) =>
        TryParse(value, out var color) ? color : fallback;

    /// <summary>Always 6 digits, uppercase, #-prefixed - the shape the scene
    /// payload and every placeholder in the UI already use.</summary>
    public static string ToHex(Color color) => $"#{color.R:X2}{color.G:X2}{color.B:X2}";
}
