using System.Text;
namespace CoreVideoPro.MediaCore.Models;

// Optional additive evidence: null means an older core supplied no catalog.
// Valid + [] means known empty; invalid evidence never exposes selectable tokens.
public sealed record NativeSourceAuthority
{
    public int Version { get; init; }
    public bool Valid { get; init; }
    public string ProcessEpoch { get; init; } = "";
    public long Sequence { get; init; }
    public IReadOnlyList<NativeSourceIdentity>? Sources { get; init; }
    public NativeSourceAuthority Validated()
    {
        const long max = 9_007_199_254_740_991;
        var valid = Valid && Version == 1 && Text(ProcessEpoch) && Sequence is > 0 and <= max
            && Sources is { Count: <= 8192 };
        if (valid)
        {
            var ids = new HashSet<string>(StringComparer.Ordinal);
            var instances = new HashSet<string>(StringComparer.Ordinal);
            var external = new HashSet<(string, string)>();
            foreach (var source in Sources!)
            {
                if (source is null || !Text(source.SourceId) || !Text(source.InstanceId)
                    || source.ProcessEpoch != ProcessEpoch || source.Generation is <= 0 or > max
                    || source.Kind is not ("camera" or "share") || source.Available is null
                    || !uint.TryParse(source.ParticipantId, System.Globalization.NumberStyles.None,
                        System.Globalization.CultureInfo.InvariantCulture, out var participant) || participant == 0
                    || source.ParticipantId != participant.ToString(System.Globalization.CultureInfo.InvariantCulture)
                    || !ids.Add(source.SourceId) || !instances.Add(source.InstanceId)
                    || !external.Add((source.ParticipantId, source.Kind)))
                { valid = false; break; }
            }
        }
        return valid ? this with { Sources = Array.AsReadOnly(Sources!.ToArray()) }
                     : new NativeSourceAuthority { Version = Version, Valid = false, Sources = [] };
    }
    private static bool Text(string? value)
    {
        if (string.IsNullOrEmpty(value) || value.Length > 512) return false;
        try { return new UTF8Encoding(false, true).GetByteCount(value) <= 512; }
        catch (EncoderFallbackException) { return false; }
    }
}
public sealed record NativeSourceIdentity
{
    public string SourceId { get; init; } = "";
    public string InstanceId { get; init; } = "";
    public string ProcessEpoch { get; init; } = "";
    public long Generation { get; init; }
    public string ParticipantId { get; init; } = ""; // SDK identity, never durable PersonId.
    public string Kind { get; init; } = "";
    public bool? Available { get; init; }
}
