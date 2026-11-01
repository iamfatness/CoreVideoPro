using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>A Tiles member candidate. <paramref name="Group"/> is the source-picker group
/// ("Zoom" for a participant; "Camera"/"Screen"/... for a local source, #1071).
/// <see cref="Label"/> is what the member list shows: the bare name for a Zoom guest, and
/// "Group · Name" for a local source, mirroring the show-input picker button.</summary>
public sealed record GalleryMemberChoice(string Id, string Name, string Group = "Zoom")
{
    public string Label => Group == "Zoom" ? Name : $"{Group} · {Name}";
}

/// <summary>
/// #1071: the Tiles member picker's list — Zoom participants (roster order) and then the
/// in-show local sources grouped the way the show-input picker groups them
/// (<see cref="ShowInputRosterService.UnifiedSourceGroups"/> order, slot order within a
/// group). Extracted from <c>StudioViewModel.GalleryMemberChoices</c> so the order and the
/// labels are testable without the god object. Every Tiles setter that takes a source id
/// (<c>AssignTilesSlot</c>, <c>SetTilesExclusion</c>, <c>SetTilesBackground</c>,
/// <c>SetTilesOverride</c>) validates against this list, and
/// <see cref="TilesLayerPayloadBuilder"/>'s eligible set is drawn from the same local list,
/// so what the operator can pick is what auto fill can seat.
/// </summary>
public static class TilesMemberChoicePolicy
{
    public static IReadOnlyList<GalleryMemberChoice> Build(
        IReadOnlyList<Participant> roomVideoParticipants,
        IReadOnlyList<TilesLocalSource> localSources)
    {
        var zoom = roomVideoParticipants
            .Select(p => new GalleryMemberChoice(p.Id.Contains(':') ? p.Id : ShowInputRosterService.ZoomSourceId(p.Id), p.Name));
        var groupOrder = ShowInputRosterService.UnifiedSourceGroups
            .Select((group, index) => (group, index))
            .ToDictionary(pair => pair.group, pair => pair.index, StringComparer.Ordinal);
        var local = localSources
            .OrderBy(s => groupOrder.GetValueOrDefault(s.Group, int.MaxValue))  // stable: slot order within a group
            .Select(s => new GalleryMemberChoice(s.Id, s.Name, s.Group));
        return zoom.Concat(local).DistinctBy(c => c.Id, StringComparer.Ordinal).ToList();
    }

    /// <summary>True when <paramref name="sourceId"/> is one of the current choices — the
    /// validation every Tiles setter applies before writing a source id into the scene.</summary>
    public static bool Contains(IReadOnlyList<GalleryMemberChoice> choices, string? sourceId) =>
        sourceId is { Length: > 0 } && choices.Any(c => string.Equals(c.Id, sourceId, StringComparison.Ordinal));
}
