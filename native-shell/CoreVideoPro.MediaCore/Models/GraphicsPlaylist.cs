using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>One operator-facing playlist row (issue #1076). Configuration only:
/// no instance ids, revisions, cue/program state or source generations are persisted.
/// Guest rows are expanded into one <see cref="GraphicsPlaylistEntry"/> per bound
/// roster slot at runtime.</summary>
public sealed record GraphicsPlaylistRow
{
    public const string Guest = "guest", Title = "title", Logo = "logo", LiveBug = "liveBug", Sponsor = "sponsor";
    public static readonly IReadOnlyList<string> Kinds = [Guest, Title, Logo, LiveBug, Sponsor];
    public static readonly IReadOnlyList<int> PlayOutAfterChoices = [0, 8000, 15000];
    private static readonly Regex HotkeyPattern = new("^F([1-9]|1[0-2])$", RegexOptions.CultureInvariant);

    public string Id { get; init; } = "";
    public string Kind { get; init; } = Title;
    public string Name { get; init; } = "";
    /// <summary>"" or F1..F12.</summary>
    public string Hotkey { get; init; } = "";
    /// <summary>0 = stays on until Play Out; otherwise auto Play Out after this many ms.</summary>
    public int PlayOutAfterMs { get; init; }
    /// <summary>Exact template pin. Empty for a placeholder row (sponsor card before an image is chosen).</summary>
    public string TemplateId { get; init; } = "";
    public int TemplateVersion { get; init; }
    public IReadOnlyDictionary<string, JsonElement> Fields { get; init; } = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
    public LowerThirdAppearance Style { get; init; } = new();
    /// <summary>Operator-chosen local file for an image row; the verified package is <see cref="Image"/>.</summary>
    public string? ImagePath { get; init; }
    public NativeGraphicsImageReference? Image { get; init; }
    /// <summary>Guest rows: name/title come from the Sources page text of the entry's source.</summary>
    public bool FollowSources { get; init; }
    public bool ShowClock { get; init; }
    /// <summary>In/out motion sent with the instance (titler spec Phase 1 "in/out transitions").
    /// Null = the playout default (fade 250/200 ms). Values follow the core's whitelist.</summary>
    public NativeGraphicsAnimations? Motion { get; init; }

    public static bool ValidTransition(NativeGraphicsTransition t) =>
        t.Type is "none" or "fade" or "slide" && t.Direction is "left" or "right" or "up" or "down" &&
        t.Easing is "linear" or "ease-in" or "ease-out" or "ease-in-out" && double.IsFinite(t.DurationMs) && t.DurationMs is >= 0 and <= 3000;
    public static bool ValidMotion(NativeGraphicsAnimations? motion) => motion is null || (ValidTransition(motion.In) && ValidTransition(motion.Out));

    public static bool ValidId(string value) => value.Length is > 0 and <= 48 && value[0] is >= 'a' and <= 'z' &&
        value.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '-');
    public static bool ValidHotkey(string value) => value.Length == 0 || HotkeyPattern.IsMatch(value);

    [JsonIgnore] public bool IsValid => ValidId(Id) && Kinds.Contains(Kind, StringComparer.Ordinal) && Name is { Length: > 0 and <= 128 } &&
        Hotkey is not null && ValidHotkey(Hotkey) && PlayOutAfterChoices.Contains(PlayOutAfterMs) &&
        TemplateId is not null && TemplateId.Length <= 64 && TemplateVersion >= 0 && (TemplateId.Length == 0) == (TemplateVersion == 0) &&
        Fields is not null && Fields.Count <= 32 && Fields.Keys.All(k => k.Length is > 0 and <= 64) && Style is { IsValid: true } &&
        (ImagePath is null || (ImagePath.Length <= 2048 && !ImagePath.Contains('\0'))) && ValidMotion(Motion) &&
        (Image is null || (Image.TemplateId.Length is > 0 and <= 64 && Image.TemplateVersion >= 1 && Image.AssetId.Length is > 0 and <= 64));
}

/// <summary>A playable item: a single row (SourceId "") or one guest of a guest row.
/// <see cref="Bindings"/> maps template variable name -> value resolved at projection
/// time (today "name" and "title" from the Sources page; later Zoom participant
/// metadata, clocks and sheets slot in under new variable names). Never persisted.</summary>
public sealed record GraphicsPlaylistEntry
{
    public const string NameVariable = "name", TitleVariable = "title";
    private static readonly IReadOnlyDictionary<string, string> None = new Dictionary<string, string>(StringComparer.Ordinal);
    public GraphicsPlaylistEntry() { }
    public GraphicsPlaylistEntry(string rowId, string sourceId, IReadOnlyDictionary<string, string> bindings) => (RowId, SourceId, Bindings) = (rowId, sourceId, bindings);
    public GraphicsPlaylistEntry(string rowId, string sourceId, string label, string title)
        : this(rowId, sourceId, new Dictionary<string, string>(StringComparer.Ordinal) { [NameVariable] = label, [TitleVariable] = title }) { }
    public string RowId { get; init; } = "";
    public string SourceId { get; init; } = "";
    public IReadOnlyDictionary<string, string> Bindings { get; init; } = None;
    [JsonIgnore] public string Label => Bindings.GetValueOrDefault(NameVariable, "");
    [JsonIgnore] public string Title => Bindings.GetValueOrDefault(TitleVariable, "");
    public bool Equals(GraphicsPlaylistEntry? other) => other is not null && RowId == other.RowId && SourceId == other.SourceId &&
        Bindings.Count == other.Bindings.Count && Bindings.All(p => other.Bindings.TryGetValue(p.Key, out var v) && v == p.Value);
    public override int GetHashCode() => HashCode.Combine(RowId, SourceId, Bindings.Count);
}

/// <summary>A shot layout (issue #1076 slice 2): one button that cues a scene to Preview and
/// stages playlist rows with it, so a single Take carries the picture and the graphics.
/// Configuration only; nothing about air state is persisted. <see cref="SceneId"/> "" = no
/// scene change; <see cref="RowIds"/> may be empty; <see cref="PlayOutOthers"/> plays out
/// every Program graphic the layout does not carry.</summary>
public sealed record ShotLayout
{
    public const string HotkeyPrefix = "Shift+F";
    private static readonly Regex HotkeyPattern = new(@"^Shift\+F([1-9]|1[01])$", RegexOptions.CultureInvariant);

    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public string SceneId { get; init; } = "";
    public IReadOnlyList<string> RowIds { get; init; } = [];
    /// <summary>"" or Shift+F1..Shift+F11 (Shift+F12 is Play Out all).</summary>
    public string Hotkey { get; init; } = "";
    public bool PlayOutOthers { get; init; }

    public static bool ValidHotkey(string value) => value.Length == 0 || HotkeyPattern.IsMatch(value);

    [JsonIgnore] public bool IsValid => GraphicsPlaylistRow.ValidId(Id) && Name is { Length: > 0 and <= 128 } &&
        SceneId is not null && SceneId.Length <= 64 && Hotkey is not null && ValidHotkey(Hotkey) &&
        RowIds is not null && RowIds.Count <= 16 && RowIds.All(GraphicsPlaylistRow.ValidId) &&
        RowIds.Distinct(StringComparer.Ordinal).Count() == RowIds.Count;
}
