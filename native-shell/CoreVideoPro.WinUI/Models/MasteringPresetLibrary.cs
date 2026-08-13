using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.Models;

/// <summary>An operator-saved master-rack preset (B2). Immutable record; the
/// library operations below return updated lists rather than mutating.</summary>
public sealed record MasteringUserPreset(string Id, string Name, MasteringSettings Settings);

/// <summary>
/// Pure list operations for operator-saved mastering presets
/// (master-vst-round2-spec §B2: save/rename/delete alongside the 4 built-ins).
/// Built-ins live in <see cref="MasteringPresetCatalog"/> and are immutable —
/// this library refuses names that collide with them so the preset surface
/// never shows two different "Streaming" entries. All operations are
/// case-insensitive on names, trim whitespace, and never throw for operator
/// input: an invalid request returns the input list unchanged with a null/false
/// result the caller surfaces as status text.
/// </summary>
public static class MasteringPresetLibrary
{
    public static readonly IReadOnlyList<string> BuiltInNames = ["Streaming", "Podcast", "Broadcast", "Neutral"];

    public const int MaxPresets = 32;
    public const int MaxNameLength = 48;

    public static bool IsBuiltInName(string? name) =>
        !string.IsNullOrWhiteSpace(name) &&
        BuiltInNames.Any(builtIn => string.Equals(builtIn, name.Trim(), StringComparison.OrdinalIgnoreCase));

    public static string? NormalizeName(string? name)
    {
        var trimmed = name?.Trim();
        if (string.IsNullOrEmpty(trimmed) || trimmed.Length > MaxNameLength || IsBuiltInName(trimmed))
        {
            return null;
        }

        return trimmed;
    }

    /// <summary>Saves <paramref name="settings"/> under <paramref name="name"/>.
    /// An existing custom preset with the same name is overwritten in place
    /// (same id — "save" doubles as "update"). Returns null when the name is
    /// invalid/built-in or the library is full.</summary>
    public static (IReadOnlyList<MasteringUserPreset> Presets, MasteringUserPreset Saved)? Save(
        IReadOnlyList<MasteringUserPreset> presets, string? name, MasteringSettings settings)
    {
        if (NormalizeName(name) is not { } normalized)
        {
            return null;
        }

        var existing = presets.FirstOrDefault(preset =>
            string.Equals(preset.Name, normalized, StringComparison.OrdinalIgnoreCase));
        if (existing is null && presets.Count >= MaxPresets)
        {
            return null;
        }

        var saved = existing is null
            ? new MasteringUserPreset($"user-{Guid.NewGuid():N}", normalized, settings)
            : existing with { Name = normalized, Settings = settings };
        var updated = existing is null
            ? presets.Append(saved).ToList()
            : presets.Select(preset => preset.Id == saved.Id ? saved : preset).ToList();
        return (updated, saved);
    }

    /// <summary>Renames the preset with <paramref name="id"/>. Returns null when
    /// the id is unknown, the name is invalid/built-in, or another custom preset
    /// already uses it.</summary>
    public static (IReadOnlyList<MasteringUserPreset> Presets, MasteringUserPreset Renamed)? Rename(
        IReadOnlyList<MasteringUserPreset> presets, string? id, string? newName)
    {
        if (NormalizeName(newName) is not { } normalized)
        {
            return null;
        }

        var target = presets.FirstOrDefault(preset => string.Equals(preset.Id, id, StringComparison.Ordinal));
        if (target is null)
        {
            return null;
        }

        var collision = presets.Any(preset =>
            preset.Id != target.Id &&
            string.Equals(preset.Name, normalized, StringComparison.OrdinalIgnoreCase));
        if (collision)
        {
            return null;
        }

        var renamed = target with { Name = normalized };
        return (presets.Select(preset => preset.Id == renamed.Id ? renamed : preset).ToList(), renamed);
    }

    /// <summary>Deletes the preset with <paramref name="id"/>; unknown ids
    /// return null (nothing removed).</summary>
    public static (IReadOnlyList<MasteringUserPreset> Presets, MasteringUserPreset Deleted)? Delete(
        IReadOnlyList<MasteringUserPreset> presets, string? id)
    {
        var target = presets.FirstOrDefault(preset => string.Equals(preset.Id, id, StringComparison.Ordinal));
        if (target is null)
        {
            return null;
        }

        return (presets.Where(preset => preset.Id != target.Id).ToList(), target);
    }

    // ---- Preferences (v6) conversion — keep the at-rest shape in one place ----

    public static PersistedMasteringSettings ToPersisted(MasteringSettings settings) => new()
    {
        Enabled = settings.Enabled,
        TargetIndex = settings.TargetIndex,
        GlueAmount = settings.GlueAmount,
        CeilingDbfs = settings.CeilingDbfs,
        MaxRideDb = settings.MaxRideDb,
        InputGainDb = settings.InputGainDb,
        HighPassHz = settings.HighPassHz,
        LowPassHz = settings.LowPassHz,
        LowShelfDb = settings.LowShelfDb,
        PresenceDb = settings.PresenceDb,
        HighShelfDb = settings.HighShelfDb,
        StereoWidth = settings.StereoWidth,
        LimiterEnabled = settings.LimiterEnabled,
        GlueRatio = settings.GlueRatio,
        GlueAttackMs = settings.GlueAttackMs,
        GlueReleaseMs = settings.GlueReleaseMs,
        GlueMakeupDb = settings.GlueMakeupDb,
        GlueMultiband = settings.GlueMultiband,
        GlueBandLowDb = settings.GlueBandLowDb,
        GlueBandMidDb = settings.GlueBandMidDb,
        GlueBandHighDb = settings.GlueBandHighDb
    };

    public static MasteringSettings FromPersisted(PersistedMasteringSettings persisted) => new(
        persisted.Enabled,
        Math.Clamp(persisted.TargetIndex, 0, 2),
        Math.Clamp(persisted.GlueAmount, 0.0, 1.0),
        Math.Clamp(persisted.CeilingDbfs, -6.0, -0.5),
        Math.Clamp(persisted.MaxRideDb, 0.0, 12.0),
        Math.Clamp(persisted.InputGainDb, -12.0, 12.0),
        Math.Clamp(persisted.HighPassHz, 0.0, 300.0),
        Math.Clamp(persisted.LowPassHz, 0.0, 20000.0),
        Math.Clamp(persisted.LowShelfDb, -12.0, 12.0),
        Math.Clamp(persisted.PresenceDb, -12.0, 12.0),
        Math.Clamp(persisted.HighShelfDb, -12.0, 12.0),
        Math.Clamp(persisted.StereoWidth, 0.0, 2.0),
        persisted.LimiterEnabled,
        Math.Clamp(persisted.GlueRatio, 1.0, 10.0),
        Math.Clamp(persisted.GlueAttackMs, 1.0, 200.0),
        Math.Clamp(persisted.GlueReleaseMs, 20.0, 1000.0),
        Math.Clamp(persisted.GlueMakeupDb, -6.0, 6.0),
        persisted.GlueMultiband,
        Math.Clamp(persisted.GlueBandLowDb, -6.0, 6.0),
        Math.Clamp(persisted.GlueBandMidDb, -6.0, 6.0),
        Math.Clamp(persisted.GlueBandHighDb, -6.0, 6.0));

    public static PersistedMasteringPreset ToPersistedPreset(MasteringUserPreset preset) => new()
    {
        Id = preset.Id,
        Name = preset.Name,
        Settings = ToPersisted(preset.Settings)
    };

    /// <summary>Restores the persisted preset list, dropping entries that are
    /// unusable at rest (blank id/name or a name that now collides with a
    /// built-in) rather than failing the whole preferences load.</summary>
    public static List<MasteringUserPreset> FromPersistedPresets(IEnumerable<PersistedMasteringPreset>? persisted)
    {
        var restored = new List<MasteringUserPreset>();
        foreach (var preset in persisted ?? [])
        {
            if (string.IsNullOrWhiteSpace(preset.Id) || NormalizeName(preset.Name) is not { } name ||
                restored.Any(existing => string.Equals(existing.Name, name, StringComparison.OrdinalIgnoreCase)))
            {
                continue;
            }

            restored.Add(new MasteringUserPreset(preset.Id, name, FromPersisted(preset.Settings)));
            if (restored.Count >= MaxPresets)
            {
                break;
            }
        }

        return restored;
    }
}
