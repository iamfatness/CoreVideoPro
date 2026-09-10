using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>A read-only observation of the last core snapshot the shell received.</summary>
/// <param name="Json">The core's own session-state JSON, redacted. Null when unavailable.</param>
/// <param name="ReceivedUtc">When the shell received it. Null when unavailable.</param>
/// <param name="UnavailableReason">Set iff <paramref name="Json"/> is null.</param>
public sealed record CoreSnapshotObservation(
    string? Json,
    DateTimeOffset? ReceivedUtc,
    string? UnavailableReason);

/// <summary>
/// The single boundary at which the core's raw session-state JSON may leave the process.
///
/// Redaction is applied HERE rather than at parse time on purpose: the snapshot arrives several
/// times a second on the shell's sync path, and an observation is served only when something asks
/// for one. Redacting per request keeps the media/sync path free of the cost while still making
/// it impossible to serve unredacted text — every consumer goes through
/// <see cref="Observe(NativeMediaCoreStateSnapshot?)"/>, and
/// <see cref="NativeMediaCoreStateSnapshot.RawJson"/> is <c>[JsonIgnore]</c> so it cannot escape
/// through ordinary serialization.
///
/// What the snapshot was audited to carry (2026-09-09, against native/src/core/MediaCore.cpp's
/// sessionState builder):
///  - NO stream keys, SRT passphrases, tokens, JWTs or ZAKs. Output destination settings
///    (url/streamKey/passphrase, modules::OutputDestinationSettings) are INPUTS to the core; the
///    published outputSenderSession carries only the destination ID ("rtmp"/"srt"/"ndi"),
///    counters and lifecycle. The RTMP/SRT adapters already pass endpoints through their own
///    redactedEndpoint()/redactedSrtUrl() before publishing.
///  - Free text that could quote one: sender warning / lastError / runtimeDetail, recording
///    warnings, and the top-level warnings array are adapter-authored strings. An ffmpeg failure
///    message is exactly the shape that would carry an rtmp URL with the key appended.
///  - browserSources[].url is operator-supplied and routinely carries an access token in its
///    query string.
///  - Filesystem paths (recording program/ISO paths, sendArtifactPath, encoderPath). These are
///    deliberately NOT redacted: the support bundle ships them under the same reasoning
///    ("ISO paths are not secrets", SupportBundle.cs), and which file a take actually wrote to is
///    load-bearing diagnostic evidence. They do reveal the Windows user profile name.
///
/// The free-text filter is <see cref="SupportBundleLogRedactor"/> — the same one the support
/// bundle uses on raw log tails, reused rather than reinvented — but it is applied per STRING
/// VALUE, not to the document as text. Run over whole JSON it corrupts the document: its rtmp
/// rule consumes non-whitespace greedily, so one URL in a <c>lastError</c> swallows the closing
/// quote and the properties after it. Walking the tree keeps every redaction inside the value it
/// belongs to, and adds the one thing a text filter over JSON cannot do safely — dropping the
/// value of a secret-NAMED property whose content looks like nothing in particular.
/// </summary>
public static class CoreSnapshotObserver
{
    /// <summary>Names the filter applied, so a consumer can tell which redaction contract a
    /// captured response was produced under.</summary>
    public const string RedactionFilter = "support-bundle-log-redactor";

    public static CoreSnapshotObservation Unavailable(string reason) => new(null, null, reason);

    /// <summary>Redacts and returns the snapshot's own JSON. Pure and allocation-bounded: it
    /// takes no lock, touches no core, and reads only state the caller already holds.</summary>
    public static CoreSnapshotObservation Observe(NativeMediaCoreStateSnapshot? snapshot)
    {
        if (snapshot is null)
        {
            return Unavailable("The shell has not received a core snapshot yet (engine off, or the core has not synced).");
        }

        if (string.IsNullOrWhiteSpace(snapshot.RawJson))
        {
            // A snapshot the shell synthesized rather than parsed (a generation fence, or a
            // fake in a test). Saying so is honest; serving a re-serialization of the typed
            // record would be the hand-picked projection this endpoint exists to replace.
            return Unavailable("The shell holds a snapshot that did not come from a core sync response, so the core's own JSON is not available.");
        }

        string redacted;
        try
        {
            redacted = Redact(snapshot.RawJson!);
        }
        catch (JsonException error)
        {
            // Fail closed: if the document cannot be walked it cannot be proven redacted, so it
            // is not served.
            return Unavailable($"The core snapshot could not be redacted and was withheld: {error.Message}");
        }

        return new CoreSnapshotObservation(redacted, snapshot.RawReceivedUtc, null);
    }

    /// <summary>Property names whose VALUE is a secret regardless of what it looks like. The
    /// suffix rules cover fields nobody has audited yet; they are suffixes rather than substrings
    /// on purpose, so the core's <c>keyPhase</c> / <c>keyer</c> / <c>keyPosition</c> /
    /// <c>keyLength</c> overlay diagnostics survive.</summary>
    private static readonly string[] SecretNameSuffixes =
        ["key", "token", "secret", "password", "passphrase", "jwt", "zak", "credential"];

    private static readonly HashSet<string> SecretNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "pwd", "authorization", "auth", "signature", "sig"
    };

    internal static bool IsSecretName(string name) =>
        SecretNames.Contains(name) ||
        SecretNameSuffixes.Any(suffix => name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase));

    /// <summary>Rewrites the document with every string value filtered and every secret-named
    /// value dropped. Structure, numbers and booleans are untouched — a redacted snapshot must
    /// still parse and still answer the diagnostic question that made someone ask for it.</summary>
    internal static string Redact(string json)
    {
        using var document = JsonDocument.Parse(json);
        using var stream = new MemoryStream(json.Length);
        using (var writer = new Utf8JsonWriter(stream))
        {
            WriteRedacted(document.RootElement, writer);
        }

        return Encoding.UTF8.GetString(stream.ToArray());
    }

    private static void WriteRedacted(JsonElement element, Utf8JsonWriter writer)
    {
        switch (element.ValueKind)
        {
            case JsonValueKind.Object:
                writer.WriteStartObject();
                foreach (var property in element.EnumerateObject())
                {
                    writer.WritePropertyName(property.Name);
                    if (IsSecretName(property.Name) && property.Value.ValueKind is JsonValueKind.String)
                    {
                        writer.WriteStringValue(SupportBundleLogRedactor.Placeholder);
                    }
                    else
                    {
                        WriteRedacted(property.Value, writer);
                    }
                }

                writer.WriteEndObject();
                break;
            case JsonValueKind.Array:
                writer.WriteStartArray();
                foreach (var item in element.EnumerateArray())
                {
                    WriteRedacted(item, writer);
                }

                writer.WriteEndArray();
                break;
            case JsonValueKind.String:
                writer.WriteStringValue(SupportBundleLogRedactor.Redact(element.GetString() ?? string.Empty));
                break;
            default:
                element.WriteTo(writer);
                break;
        }
    }
}
