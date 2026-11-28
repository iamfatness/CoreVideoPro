using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Live re-theme (graphics styles slice 1, owner ruling 2026-11-27): a palette change
/// updates Cue and Preview at once; Program changes only on an explicit Update on air, through
/// the existing <c>graphics.instance.update target=program</c> contract. Only playlist-row
/// instances of declarative graphics take part; Design, automation and compatibility-look
/// instances keep the palette they were created with until they are shown again.</summary>
public sealed partial class GraphicsPlayoutService
{
    /// <summary>The resolved palette a playlist instance should carry now (row override -> show
    /// palette), or null when the instance is not a declarative playlist graphic.</summary>
    public IReadOnlyDictionary<string, string>? DesiredPalette(NativeGraphicsInstance instance)
    {
        if (RowOf(instance.InstanceId) is not { } row) return null;
        var templateId = row.Image?.TemplateId ?? row.TemplateId;
        if (templateId.Length == 0 || !GraphicsPaletteMigration.Declarative(templateId) || !GraphicsPaletteMigration.Declarative(instance.TemplateId)) return null;
        return _library.EffectivePalette.Resolve(row.BrandOverrides);
    }

    /// <summary>Playlist graphics on Program whose Program revision was drawn with a different
    /// palette than the one they would get now. Native truth only (<c>programPalette</c>).</summary>
    public IReadOnlyList<NativeGraphicsInstance> PaletteStaleOnAir(NativeGraphicsState? state = null) =>
        (state ?? _observe())?.Instances.Where(i => IsOnProgram(i) && DesiredPalette(i) is { } want &&
            !GraphicsPalette.SameResolved(GraphicsPalette.FromWire(i.ProgramPalette), want)).ToArray() ?? [];

    /// <summary>Re-cue every ordinary playlist cue whose cue revision has an older palette, so
    /// Preview shows the new colours now. Layout-owned cues are guarded by their Take and are left
    /// alone. Program is never touched here.</summary>
    public Task<NativeGraphicsOperationResult> RefreshPaletteAsync(CancellationToken cancellation = default) => Run(async () => {
        var state = Available(); var epoch = _epoch = state.CoreEpoch; var refreshed = 0;
        foreach (var instance in state.Instances.Where(i => IsOnPreview(i) && i.CueOwner.Length == 0).ToArray()) {
            if (DesiredPalette(instance) is not { } want || GraphicsPalette.SameResolved(GraphicsPalette.FromWire(instance.CuePalette), want)) continue;
            var updated = await Send("graphics.instance.update", new() { ["instanceId"] = instance.InstanceId, ["expectedRevision"] = instance.Revision,
                ["target"] = "cue", ["palette"] = want }, cancellation);
            if (!updated.Accepted) return updated with { Reason = "Preview kept its colours: " + updated.Reason };
            var revision = updated.CurrentRevision > 0 ? updated.CurrentRevision : Current(epoch).Instances.FirstOrDefault(i => i.InstanceId == instance.InstanceId)?.Revision ?? 0;
            if (revision < 1) return Refuse("The native update result has no revision.");
            var cued = await Send("graphics.instance.cue", new() { ["instanceId"] = instance.InstanceId, ["expectedRevision"] = revision, ["rideTake"] = instance.RideTake }, cancellation);
            if (!cued.Accepted) return cued with { Reason = "Preview kept its colours: " + cued.Reason };
            refreshed++;
        }
        return new NativeGraphicsOperationResult { Accepted = true,
            Reason = refreshed == 0 ? "Palette applied. Nothing cued needed new colours." : $"Palette applied to {refreshed} cued graphic{(refreshed == 1 ? "" : "s")}. Program is unchanged." };
    });

    /// <summary>The palette surface's one Update on air button: every stale Program graphic adopts
    /// the current palette and nothing else. A graphic whose draft has moved on from Program (a
    /// newer cue with other edits) is skipped and named, because a Program update would carry
    /// those edits too; Show it from Preview instead.</summary>
    public Task<NativeGraphicsOperationResult> UpdateOnAirAsync(CancellationToken cancellation = default) => Run(async () => {
        var state = Available(); _epoch = state.CoreEpoch;
        var stale = PaletteStaleOnAir(state); var updated = 0; var skipped = new List<string>();
        foreach (var instance in stale) {
            var label = RowOf(instance.InstanceId)?.Name ?? instance.InstanceId;
            if (instance.Revision != instance.ProgramRevision) { skipped.Add(label); continue; }
            var result = await Send("graphics.instance.update", new() { ["instanceId"] = instance.InstanceId, ["expectedRevision"] = instance.Revision,
                ["target"] = "program", ["palette"] = DesiredPalette(instance) }, cancellation);
            if (!result.Accepted) return result with { Reason = $"{label} kept its colours on air: {result.Reason}" };
            updated++;
        }
        if (stale.Count == 0) return new NativeGraphicsOperationResult { Accepted = true, Reason = "Every graphic on air already uses this palette." };
        var reason = updated == 0 ? "Nothing on air was updated." : $"Updated {updated} graphic{(updated == 1 ? "" : "s")} on air.";
        if (skipped.Count > 0) reason += $" {string.Join(", ", skipped)}: a newer cue is waiting; Show it from Preview to update it.";
        return new NativeGraphicsOperationResult { Accepted = updated > 0 || skipped.Count == 0, Reason = reason };
    });
}
