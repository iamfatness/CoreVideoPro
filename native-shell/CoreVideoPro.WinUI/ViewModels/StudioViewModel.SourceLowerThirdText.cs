using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private readonly Dictionary<string, string> _sourceSecondaryLines = new(StringComparer.Ordinal);
    public bool UsesDefaultSourceSecondaryLine(string? sourceId) => sourceId is null || !_sourceSecondaryLines.ContainsKey(sourceId);
    public string ResolveSourceSecondaryLine(string? sourceId, string derived) =>
        sourceId is not null && _sourceSecondaryLines.TryGetValue(sourceId, out var text) ? text : derived;

    public void ApplySourceLowerThirdText(string? sourceId, string? name, string? secondaryLine)
    {
        if (string.IsNullOrWhiteSpace(sourceId) ||
            !SourceLowerThirdText.Apply(_sourceDisplayNames, _sourceSecondaryLines, sourceId, name, secondaryLine)) return;
        SaveProductionOutputPreferences();
        ScheduleShowInputRefresh();
        RefreshProgramLowerThirdKeyPosition();
    }

    private LowerThirdSource SourceWithOperatorText(string sourceId, string name, string title, string org, bool active, bool screen)
    {
        var text = SourceLowerThirdText.Resolve(_sourceSecondaryLines, sourceId, title, org);
        return new(sourceId, ResolveSourceDisplayName(sourceId, name), text.Title, text.Org, active, screen);
    }

    private void RestoreSourceSecondaryLines(IReadOnlyDictionary<string, string>? values)
    {
        _sourceSecondaryLines.Clear();
        if (values is null) return;
        foreach (var pair in values)
            if (!string.IsNullOrWhiteSpace(pair.Key) && pair.Value is not null)
                _sourceSecondaryLines[pair.Key] = pair.Value.Trim();
    }
}
