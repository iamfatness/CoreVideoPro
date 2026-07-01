using System;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// Pure (UI-thread-free, XAML-free) formatting rules for the multiview overlay: label text,
/// tally state, and meter eligibility per tile. Kept separate from the control so it can be
/// unit tested without a WinUI dispatcher.
/// </summary>
public static class MultiviewOverlayFormatting
{
    public const string TallyProgram = "pgm";
    public const string TallyPreview = "pvw";
    public const string TallyNone = "none";

    /// <summary>
    /// The label to draw in the tile's bottom bar. Program/preview role tiles get canonical
    /// "PROGRAM"/"PREVIEW" captions regardless of the per-source label; everything else uses the
    /// core-supplied label.
    /// </summary>
    public static string ResolveLabel(MultiviewTile tile)
    {
        if (tile is null)
        {
            return string.Empty;
        }

        if (IsRole(tile.Role, "pgm"))
        {
            return "PROGRAM";
        }

        if (IsRole(tile.Role, "pvw"))
        {
            return "PREVIEW";
        }

        return tile.Label ?? string.Empty;
    }

    /// <summary>
    /// Effective tally for the tile: a "pgm"/"pvw" ROLE forces the matching tally colour
    /// (red/green) regardless of the per-source tally; otherwise the per-source tally wins,
    /// normalised to one of "pgm" | "pvw" | "none".
    /// </summary>
    public static string ResolveTally(MultiviewTile tile)
    {
        if (tile is null)
        {
            return TallyNone;
        }

        if (IsRole(tile.Role, "pgm"))
        {
            return TallyProgram;
        }

        if (IsRole(tile.Role, "pvw"))
        {
            return TallyPreview;
        }

        return NormalizeTally(tile.Tally);
    }

    /// <summary>
    /// True when this tile should carry an audio meter. Program tiles always qualify (they show
    /// the master PGM meter). TODO: once the core exports per-source levels, source tiles can show
    /// their own meter — for now only the program tile does.
    /// </summary>
    public static bool ShouldShowMeter(MultiviewTile tile) =>
        tile is not null && (IsRole(tile.Role, "pgm") || IsProgramTally(tile.Tally));

    public static string FormatClock(DateTime time) => time.ToString("HH:mm:ss");

    private static bool IsProgramTally(string? tally) =>
        string.Equals(NormalizeTally(tally), TallyProgram, StringComparison.Ordinal);

    private static string NormalizeTally(string? tally)
    {
        if (string.IsNullOrWhiteSpace(tally))
        {
            return TallyNone;
        }

        return tally.Trim().ToLowerInvariant() switch
        {
            "pgm" or "program" => TallyProgram,
            "pvw" or "preview" => TallyPreview,
            _ => TallyNone
        };
    }

    private static bool IsRole(string? role, string expected) =>
        !string.IsNullOrWhiteSpace(role) &&
        string.Equals(role.Trim(), expected, StringComparison.OrdinalIgnoreCase);
}
