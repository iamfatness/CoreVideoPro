using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class TilesMembershipPolicy
{
    public static string NormalizeMode(string? mode) => mode?.Trim().ToLowerInvariant() switch
    {
        "manual" => "manual",
        "routed" => "routed",
        _ => "eligible"
    };

    public static IReadOnlyList<string> Resolve(
        DynamicGallerySettings settings,
        IReadOnlyList<string> eligible,
        IReadOnlyCollection<string>? routed = null,
        IReadOnlyList<TilesIdentityPolicy.Person>? roster = null,
        string? currentMeetingId = null)
    {
        var mode = NormalizeMode(settings.MembershipMode);
        var excluded = new HashSet<string>(StringComparer.Ordinal);
        foreach (var key in settings.ExcludedSourceIds ?? [])
        {
            var live = roster is null
                ? key
                : TilesIdentityPolicy.ResolveLiveSourceId(key, roster, currentMeetingId, settings.BoundMeetingId);
            if (!string.IsNullOrEmpty(live))
            {
                excluded.Add(live);
            }
        }

        var available = eligible.Where(id => !string.IsNullOrWhiteSpace(id) && !excluded.Contains(id)).Distinct(StringComparer.Ordinal).ToList();
        if (mode == "routed")
        {
            var routedSet = new HashSet<string>(routed ?? [], StringComparer.Ordinal);
            available = available.Where(routedSet.Contains).ToList();
        }
        var present = new HashSet<string>(available, StringComparer.Ordinal);
        var slots = new string?[Math.Clamp(settings.MaxTiles, 1, 64)];
        var used = new HashSet<string>(StringComparer.Ordinal);
        var manual = (settings.ManualSlots ?? []).Select(id =>
            roster is null
                ? id
                : TilesIdentityPolicy.ResolveLiveSourceId(id, roster, currentMeetingId, settings.BoundMeetingId)).ToList();
        if (mode == "manual")
        {
            // Keep slot positions even when a source is absent. Native manual
            // layout reserves these cells and only draws fresh admitted sources.
            return manual.Take(slots.Length).Select(id =>
                !string.IsNullOrEmpty(id) && !excluded.Contains(id) && used.Add(id) ? id : string.Empty).ToList();
        }
        for (var index = 0; index < Math.Min(slots.Length, manual.Count); index++)
            if (manual[index] is { } id && present.Contains(id) && used.Add(id)) slots[index] = id;
        if (mode != "manual")
        {
            var next = 0;
            foreach (var id in available)
            {
                if (!used.Add(id)) continue;
                while (next < slots.Length && slots[next] is not null) next++;
                if (next == slots.Length) break;
                slots[next] = id;
            }
        }
        // Native draws only currently eligible members; missing manual members
        // remain in saved intent and can return, without substituting in manual mode.
        return slots.Where(id => id is not null).Select(id => id!).ToList();
    }
}
