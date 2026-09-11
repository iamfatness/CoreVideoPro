using System;
using System.Collections.Generic;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;

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
            return WithBusNotice("PROGRAM", tile.Label);
        }

        if (IsRole(tile.Role, "pvw"))
        {
            return WithBusNotice("PREVIEW", tile.Label);
        }

        return tile.Label ?? string.Empty;
    }

    /// <summary>
    /// #478 N4: the core appends the bus's subscription-limit notice to the PGM/PVW cell label
    /// as "Program · &lt;notice&gt;" / "Preview · &lt;notice&gt;". Keep the canonical caption and
    /// carry the notice after it, so a cued guest the video budget left out is named on the
    /// cell instead of the cell silently showing a placeholder.
    /// </summary>
    private static string WithBusNotice(string caption, string? label)
    {
        const string separator = " \u00b7 ";
        if (label is { Length: > 0 } && label.IndexOf(separator, StringComparison.Ordinal) is var index and >= 0)
        {
            return caption + separator + label[(index + separator.Length)..];
        }

        return caption;
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

    /// <summary>
    /// The tiles that get a click target and decorations: every PGM/PVW cell plus up to
    /// <see cref="ShowInputRosterService.MaxShowInputs"/> source cells. The cap applies to
    /// SOURCES only — the core's list leads with PGM and PVW, so a flat Take(10) used to drop
    /// sources 9 and 10 (not cueable, no label/tally).
    /// </summary>
    public static IReadOnlyList<MultiviewTile> SelectOverlayTiles(IEnumerable<MultiviewTile> tiles)
    {
        var selected = new List<MultiviewTile>();
        var sources = 0;
        foreach (var tile in tiles)
        {
            if (IsRole(tile.Role, "source"))
            {
                if (sources >= ShowInputRosterService.MaxShowInputs)
                {
                    continue;
                }

                sources++;
            }

            selected.Add(tile);
        }

        return selected;
    }

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
