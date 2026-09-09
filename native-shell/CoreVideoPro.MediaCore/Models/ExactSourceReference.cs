using System.Text;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

// Exact provider incarnation, not a participant identity or a durable person.
// Foundation only: production scene commands do not emit this until capability-gated.
public sealed record ExactSourceReference
{
    [JsonPropertyName("sourceId")] public string SourceId { get; }
    [JsonPropertyName("instanceId")] public string InstanceId { get; }
    [JsonPropertyName("processEpoch")] public string ProcessEpoch { get; }
    [JsonPropertyName("generation")] public long Generation { get; }
    [JsonPropertyName("kind")] public string Kind { get; }

    [JsonConstructor]
    public ExactSourceReference(string sourceId, string instanceId, string processEpoch, long generation, string kind)
    {
        SourceId = Identity(sourceId, nameof(sourceId));
        InstanceId = Identity(instanceId, nameof(instanceId));
        ProcessEpoch = Identity(processEpoch, nameof(processEpoch));
        if (generation is <= 0 or > 9_007_199_254_740_991) throw new ArgumentOutOfRangeException(nameof(generation));
        if (kind is not ("camera" or "share")) throw new ArgumentException("Unknown Zoom source kind.", nameof(kind));
        Generation = generation;
        Kind = kind;
    }

    private static string Identity(string value, string name)
    {
        ArgumentNullException.ThrowIfNull(value, name);
        if (value.Length == 0 || value.Length > 512 || new UTF8Encoding(false, true).GetByteCount(value) > 512)
            throw new ArgumentException("A nonempty identity of at most 512 UTF-8 bytes is required.", name);
        return value;
    }
}
