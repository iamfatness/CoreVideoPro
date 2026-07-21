namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Pure resolution of the operator's ISO recording selection into the canonical
/// <c>isoSourceIds</c> list sent to the core (ISO-4, spec §7). Extracted from
/// <c>StudioViewModel</c> so the selection logic is unit-testable without the VM.
///
/// Rules:
/// <list type="bullet">
/// <item>When "Program + ISOs" is OFF, the result is EMPTY — no ISO writers arm, and
/// program recording is byte-identical to the program-only path.</item>
/// <item>Only sources that are currently eligible-and-present arm a writer (a selected
/// guest who left / a device that unplugged is dropped, not a dangling writer).</item>
/// <item>Order follows the eligible-present list (roster order) for determinism, deduped,
/// capped at <paramref name="max"/>.</item>
/// </list>
/// </summary>
public static class IsoSourceSelectionResolver
{
    /// <summary>Encoder ISO-writer cap (mirrors the shell's historical `.Take(8)`).</summary>
    public const int DefaultMaxIsoSources = 8;

    /// <summary>
    /// Resolve the canonical scheme-qualified ISO source ids (`zoom:&lt;pid&gt;` /
    /// `capture:&lt;id&gt;`) to arm.
    /// </summary>
    /// <param name="enabled">The "Program + ISOs" master switch.</param>
    /// <param name="selectedSourceIds">The operator's persisted ISO selection (a set).</param>
    /// <param name="eligiblePresentSourceIds">
    /// Currently eligible + present video-bearing source ids, in roster order.
    /// </param>
    /// <param name="max">Maximum ISO writers to arm.</param>
    public static IReadOnlyList<string> Resolve(
        bool enabled,
        IEnumerable<string>? selectedSourceIds,
        IEnumerable<string>? eligiblePresentSourceIds,
        int max = DefaultMaxIsoSources)
    {
        if (!enabled || max <= 0)
        {
            return [];
        }

        var selected = new HashSet<string>(
            selectedSourceIds?.Where(id => !string.IsNullOrWhiteSpace(id)) ?? [],
            StringComparer.Ordinal);
        if (selected.Count == 0)
        {
            return [];
        }

        var result = new List<string>();
        var emitted = new HashSet<string>(StringComparer.Ordinal);
        foreach (var id in eligiblePresentSourceIds ?? [])
        {
            if (string.IsNullOrWhiteSpace(id) || !selected.Contains(id) || !emitted.Add(id))
            {
                continue;
            }

            result.Add(id);
            if (result.Count >= max)
            {
                break;
            }
        }

        return result;
    }

    /// <summary>
    /// Project a scheme-qualified source id list to the legacy bare-participant-id list
    /// (`zoom:&lt;pid&gt;` → `&lt;pid&gt;`) for back-compat with readers that predate
    /// `isoSourceIds`. Capture ids have no bare form, so they are dropped from the legacy
    /// projection (the core prefers `isoSourceIds` whenever present).
    /// </summary>
    public static IReadOnlyList<string> ToLegacyParticipantIds(IEnumerable<string>? sourceIds)
    {
        const string zoomPrefix = "zoom:";
        var result = new List<string>();
        foreach (var id in sourceIds ?? [])
        {
            if (string.IsNullOrWhiteSpace(id))
            {
                continue;
            }

            if (id.StartsWith(zoomPrefix, StringComparison.Ordinal))
            {
                result.Add(id[zoomPrefix.Length..]);
            }
        }

        return result;
    }
}
