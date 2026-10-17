using System.Globalization;
using Windows.UI;

namespace CoreVideoPro.WinUI.Services;
// Declarative graphics use RGBA; legacy scene/style colours intentionally keep
// their six-digit HexColor contract.
public static class GraphicsColor
{
    public static Color Parse(string? text,Color fallback)
    {
        if(text is null || text.Length is not (7 or 9) || text[0]!='#' || !uint.TryParse(text.AsSpan(1),NumberStyles.HexNumber,CultureInfo.InvariantCulture,out var rgba)) return fallback;
        return text.Length==9 ? Color.FromArgb((byte)rgba,(byte)(rgba>>24),(byte)(rgba>>16),(byte)(rgba>>8)) : Color.FromArgb(255,(byte)(rgba>>16),(byte)(rgba>>8),(byte)rgba);
    }
}
