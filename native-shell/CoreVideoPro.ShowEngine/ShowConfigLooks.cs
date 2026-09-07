using System.Text.Json;

namespace CoreVideoPro.ShowEngine;

/// <summary>
/// The one thing the shell needs out of <c>config.engine.looks[]</c> besides validation: the
/// <c>lookId → scenePreset</c> map.
///
/// <para><b>Why this exists (Plan 7a Task 13, fix round 1).</b> <c>OhgHostAdapter</c> cues a scene
/// for <c>setPreview({kind:"look", lookId})</c>, and the only field that names the scene is
/// <c>applyLook</c>'s <c>scenePreset</c>. The engine, however, emits the <c>setPreview</c> for a
/// look BEFORE the <c>applyLook</c> that first names it — same tick, one <c>seq</c> apart — so an
/// adapter that learned the mapping only from <c>applyLook</c> refused the first cue of every look
/// on every show. The mapping was in the config all along; the adapter simply was not given it.
/// Seeding from here makes the adapter answer that command from the FIRST tick, and
/// <c>applyLook</c> keeps updating the map as a second, live source (a look whose preset was
/// edited engine-side without a config round trip still resolves).</para>
///
/// <para>Pure and total by design: this parses an operator-editable document, so a missing
/// <c>looks</c> array, a non-array, a non-object entry, or an entry missing either string field is
/// simply not in the result. Rejecting a malformed look is
/// <see cref="ShowConfigValidator"/>'s job and it has its own message for it; duplicating the
/// refusal here would mean two places deciding a config is bad.</para>
/// </summary>
public static class ShowConfigLooks
{
    /// <summary>Every <c>looks[]</c> entry that carries both a string <c>id</c> and a string
    /// <c>scenePreset</c>, keyed by id. A duplicate id keeps the FIRST entry — the same one
    /// <c>parseShowEngineConfig</c> would resolve, since the engine's look list is ordered and its
    /// lookup is by first match.</summary>
    public static IReadOnlyDictionary<string, string> PresetsByLookId(JsonElement engine)
    {
        var presets = new Dictionary<string, string>(StringComparer.Ordinal);

        if (engine.ValueKind != JsonValueKind.Object ||
            !engine.TryGetProperty("looks", out var looks) ||
            looks.ValueKind != JsonValueKind.Array)
        {
            return presets;
        }

        foreach (var look in looks.EnumerateArray())
        {
            if (look.ValueKind != JsonValueKind.Object) continue;

            if (!look.TryGetProperty("id", out var id) || id.ValueKind != JsonValueKind.String) continue;
            if (!look.TryGetProperty("scenePreset", out var preset) || preset.ValueKind != JsonValueKind.String) continue;

            var lookId = id.GetString();
            var scenePreset = preset.GetString();
            if (string.IsNullOrEmpty(lookId) || string.IsNullOrEmpty(scenePreset)) continue;

            presets.TryAdd(lookId!, scenePreset!);
        }

        return presets;
    }
}
