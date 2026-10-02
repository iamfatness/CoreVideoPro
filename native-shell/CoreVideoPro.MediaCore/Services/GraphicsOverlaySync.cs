using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Maps caption track cues from media-core snapshots into the studio transcript.
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