using System.Text.Json;

namespace CoreVideoPro.ShowEngine;

/// <summary>The whole <c>ohg-show-config.json</c> document (spec §9). <see cref="Engine"/> is opaque
/// to the shell except for the handful of fields it reads directly (capacity, statePath,
/// looks[].scenePreset) — everything else is validated engine-side by
/// <c>parseShowEngineConfig</c> in the TypeScript host.</summary>
public sealed class ShowConfig
{
    public const int CurrentVersion = 1;

    public int Version { get; init; } = CurrentVersion;

    public JsonElement Engine { get; init; }

    public ShowShellConfig Shell { get; init; } = new();
}

/// <summary>The shell-owned half of the config: which scenes to cue for the engine's fixed
/// preview slots, the default take transition, and (reserved, unused in Plan 7a) the tally
/// publisher's HTTP target.</summary>
public sealed class ShowShellConfig
{
    /// <summary>Default false = shadow mode: host commands are logged, never applied.</summary>
    public bool DriveHost { get; init; }

    public ShowPresetScenes Presets { get; init; } = new();

    public string DefaultTransition { get; init; } = "cut";

    public string? TallyUrl { get; init; }
}

/// <summary>The four fixed on-screen presets the engine's <c>setPreview</c>/<c>applyLook</c> host
/// commands cue by name. Each is a scene id or null (unconfigured — refused at use time, not here).</summary>
public sealed class ShowPresetScenes
{
    public string? Solo { get; init; }

    public string? ActiveSpeaker { get; init; }

    public string? Black { get; init; }

    public string? Gallery { get; init; }
}
