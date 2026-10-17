using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>One graphics command boundary for operator and HTTP/OSC intents.
/// An accepted result is native admission, never inferred preparation/delivery.</summary>
public sealed class GraphicsCommandGateway
{
    private readonly Func<bool> _running;
    private readonly Func<NativeMediaCoreStateSnapshot?> _observe;
    private readonly Func<IReadOnlyList<NativeMediaCoreCommand>, CancellationToken, Task<NativeMediaCoreStateSnapshot>> _send;
    private int _sending;

    public GraphicsCommandGateway(IMediaCoreBridge bridge)
        : this(() => bridge.Running && bridge.Profile?.HasAvailableCapability("graphics-instances") == true, () => bridge.LastSnapshot,
            (commands, cancellation) => bridge.SyncAsync(commands, cancellationToken: cancellation)) { }

    public GraphicsCommandGateway(Func<bool> running, Func<NativeMediaCoreStateSnapshot?> observe,
        Func<IReadOnlyList<NativeMediaCoreCommand>, CancellationToken, Task<NativeMediaCoreStateSnapshot>> send)
        => (_running, _observe, _send) = (running, observe, send);

    public async Task<NativeGraphicsOperationResult> InvokeAsync(string action, string request,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var observed = _observe()?.Graphics;
        if (!_running() || observed is not { SchemaVersion: 1 } || string.IsNullOrEmpty(observed.CoreEpoch))
            return Refuse("", "graphics-native-state-unavailable");
        if (!GraphicsCommandRequest.TryBuild(action, request, observed, out var command, out var requestId, out var error))
            return Refuse(requestId, error);
        if (Interlocked.CompareExchange(ref _sending, 1, 0) != 0) return Refuse(requestId, "graphics-command-overloaded");
        try
        {
            var response = await _send([command!], cancellationToken).ConfigureAwait(false);
            var result = response.Graphics?.LastResult;
            if (response.Graphics?.CoreEpoch != observed.CoreEpoch || result?.RequestId != requestId)
                return Refuse(requestId, "graphics-result-not-correlated");
            return result;
        }
        catch (MediaCoreCommandOverloadedException) { return Refuse(requestId, "graphics-command-overloaded"); }
        finally { Volatile.Write(ref _sending, 0); }
    }

    private static NativeGraphicsOperationResult Refuse(string requestId, string reason) =>
        new() { RequestId = requestId, Accepted = false, Reason = reason };
}

/// <summary>Bounded scalar-registry JSON request parser. Native validation remains
/// authoritative; this is not an arbitrary property setter or a second renderer.</summary>
public static class GraphicsCommandRequest
{
    private static readonly IReadOnlyDictionary<string, string[]> Parameters = new Dictionary<string, string[]>(StringComparer.Ordinal)
    {
        ["graphics.instance.create"] = ["templateId", "templateVersion", "scope", "sceneId", "sourceId", "fields", "style"],
        ["graphics.instance.update"] = ["target", "sourceId", "fields", "style"],
        ["graphics.instance.cue"] = [], ["graphics.instance.trigger"] = ["action"],
        ["graphics.instance.remove"] = [], ["graphics.program.clear"] = ["channel"]
    };
    private static readonly HashSet<string> Common = new(StringComparer.Ordinal)
        { "schemaVersion", "requestId", "clientId", "coreEpoch", "instanceId", "expectedRevision" };
    private static readonly HashSet<string> StyleStrings = new(StringComparer.Ordinal)
        { "preset", "anchor", "fontFamily", "nameColor", "titleColor", "backgroundColor", "accentColor" };
    private static readonly HashSet<string> StyleNumbers = new(StringComparer.Ordinal)
        { "nameSize", "titleSize", "backgroundOpacity", "padding", "width", "cornerRadius", "safeOffsetX", "safeOffsetY", "logoScale" };
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public static IReadOnlyCollection<string> Actions => Parameters.Keys.ToArray();

    public static bool TryBuild(string action, string request, NativeGraphicsState observed,
        out NativeMediaCoreCommand? command, out string requestId, out string error)
    {
        command = null; requestId = ""; error = "invalid-graphics-request";
        if (!Parameters.TryGetValue(action, out var specific)) { error = "unsupported-graphics-action"; return false; }
        if (request.Length > 32768 || Encoding.UTF8.GetByteCount(request) > 32768) return false;
        try
        {
            using var document = JsonDocument.Parse(request, new JsonDocumentOptions { MaxDepth = 4 });
            var body = document.RootElement;
            if (!GraphicsOperationIdentityContract.Validate(body) || Number(body, "schemaVersion") != 1) return false;
            requestId = body.GetProperty("requestId").GetString()!;
            if (!Id(requestId) || !Id(body.GetProperty("clientId").GetString()!)) return false;
            if (body.GetProperty("coreEpoch").GetString() != observed.CoreEpoch) { error = "core-epoch-conflict"; return false; }
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in body.EnumerateObject())
            {
                if (!names.Add(property.Name) || (!Common.Contains(property.Name) && !specific.Contains(property.Name, StringComparer.Ordinal)))
                { error = "unsupported-or-duplicate-request-field"; return false; }
                if (property.Name is "fields" or "style") continue;
                if (property.Name is "schemaVersion" or "expectedRevision" or "templateVersion")
                {
                    if (!Integer(property.Value, 0)) return false;
                }
                else if (property.Value.ValueKind != JsonValueKind.String || Encoding.UTF8.GetByteCount(property.Value.GetString()!) > 256)
                    return false;
            }
            if (action != "graphics.program.clear" && (!body.TryGetProperty("instanceId", out var instanceId) || !Id(instanceId.GetString()!))) return false;
            if (action is not ("graphics.instance.create" or "graphics.program.clear") &&
                (!body.TryGetProperty("expectedRevision", out var revision) || !Integer(revision, 1))) return false;
            if (action == "graphics.instance.create")
            {
                if (Number(body, "templateVersion") != 1 || !Template(Text(body, "templateId"))) return false;
                var scope = Text(body, "scope"); var scene = Text(body, "sceneId");
                if ((scope != "global" && scope != "scene") || (scope == "scene" && scene.Length == 0) || (scope == "global" && scene.Length != 0)) return false;
            }
            if (action == "graphics.instance.update" && Text(body, "target") is not ("cue" or "program")) return false;
            if (action == "graphics.instance.trigger" && Text(body, "action") is not ("show" or "hide" or "clear")) return false;
            if (action == "graphics.program.clear" && Text(body, "channel") != "all") return false;
            if (body.TryGetProperty("fields", out var fields))
            {
                if (fields.ValueKind != JsonValueKind.Object) return false;
                var fieldNames = new HashSet<string>(StringComparer.Ordinal);
                foreach (var field in fields.EnumerateObject())
                    if (!fieldNames.Add(field.Name) || field.Value.ValueKind != JsonValueKind.String ||
                        field.Name is not ("name" or "title") || Encoding.UTF8.GetByteCount(field.Value.GetString()!) > (field.Name == "name" ? 256 : 512))
                        return false;
            }
            if (body.TryGetProperty("style", out var style))
            {
                if (style.ValueKind != JsonValueKind.Object) return false;
                var current = observed.Instances.FirstOrDefault(i => i.InstanceId == Text(body, "instanceId"))?.Style
                    ?? LowerThirdAppearance.FromPreset(Text(body, "templateId"));
                var merged = JsonSerializer.SerializeToElement(current, JsonOptions).EnumerateObject()
                    .ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal);
                var styleNames = new HashSet<string>(StringComparer.Ordinal);
                foreach (var value in style.EnumerateObject())
                {
                    if (!styleNames.Add(value.Name)) return false;
                    if (StyleStrings.Contains(value.Name)) { if (value.Value.ValueKind != JsonValueKind.String) return false; }
                    else if (StyleNumbers.Contains(value.Name)) { if (!value.Value.TryGetDouble(out var number) || !double.IsFinite(number)) return false; }
                    else if (value.Name == "showLogo") { if (value.Value.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) return false; }
                    else if (value.Name != "version" || !Integer(value.Value, 1) || value.Value.GetDouble() != 1) return false;
                    merged[value.Name] = value.Name == "version" ? JsonSerializer.SerializeToElement(1) : value.Value.Clone();
                }
                if (JsonSerializer.Deserialize<LowerThirdAppearance>(JsonSerializer.Serialize(merged), JsonOptions)?.IsValid != true) return false;
            }
            command = new NativeMediaCoreCommand
            {
                Type = action.Replace('.', '-'),
                ExtensionData = new Dictionary<string, JsonElement> { ["graphics"] = body.Clone() }
            };
            error = "";
            return true;
        }
        catch (JsonException) { return false; }
        catch (InvalidOperationException) { return false; }
    }

    private static string Text(JsonElement value, string key) => value.TryGetProperty(key, out var item) && item.ValueKind == JsonValueKind.String ? item.GetString()! : "";
    private static double Number(JsonElement value, string key) => value.TryGetProperty(key, out var item) && item.ValueKind == JsonValueKind.Number && item.TryGetDouble(out var number) ? number : -1;
    private static bool Integer(JsonElement value, double minimum) => value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var number) &&
        double.IsFinite(number) && Math.Truncate(number) == number && number >= minimum && number <= 9007199254740991d;
    private static bool Id(string value) => value.Length is > 0 and <= 64 && value[0] is >= 'a' and <= 'z' &&
        value.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '-');
    private static bool Template(string value) => value is "compact-solid" or "minimal-accent" or "broadcast";
}
