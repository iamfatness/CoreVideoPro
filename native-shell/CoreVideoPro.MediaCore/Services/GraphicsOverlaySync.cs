using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Maps native render-plan overlay layers and caption track cues into studio graphics state.
/// Mirrors React overlay + caption transcript wiring from media-core snapshots.
/// </summary>
public static class GraphicsOverlaySync
{
    public const int MaxTranscriptEntries = 6;

    public sealed record CaptionTranscriptEntryPatch
    {
        public required string Id { get; init; }
        public required string SpeakerName { get; init; }
        public required string Role { get; init; }
        public required string Text { get; init; }
        public required int Confidence { get; init; }
    }

    /// <summary>
    /// Overlay ids present in the active render plan. Empty when no overlay layers are published.
    /// </summary>
    public static IReadOnlySet<string> ResolveActiveOverlayIds(NativeMediaCoreStateSnapshot snapshot) =>
        snapshot.RenderPlan.Layers
            .Where(layer => layer.Kind.Equals("overlay", StringComparison.OrdinalIgnoreCase))
            .Select(layer => layer.OverlayId)
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Select(id => id!.Trim())
            .ToHashSet(StringComparer.Ordinal);

    /// <summary>
    /// When the engine is running, map graphic ids to enabled flags from the render plan.
    /// Returns null when the engine is off so local toggles are preserved.
    /// </summary>
    public static IReadOnlyDictionary<string, bool>? ResolveOverlayEnabledFlags(
        NativeMediaCoreStateSnapshot snapshot,
        IEnumerable<string> graphicIds,
        bool engineRunning)
    {
        if (!engineRunning)
        {
            return null;
        }

        var activeOverlayIds = ResolveActiveOverlayIds(snapshot);
        if (activeOverlayIds.Count == 0 && snapshot.OverlayCount == 0)
        {
            return null;
        }

        return graphicIds.ToDictionary(
            id => id,
            id => activeOverlayIds.Contains(id),
            StringComparer.Ordinal);
    }

    /// <summary>
    /// Append a caption transcript entry when the snapshot publishes a new cue.
    /// </summary>
    public static IReadOnlyList<CaptionTranscriptEntryPatch> AppendCaptionTranscriptFromSnapshot(
        NativeMediaCoreStateSnapshot snapshot,
        IReadOnlyList<CaptionTranscriptEntryPatch> existing,
        IReadOnlyDictionary<string, string>? speakerRoles = null)
    {
        var cue = snapshot.CaptionTrack.CurrentCue;
        if (cue is null || string.IsNullOrWhiteSpace(cue.Text))
        {
            return existing;
        }

        var text = cue.Text.Trim();
        var speakerName = string.IsNullOrWhiteSpace(cue.Speaker) ? "Program audio" : cue.Speaker.Trim();
        var last = existing.LastOrDefault();
        if (last is not null &&
            last.SpeakerName.Equals(speakerName, StringComparison.Ordinal) &&
            last.Text.Equals(text, StringComparison.Ordinal))
        {
            return existing;
        }

        var role = ResolveSpeakerRole(speakerName, speakerRoles);
        var confidence = (int)Math.Clamp(Math.Round(cue.Confidence), 0, 100);
        var entry = new CaptionTranscriptEntryPatch
        {
            Id = $"cc-{cue.AtMs:0}-{Slug(speakerName)}",
            SpeakerName = speakerName,
            Role = role,
            Text = text,
            Confidence = confidence
        };

        return existing.Concat([entry]).TakeLast(MaxTranscriptEntries).ToList();
    }

    private static string ResolveSpeakerRole(
        string speakerName,
        IReadOnlyDictionary<string, string>? speakerRoles)
    {
        if (speakerRoles is not null &&
            speakerRoles.TryGetValue(speakerName, out var role) &&
            !string.IsNullOrWhiteSpace(role))
        {
            return role;
        }

        return "Speaker";
    }

    private static string Slug(string value)
    {
        var slug = new string(value
            .ToLowerInvariant()
            .Select(ch => char.IsLetterOrDigit(ch) ? ch : '-')
            .ToArray())
            .Trim('-');

        return string.IsNullOrWhiteSpace(slug) ? "speaker" : slug;
    }
}