using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.ViewModels.ShowInputs;

/// <summary>Maps a core-composited tile rectangle to XAML framing of the same GPU surface.</summary>
public static class MultiviewTileCropPolicy
{
    public static bool SameTiles(IReadOnlyList<MultiviewTile>? a, IReadOnlyList<MultiviewTile> b)
    {
        if (a is null || a.Count != b.Count) return false;
        for (var i = 0; i < a.Count; i++)
        {
            var left = a[i];
            var right = b[i];
            if (left.Role != right.Role || left.Slot != right.Slot || left.SourceId != right.SourceId ||
                left.Tally != right.Tally || left.ActiveSpeaker != right.ActiveSpeaker ||
                left.X != right.X || left.Y != right.Y || left.W != right.W || left.H != right.H)
                return false;
        }
        return true;
    }

    public static Windows.Foundation.Rect? Resolve(MultiviewTile? tile)
    {
        if (tile is null || tile.W <= 0 || tile.H <= 0 || tile.W > 1 || tile.H > 1 ||
            tile.X < 0 || tile.Y < 0 || tile.X + tile.W > 1.001 || tile.Y + tile.H > 1.001)
            return null;
        return new Windows.Foundation.Rect(tile.X, tile.Y, tile.W, tile.H);
    }

    public static string Tally(MultiviewTile? tile, bool stalled) =>
        stalled ? "hold" : tile?.Tally switch
        {
            "pgm" => "pgm",
            "pvw" => "pvw",
            _ => tile?.ActiveSpeaker == true ? "talking" : "none"
        };
}
