using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text;
using System.Security.Cryptography;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class MediaCoreSupervisor
{
    public async Task<SourceGradeApplyOutcome> ApplySourceGradeAsync(string sourceId,ulong sourceEpoch,long expectedRevision,
        MediaCoreColorGradeWire grade,CancellationToken cancellationToken = default)
    {
        var advanced = grade.Advanced is not null ? await RegisterGradeDocumentAsync(grade.Advanced,cancellationToken).ConfigureAwait(false) : null;
        using var response = await SendAsync(new Dictionary<string,object?> {
            ["id"] = NextId(), ["type"] = "set-source-grade", ["sourceId"] = sourceId, ["sourceEpoch"] = sourceEpoch,
            ["expectedRevision"] = expectedRevision, ["grade"] = new { lut = grade.Lut, exposure = grade.Exposure,
                contrast = grade.Contrast, saturation = grade.Saturation, temperature = grade.Temperature, advanced }
        },cancellationToken).ConfigureAwait(false);
        ThrowIfRejected(response,"set-source-grade"); var root = response.RootElement;
        return new(root.TryGetProperty("accepted",out var accepted) && accepted.GetBoolean(),
            root.TryGetProperty("revision",out var revision) ? revision.GetInt64() : expectedRevision,
            root.TryGetProperty("sourceEpoch",out var epoch) ? epoch.GetUInt64() : sourceEpoch,
            root.TryGetProperty("reason",out var reason) ? reason.GetString() ?? "" : "");
    }
    private async Task<object> RegisterGradeDocumentAsync(AdvancedGradeDocument document,CancellationToken cancellationToken)
    {
        document.Validate();
        var json = JsonSerializer.Serialize(document,new JsonSerializerOptions(JsonSerializerDefaults.Web));
        return await RegisterGradeJsonAsync(json,cancellationToken).ConfigureAwait(false);
    }
    private async Task<object> RegisterGradeJsonAsync(string json,CancellationToken cancellationToken)
    {
        if (Encoding.UTF8.GetByteCount(json)>2_000_000) throw new ArgumentException("Grade document exceeds its 2 MB transport budget.");
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(json))).ToLowerInvariant();
        using var query = await SendAsync(new Dictionary<string,object?> {
            ["id"] = NextId(), ["type"] = "register-grade-document", ["documentRef"] = hash
        },cancellationToken).ConfigureAwait(false);
        ThrowIfRejected(query,"register-grade-document");
        if (!query.RootElement.TryGetProperty("present",out var present) || !present.GetBoolean()) {
            using var upload = await SendAsync(new Dictionary<string,object?> {
                ["id"] = NextId(), ["type"] = "register-grade-document", ["documentRef"] = hash, ["documentJson"] = json
            },cancellationToken).ConfigureAwait(false);
            ThrowIfRejected(upload,"register-grade-document");
        }
        return new Dictionary<string,object?> { ["documentRef"] = hash };
    }
    private async Task<object> PrepareGradeDocumentsAsync(IReadOnlyList<NativeMediaCoreCommand> commands,CancellationToken cancellationToken)
    {
        // Preserve the established serialization path when no advanced grade is present.
        static bool HasDocument(JsonElement node) => node.ValueKind switch {
            JsonValueKind.Object => node.EnumerateObject().Any(p => (p.NameEquals("advanced") && p.Value.ValueKind == JsonValueKind.Object) || HasDocument(p.Value)),
            JsonValueKind.Array => node.EnumerateArray().Any(HasDocument), _ => false };
        if (!commands.Any(c => c.ExtensionData?.Values.Any(HasDocument) == true)) return commands;
        var array = JsonSerializer.SerializeToNode(commands)!.AsArray();
        var registered = new Dictionary<string,object>();
        async Task Visit(JsonNode? node)
        {
            if (node is JsonArray children) { foreach (var child in children) await Visit(child).ConfigureAwait(false); }
            if (node is not JsonObject obj) return;
            foreach (var key in obj.Select(p=>p.Key).ToArray()) {
                if (key == "advanced" && obj[key] is JsonObject grade) {
                    var json = grade.ToJsonString();
                    if (!registered.TryGetValue(json,out var reference)) {
                        reference = await RegisterGradeJsonAsync(json,cancellationToken).ConfigureAwait(false); registered[json] = reference;
                    }
                    obj[key] = JsonSerializer.SerializeToNode(reference);
                } else await Visit(obj[key]).ConfigureAwait(false);
            }
        }
        await Visit(array).ConfigureAwait(false); return array;
    }
}
