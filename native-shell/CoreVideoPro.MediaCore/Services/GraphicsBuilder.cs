using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>What an author can make in the Graphics tab's Design mode, in the operator's words.
/// One choice narrows the installed templates to a family and decides the playlist row kind the
/// result is saved as (titler spec: "Basic designer so authors can go beyond auto lower-thirds").</summary>
public sealed record GraphicsBuilderKind(string Id, string Label, string Family, string Blurb, string RowKind);

/// <summary>Pure decisions behind the template-first graphic builder (Design mode, #1076 slice 3).
/// No UI, no native calls: the builder control feeds it the workspace draft and the current playlist
/// and saves what comes back through <see cref="GraphicsWorkspaceLibrary.SavePlaylistAsync"/>.</summary>
public static class GraphicsBuilder
{
    public const string SponsorKindId = "sponsor";

    public static IReadOnlyList<GraphicsBuilderKind> Kinds { get; } = [
        new("lower-third", "Lower third", "lower-third", "A name and title over the picture. Can follow whoever is on Program.", GraphicsPlaylistRow.Guest),
        new("title", "Title", "title", "A heading for the top of the show, a segment or a thank-you.", GraphicsPlaylistRow.Title),
        new("bug", "Logo or bug", "bug", "Your logo in a corner, with an optional LIVE tag.", GraphicsPlaylistRow.Logo),
        new("clock", "Clock or countdown", "clock", "Time of day, a countdown to air, or elapsed time.", GraphicsPlaylistRow.LiveBug),
        new(SponsorKindId, "Sponsor card", "", "A full-frame image you already have (PNG or JPG).", GraphicsPlaylistRow.Sponsor),
    ];

    public static GraphicsBuilderKind? Kind(string id) => Kinds.FirstOrDefault(k => k.Id == id);

    /// <summary>The kind a saved row belongs to, so editing it reopens the right shelf.</summary>
    public static GraphicsBuilderKind KindOf(GraphicsPlaylistRow row, NativeGraphicsState? state)
    {
        if (row.Image is not null || (row.Kind == GraphicsPlaylistRow.Sponsor && row.TemplateId.Length == 0)) return Kinds[4];
        var family = state?.Templates.FirstOrDefault(t => t.TemplateId == row.TemplateId && t.TemplateVersion == row.TemplateVersion)?.Family
            ?? (row.TemplateId is "compact-solid" or "minimal-accent" or "broadcast" ? "lower-third" : "");
        return Kinds.FirstOrDefault(k => k.Family.Length > 0 && k.Family == family)
            ?? Kinds.FirstOrDefault(k => k.RowKind == row.Kind) ?? Kinds[0];
    }

    /// <summary>Installed templates for a kind, newest version of each id, name order. Sponsor
    /// cards are images, never templates.</summary>
    public static IReadOnlyList<NativeGraphicsTemplate> TemplatesFor(GraphicsBuilderKind kind, NativeGraphicsState? state)
    {
        if (state is null || kind.Family.Length == 0) return [];
        return state.Templates.Where(t => t.Family == kind.Family)
            .GroupBy(t => t.TemplateId, StringComparer.Ordinal).Select(g => g.OrderByDescending(t => t.TemplateVersion).First())
            .OrderBy(t => t.Name, StringComparer.CurrentCultureIgnoreCase).ToArray();
    }

    /// <summary>F1..F12 not taken by another row, lowest first; "" when all twelve are used.</summary>
    public static string NextFreeHotkey(IEnumerable<GraphicsPlaylistRow> rows, string? exceptRowId = null)
    {
        var taken = rows.Where(r => r.Id != exceptRowId).Select(r => r.Hotkey).ToHashSet(StringComparer.Ordinal);
        return Enumerable.Range(1, 12).Select(n => "F" + n).FirstOrDefault(k => !taken.Contains(k)) ?? "";
    }

    /// <summary>A playlist id from a display name: lower-case a-z0-9 and dashes, 1..48 chars,
    /// starting with a letter, unique among <paramref name="rows"/>.</summary>
    public static string NewRowId(string name, IEnumerable<GraphicsPlaylistRow> rows)
    {
        var chars = name.ToLowerInvariant().Select(c => c is (>= 'a' and <= 'z') or (>= '0' and <= '9') ? c : '-').ToArray();
        var slug = string.Join('-', new string(chars).Split('-', StringSplitOptions.RemoveEmptyEntries));
        slug = slug.TrimStart('-', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9');
        if (slug.Length == 0) slug = "graphic";
        if (slug.Length > 40) slug = slug[..40].TrimEnd('-');
        var used = rows.Select(r => r.Id).ToHashSet(StringComparer.Ordinal);
        if (!used.Contains(slug)) return slug;
        for (var n = 2; ; n++) { var candidate = $"{slug}-{n}"; if (!used.Contains(candidate)) return candidate; }
    }

    /// <summary>The row a finished draft saves as. <paramref name="existing"/> keeps its id (so
    /// shot layouts that name it survive) and its chosen image path.</summary>
    public static GraphicsPlaylistRow RowFrom(GraphicsBuilderKind kind, NativeGraphicsTemplate template, NativeGraphicsDraft draft,
        string name, string hotkey, int playOutAfterMs, bool followSources, IReadOnlyList<GraphicsPlaylistRow> rows, GraphicsPlaylistRow? existing = null,
        NativeGraphicsAnimations? motion = null)
    {
        var fields = draft.Fields.ValueKind == JsonValueKind.Object
            ? draft.Fields.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal)
            : new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        var trimmed = name.Trim(); if (trimmed.Length == 0) trimmed = template.Name; if (trimmed.Length > 128) trimmed = trimmed[..128];
        var rowKind = kind.Family == "lower-third" ? (followSources ? GraphicsPlaylistRow.Guest : GraphicsPlaylistRow.Title) : kind.RowKind;
        var hasClock = template.Family == "clock" || template.Fields.Any(f => f.Id == "time");
        return new GraphicsPlaylistRow {
            Id = existing?.Id ?? NewRowId(trimmed, rows),
            Kind = rowKind, Name = trimmed,
            Hotkey = GraphicsPlaylistRow.ValidHotkey(hotkey) ? hotkey : "",
            PlayOutAfterMs = GraphicsPlaylistRow.PlayOutAfterChoices.Contains(playOutAfterMs) ? playOutAfterMs : 0,
            TemplateId = template.TemplateId, TemplateVersion = template.TemplateVersion,
            Fields = fields, Style = draft.Style, Image = draft.Image, ImagePath = existing?.ImagePath,
            FollowSources = rowKind == GraphicsPlaylistRow.Guest, ShowClock = hasClock, Motion = motion,
        };
    }

    /// <summary>Rows with <paramref name="row"/> inserted or replaced by id; a new row goes before
    /// the sponsor placeholder (if any) so F-key order reads top to bottom.</summary>
    public static IReadOnlyList<GraphicsPlaylistRow> Upsert(IReadOnlyList<GraphicsPlaylistRow> rows, GraphicsPlaylistRow row)
    {
        var list = rows.ToList(); var index = list.FindIndex(r => r.Id == row.Id);
        if (index >= 0) { list[index] = row; return list; }
        var sponsor = list.FindIndex(r => r.Kind == GraphicsPlaylistRow.Sponsor && r.TemplateId.Length == 0 && r.Image is null);
        if (sponsor >= 0) list.Insert(sponsor, row); else list.Add(row);
        return list;
    }

    /// <summary>Operator-facing motion presets. "Default" leaves the row's Motion null so the
    /// playout default applies; slides come in from and leave toward the same edge.</summary>
    public static IReadOnlyList<(string Id, string Label)> MotionPresets { get; } = [
        ("default", "Fade (default)"), ("cut", "Cut"), ("slide-left", "Slide in from left"), ("slide-right", "Slide in from right"),
        ("slide-up", "Slide up from below"), ("slide-down", "Slide down from above")];
    public static IReadOnlyList<(string Label, int Ms)> MotionSpeeds { get; } = [("Quick", 150), ("Normal", 300), ("Slow", 600)];

    public static NativeGraphicsAnimations? MotionFor(string presetId, int durationMs)
    {
        var ms = Math.Clamp(durationMs, 0, 3000);
        switch (presetId) {
            case "cut": return new() { In = new() { Type = "none" }, Out = new() { Type = "none" } };
            case "slide-left": case "slide-right": case "slide-up": case "slide-down": {
                var direction = presetId[6..];
                return new() { In = new() { Type = "slide", Direction = direction, DurationMs = ms, Easing = "ease-out" },
                    Out = new() { Type = "slide", Direction = direction, DurationMs = Math.Max(0, ms * 2 / 3), Easing = "ease-in" } };
            }
            default: return null;
        }
    }

    /// <summary>The preset and speed a saved row's motion came from (round trip for editing).</summary>
    public static (string PresetId, int Ms) MotionPresetOf(NativeGraphicsAnimations? motion)
    {
        if (motion is null) return ("default", 300);
        if (motion.In.Type == "none") return ("cut", 300);
        if (motion.In.Type == "slide") return ("slide-" + motion.In.Direction, (int)Math.Round(motion.In.DurationMs));
        return ("default", (int)Math.Round(motion.In.DurationMs));
    }

    /// <summary>A row's saved fields as the object element the workspace draft expects.</summary>
    public static JsonElement FieldsElement(GraphicsPlaylistRow row) => JsonSerializer.SerializeToElement(row.Fields);
}
