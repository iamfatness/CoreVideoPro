namespace CoreVideoPro.WinUI.Services;

public enum SecondaryLineMode { SourceDefault, Custom, Hidden }

public static class SourceLowerThirdText
{
    public static SecondaryLineMode Mode(bool usesDefault, string text) => usesDefault
        ? SecondaryLineMode.SourceDefault : string.IsNullOrWhiteSpace(text) ? SecondaryLineMode.Hidden : SecondaryLineMode.Custom;

    public static string? Value(SecondaryLineMode mode, string custom) => mode switch
    {
        SecondaryLineMode.SourceDefault => null,
        SecondaryLineMode.Hidden => "",
        _ => custom.Trim()
    };

    // Missing = source default; an explicitly saved empty string = hide the
    // entire secondary line, including any legacy organization/role fallback.
    public static (string Title, string Org) Resolve(IReadOnlyDictionary<string, string> overrides,
        string? sourceId, string title, string org) =>
        sourceId is not null && overrides.TryGetValue(sourceId, out var text) ? (text, "") : (title, org);

    public static string Preview(string title, string org) =>
        string.IsNullOrWhiteSpace(org) ? title : string.IsNullOrWhiteSpace(title) ? org : $"{title} | {org}";

    public static bool Apply(Dictionary<string, string> names, Dictionary<string, string> secondary,
        string sourceId, string? name, string? secondaryLine)
    {
        var changed = Set(names, sourceId, string.IsNullOrWhiteSpace(name) ? null : name.Trim());
        return Set(secondary, sourceId, secondaryLine?.Trim()) || changed;
    }

    private static bool Set(Dictionary<string, string> values, string id, string? value)
    {
        if (value is null) return values.Remove(id);
        if (values.TryGetValue(id, out var current) && current == value) return false;
        values[id] = value; return true;
    }
}
