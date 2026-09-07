using System.Text.Json;

namespace CoreVideoPro.ShowEngine;

/// <summary>Shell-side pre-flight over a <see cref="ShowConfig"/> before spawning the engine
/// (spec §9): the shell validates only what it can check against ITS OWN scene list; the engine
/// validates the rest of <c>engine</c> at startup. Presets left null are allowed here — the host
/// adapter (Task 10) refuses at USE time, not here.</summary>
public static class ShowConfigValidator
{
    private static readonly HashSet<string> ValidTransitions =
        new(StringComparer.Ordinal) { "cut", "fade", "dip", "wipe" };

    /// <summary>Returns the FIRST problem found, or null when the config is valid. Checks, in
    /// order: <c>engine</c> is an object; <c>engine.capacity == hostCapacity</c>; every
    /// <c>engine.looks[i].scenePreset</c> is in <paramref name="sceneIds"/>; each of the four
    /// <c>shell.presets.*</c>, when non-null, is in <paramref name="sceneIds"/>;
    /// <c>shell.defaultTransition</c> is one of cut/fade/dip/wipe.</summary>
    public static string? Validate(ShowConfig config, IReadOnlySet<string> sceneIds, int hostCapacity = 10)
    {
        if (config.Engine.ValueKind != JsonValueKind.Object)
        {
            return "config.engine must be an object";
        }

        var capacityProblem = ValidateCapacity(config.Engine, hostCapacity);
        if (capacityProblem is not null)
        {
            return capacityProblem;
        }

        var lookProblem = ValidateLooks(config.Engine, sceneIds);
        if (lookProblem is not null)
        {
            return lookProblem;
        }

        var presetProblem = ValidatePresets(config.Shell.Presets, sceneIds);
        if (presetProblem is not null)
        {
            return presetProblem;
        }

        if (!ValidTransitions.Contains(config.Shell.DefaultTransition))
        {
            return $"defaultTransition must be one of cut, fade, dip, wipe; found '{config.Shell.DefaultTransition}'";
        }

        return null;
    }

    private static string? ValidateCapacity(JsonElement engine, int hostCapacity)
    {
        if (!engine.TryGetProperty("capacity", out var capacityElement))
        {
            return $"config.capacity must be {hostCapacity} (the Show Input count); found <missing>";
        }

        if (capacityElement.ValueKind == JsonValueKind.Number &&
            capacityElement.TryGetInt32(out var capacity) &&
            capacity == hostCapacity)
        {
            return null;
        }

        var found = capacityElement.ValueKind == JsonValueKind.Number
            ? capacityElement.GetRawText()
            : capacityElement.ValueKind.ToString();
        return $"config.capacity must be {hostCapacity} (the Show Input count); found {found}";
    }

    private static string? ValidateLooks(JsonElement engine, IReadOnlySet<string> sceneIds)
    {
        if (!engine.TryGetProperty("looks", out var looksElement) || looksElement.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        foreach (var look in looksElement.EnumerateArray())
        {
            if (look.ValueKind != JsonValueKind.Object)
            {
                continue;
            }

            if (!look.TryGetProperty("scenePreset", out var presetElement) ||
                presetElement.ValueKind != JsonValueKind.String)
            {
                continue;
            }

            var preset = presetElement.GetString() ?? string.Empty;
            if (sceneIds.Contains(preset))
            {
                continue;
            }

            var id = look.TryGetProperty("id", out var idElement) && idElement.ValueKind == JsonValueKind.String
                ? idElement.GetString()
                : "?";
            return $"look '{id}' names scene '{preset}' which does not exist";
        }

        return null;
    }

    private static string? ValidatePresets(ShowPresetScenes presets, IReadOnlySet<string> sceneIds)
    {
        foreach (var (name, value) in new (string Name, string? Value)[]
                 {
                     ("solo", presets.Solo),
                     ("activeSpeaker", presets.ActiveSpeaker),
                     ("black", presets.Black),
                     ("gallery", presets.Gallery)
                 })
        {
            if (string.IsNullOrEmpty(value))
            {
                continue;
            }

            if (!sceneIds.Contains(value))
            {
                return $"preset '{name}' names scene '{value}' which does not exist";
            }
        }

        return null;
    }
}
