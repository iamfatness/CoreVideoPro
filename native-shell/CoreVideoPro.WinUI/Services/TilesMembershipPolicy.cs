using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class TilesMembershipPolicy
{
    public static IReadOnlyList<string> Resolve(DynamicGallerySettings settings, IReadOnlyList<string> eligible)
    {
        var excluded = new HashSet<string>(settings.ExcludedSourceIds ?? [], StringComparer.Ordinal);
        var available = eligible.Where(id => !string.IsNullOrWhiteSpace(id) && !excluded.Contains(id)).Distinct(StringComparer.Ordinal).ToList();
        var present = new HashSet<string>(available, StringComparer.Ordinal);
        var slots = new string?[Math.Clamp(settings.MaxTiles, 1, 64)];
        var used = new HashSet<string>(StringComparer.Ordinal);
        var manual = settings.ManualSlots ?? [];
        if (!settings.AutoFill)
        {
            // Keep slot positions even when a source is absent. Native manual
            // layout reserves these cells and only draws fresh admitted sources.
            return manual.Take(slots.Length).Select(id =>
                !string.IsNullOrEmpty(id) && !excluded.Contains(id) && used.Add(id) ? id : string.Empty).ToList();
        }
        for (var index = 0; index < Math.Min(slots.Length, manual.Count); index++)
            if (manual[index] is { } id && present.Contains(id) && used.Add(id)) slots[index] = id;
        if (settings.AutoFill)
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
