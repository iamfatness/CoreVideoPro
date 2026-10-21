using System.Text.Json;

namespace CoreVideoPro.Control;

/// <summary>Parse and refuse-closed checks for <c>scene.cueWithRoutes</c>. Pure: no ViewModel,
/// no mutation. The WinUI surface applies the routes only after this returns null.</summary>
public static class SceneCueWithRoutesArgs
{
    public static bool TryParseRoutes(string? raw, out IReadOnlyDictionary<string, int?> routes, out string? error)
    {
        routes = new Dictionary<string, int?>(StringComparer.Ordinal);
        if (string.IsNullOrWhiteSpace(raw))
        {
            error = "routes must be a JSON object of routeId to Show Input slot (or null).";
            return false;
        }

        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(raw);
        }
        catch (JsonException ex)
        {
            error = $"routes is not JSON: {ex.Message}";
            return false;
        }

        using (doc)
        {
            if (doc.RootElement.ValueKind != JsonValueKind.Object)
            {
                error = "routes must be a JSON object of routeId to Show Input slot (or null).";
                return false;
            }

            var parsed = new Dictionary<string, int?>(StringComparer.Ordinal);
            foreach (var property in doc.RootElement.EnumerateObject())
            {
                if (string.IsNullOrWhiteSpace(property.Name))
                {
                    error = "routes keys must be non-empty route ids.";
                    return false;
                }

                if (property.Value.ValueKind == JsonValueKind.Null)
                {
                    parsed[property.Name] = null;
                    continue;
                }

                if (property.Value.ValueKind != JsonValueKind.Number ||
                    !property.Value.TryGetInt32(out var slot))
                {
                    error = $"routes '{property.Name}' must be an integer slot or null.";
                    return false;
                }

                parsed[property.Name] = slot;
            }

            routes = parsed;
            error = null;
            return true;
        }
    }

    /// <summary>Null when the cue may run. Unknown scene, route or slot: a reason, and the
    /// caller must change nothing.</summary>
    public static string? Refuse(
        string sceneId,
        IReadOnlyCollection<string> knownScenes,
        IReadOnlyCollection<string> knownRouteIds,
        IReadOnlyCollection<int> knownSlots,
        IReadOnlyDictionary<string, int?> routes)
    {
        if (string.IsNullOrWhiteSpace(sceneId) ||
            !knownScenes.Any(id => string.Equals(id, sceneId, StringComparison.Ordinal)))
        {
            return $"Unknown scene '{sceneId}'.";
        }

        foreach (var routeId in routes.Keys)
        {
            if (!knownRouteIds.Any(id => string.Equals(id, routeId, StringComparison.Ordinal)))
            {
                return $"Unknown route '{routeId}' on scene '{sceneId}'.";
            }
        }

        foreach (var (routeId, slot) in routes)
        {
            if (slot is { } slotNumber && !knownSlots.Contains(slotNumber))
            {
                return $"Unknown Show Input slot {slotNumber} for route '{routeId}'.";
            }
        }

        return null;
    }
}
