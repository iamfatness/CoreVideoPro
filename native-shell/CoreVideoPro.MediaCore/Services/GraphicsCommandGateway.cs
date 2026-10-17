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
    private readonly IGraphicsPackageReader? _packages;
    private int _sending;

    public GraphicsCommandGateway(IMediaCoreBridge bridge)
        : this(() => bridge.Running && bridge.Profile?.HasAvailableCapability("graphics-instances") == true, () => bridge.LastSnapshot,
            (commands, cancellation) => bridge.SyncAsync(commands, cancellationToken: cancellation),
            new GraphicsPackageReader(Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT") ?? Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro", "graphics"))) { }

    public GraphicsCommandGateway(Func<bool> running, Func<NativeMediaCoreStateSnapshot?> observe,
        Func<IReadOnlyList<NativeMediaCoreCommand>, CancellationToken, Task<NativeMediaCoreStateSnapshot>> send, IGraphicsPackageReader? packages = null)
        => (_running, _observe, _send, _packages) = (running, observe, send, packages);

    public async Task<NativeGraphicsOperationResult> InvokeAsync(string action, string request,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var observed = _observe()?.Graphics;
        if (!_running() || observed is not { SchemaVersion: 1 } || string.IsNullOrEmpty(observed.CoreEpoch))
            return Refuse("", "graphics-native-state-unavailable");
        if (!GraphicsCommandRequest.TryBuild(action, request, observed, out var command, out var requestId, out var error))
            return Refuse(requestId, error);
        // File expansion is independent of the short command admission gate:
        // importing a package cannot block an operator's Hide or Clear.
            if (action == "graphics.template.install" && command!.ExtensionData!["graphics"].TryGetProperty("packagePath", out var packagePath))
            {
                if (_packages is null) return Refuse(requestId, "graphics-package-import-unavailable");
                GraphicsPackageInput input;
                try { input = await _packages.ReadAsync(packagePath.GetString()!, cancellationToken).ConfigureAwait(false); }
                catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException)
                { return Refuse(requestId, "graphics-package-invalid: " + ex.Message); }
                var body = command.ExtensionData["graphics"].EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal);
                body.Remove("packagePath"); body["manifest"] = JsonSerializer.SerializeToElement(input.Manifest);
                body["packageRoot"] = JsonSerializer.SerializeToElement(input.PackageRoot);
                if (!GraphicsCommandRequest.TryBuild(action, JsonSerializer.Serialize(body), observed, out command, out requestId, out error)) return Refuse(requestId, error);
            }
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
        ["graphics.instance.create"] = ["templateId", "templateVersion", "scope", "sceneId", "sourceId", "fields", "style", "animations", "binding", "placement"],
        ["graphics.instance.update"] = ["templateVersion", "target", "sourceId", "fields", "style", "animations", "binding", "placement"],
        ["graphics.instance.cue"] = [], ["graphics.instance.trigger"] = ["action", "target"],
        ["graphics.clock.control"] = ["action","configuration","expectedClockRevision"],
        ["graphics.instance.remove"] = [], ["graphics.program.clear"] = ["channel"],
        ["graphics.template.install"] = ["manifest", "packageRoot", "packagePath"], ["graphics.template.list"] = []
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
        var limit = action == "graphics.template.install" ? 2 * 1024 * 1024 : 32768;
        if (request.Length > limit || Encoding.UTF8.GetByteCount(request) > limit) return false;
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
                if (property.Name is "fields" or "style" or "animations" or "binding" or "placement" or "configuration") continue;
                if (property.Name == "manifest")
                {
                    if (property.Value.ValueKind != JsonValueKind.String || string.IsNullOrEmpty(property.Value.GetString()) ||
                        Encoding.UTF8.GetByteCount(property.Value.GetString()!) > 256 * 1024) return false;
                    continue;
                }
                if (property.Name is "schemaVersion" or "expectedRevision" or "expectedClockRevision" or "templateVersion")
                {
                    if (!Integer(property.Value, 0)) return false;
                }
                else if (property.Value.ValueKind != JsonValueKind.String || Encoding.UTF8.GetByteCount(property.Value.GetString()!) > (property.Name is "packagePath" or "packageRoot" ? 2048 : 256) || property.Value.GetString()!.Contains('\0'))
                    return false;
            }
            var libraryAction = action.StartsWith("graphics.template.", StringComparison.Ordinal);
            if (action == "graphics.template.install") {
                var manifest = body.TryGetProperty("manifest", out _); var path = body.TryGetProperty("packagePath", out var archive);
                if (manifest == path || (path && (string.IsNullOrWhiteSpace(archive.GetString()) || body.TryGetProperty("packageRoot", out _)))) return false;
            }
            if (!libraryAction && action != "graphics.program.clear" && (!body.TryGetProperty("instanceId", out var instanceId) || !Id(instanceId.GetString()!))) return false;
            if (!libraryAction && action is not ("graphics.instance.create" or "graphics.program.clear") &&
                (!body.TryGetProperty("expectedRevision", out var revision) || !Integer(revision, 1))) return false;
            if (action == "graphics.instance.create")
            {
                if (!(Template(Text(body, "templateId")) && Number(body, "templateVersion") == 1) &&
                    !observed.Templates.Any(t => t.TemplateId == Text(body, "templateId") && t.TemplateVersion == Number(body, "templateVersion") && t.Runtime == "declarative")) return false;
                var scope = Text(body, "scope"); var scene = Text(body, "sceneId");
                if ((scope != "global" && scope != "scene") || (scope == "scene" && scene.Length == 0) || (scope == "global" && scene.Length != 0)) return false;
            }
            if (action == "graphics.instance.update" && Text(body, "target") is not ("cue" or "program")) return false;
            if (action == "graphics.instance.trigger" && Text(body, "action") is not ("show" or "hide" or "clear")) return false;
            if (action == "graphics.instance.trigger" && body.TryGetProperty("target", out _) &&
                (Text(body, "target") != "cue" || Text(body, "action") != "show")) return false;
            if (action == "graphics.program.clear" && Text(body, "channel") != "all") return false;
            if (body.TryGetProperty("binding", out var binding))
            {
                if (binding.ValueKind == JsonValueKind.Null)
                {
                    if (Text(body, "sourceId").Length != 0) return false;
                }
                else
                {
                    if (!SourceInstanceIdentityContract.Validate(binding)) { error = "invalid-source-binding"; return false; }
                    var bindingNames = new HashSet<string>(StringComparer.Ordinal);
                    foreach (var value in binding.EnumerateObject())
                        if (!bindingNames.Add(value.Name) || value.Name is not ("sourceId" or "instanceId" or "processEpoch" or "generation") ||
                            (value.Value.ValueKind == JsonValueKind.String && Encoding.UTF8.GetByteCount(value.Value.GetString()!) > 512)) return false;
                    if (body.TryGetProperty("sourceId", out _) && Text(body, "sourceId") != Text(binding, "sourceId")) return false;
                }
            }
            else if (body.TryGetProperty("sourceId", out _) && Text(body, "sourceId").Length != 0 &&
                observed.Instances.FirstOrDefault(i => i.InstanceId == Text(body, "instanceId"))?.Binding?.SourceId != Text(body, "sourceId"))
            { error = "source-binding-required"; return false; }
            if (action == "graphics.clock.control") {
                var operation = Text(body,"action");
                if (operation is not ("configure" or "start" or "pause" or "resume" or "reset")) return false;
                if (operation == "configure") {
                    if(!body.TryGetProperty("expectedClockRevision",out var clockRevision) || !Integer(clockRevision,1)) return false;
                    if (!body.TryGetProperty("configuration",out var config) || config.ValueKind != JsonValueKind.Object || config.EnumerateObject().Count() is <6 or >7 ||
                        config.EnumerateObject().Select(p=>p.Name).Distinct(StringComparer.Ordinal).Count()!=config.EnumerateObject().Count() ||
                        config.EnumerateObject().Any(p=>p.Name is not ("mode" or "zone" or "format" or "finish" or "offsetMinutes" or "durationSeconds" or "restoreRemainingSeconds")) ||
                        new[] { "mode","zone","format","finish","offsetMinutes","durationSeconds" }.Any(key=>!config.TryGetProperty(key,out _)) ||
                        config.Deserialize<NativeGraphicsClockConfiguration>(JsonOptions)?.IsValid!=true) return false;
                } else if (body.TryGetProperty("configuration",out _)) return false;
            }
            if (body.TryGetProperty("placement", out var placement) && placement.ValueKind != JsonValueKind.Null)
            {
                if (placement.ValueKind != JsonValueKind.Object || placement.EnumerateObject().Count() != 4 ||
                    placement.EnumerateObject().Select(p => p.Name).Distinct(StringComparer.Ordinal).Count() != 4 ||
                    placement.EnumerateObject().Any(p => p.Name is not ("anchor" or "x" or "y" or "scale")) ||
                    placement.Deserialize<NativeGraphicsPlacement>(JsonOptions)?.IsValid != true) return false;
            }
            if (body.TryGetProperty("animations", out var animations) && !ValidAnimations(animations)) return false;
            if (body.TryGetProperty("fields", out var fields))
            {
                if (fields.ValueKind != JsonValueKind.Object) return false;
                var instance = observed.Instances.FirstOrDefault(i => i.InstanceId == Text(body, "instanceId"));
                var template = observed.Templates.FirstOrDefault(t => t.TemplateId ==
                    (action == "graphics.instance.create" ? Text(body, "templateId") : instance?.TemplateId) &&
                    t.TemplateVersion == (body.TryGetProperty("templateVersion",out _) ? Number(body,"templateVersion") : instance?.TemplateVersion));
                var fieldNames = new HashSet<string>(StringComparer.Ordinal);
                foreach (var field in fields.EnumerateObject())
                {
                    if (!fieldNames.Add(field.Name)) return false;
                    if (template?.Runtime == "declarative")
                    {
                        var descriptor = template.Fields.FirstOrDefault(f => f.Id == field.Name);
                        if (descriptor is null || !ValidField(field.Value, descriptor)) return false;
                    }
                    else if (field.Value.ValueKind != JsonValueKind.String || field.Name is not ("name" or "title") ||
                        Encoding.UTF8.GetByteCount(field.Value.GetString()!) > (field.Name == "name" ? 256 : 512)) return false;
                }
            }
            if (body.TryGetProperty("style", out var style))
            {
                if (style.ValueKind != JsonValueKind.Object) return false;
                var templateId = action == "graphics.instance.create" ? Text(body, "templateId") :
                    observed.Instances.FirstOrDefault(i => i.InstanceId == Text(body, "instanceId"))?.TemplateId;
                var declarative = observed.Templates.Any(t => t.TemplateId == templateId && t.Runtime == "declarative");
                var current = observed.Instances.FirstOrDefault(i => i.InstanceId == Text(body, "instanceId"))?.Style
                    ?? LowerThirdAppearance.FromPreset(Text(body, "templateId"));
                var merged = JsonSerializer.SerializeToElement(current, JsonOptions).EnumerateObject()
                    .ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal);
                var styleNames = new HashSet<string>(StringComparer.Ordinal);
                foreach (var value in style.EnumerateObject())
                {
                    if (!styleNames.Add(value.Name)) return false;
                    if (declarative && value.Name is not ("version" or "anchor" or "fontFamily" or "nameColor" or "titleColor" or
                        "backgroundColor" or "accentColor" or "backgroundOpacity" or "safeOffsetX" or "safeOffsetY")) return false;
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
    internal static bool ValidField(JsonElement value, NativeGraphicsTemplateField field)
    {
        if (field.Choices.Count != 0 && !field.Choices.Any(choice => JsonElement.DeepEquals(value, choice))) return false;
        return field.Type switch
        {
            "string" => value.ValueKind == JsonValueKind.String && Encoding.UTF8.GetByteCount(value.GetString()!) <= (field.MaxLength ?? 512),
            "color" => value.ValueKind == JsonValueKind.String && value.GetString() is { } color &&
                color.Length is 7 or 9 && color[0] == '#' && color.AsSpan(1).IndexOfAnyExcept("0123456789abcdefABCDEF") < 0,
            "image" => value.ValueKind == JsonValueKind.String && (value.GetString() == "" || Id(value.GetString()!)),
            "number" => value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var number) && double.IsFinite(number) &&
                number >= (field.Minimum ?? -1e9) && number <= (field.Maximum ?? 1e9),
            "boolean" => value.ValueKind is JsonValueKind.True or JsonValueKind.False,
            _ => false
        };
    }
    private static bool ValidAnimations(JsonElement animations)
    {
        if (animations.ValueKind != JsonValueKind.Object) return false;
        var phases = new HashSet<string>(StringComparer.Ordinal);
        foreach (var phase in animations.EnumerateObject())
        {
            if (phase.Name is not ("in" or "out") || !phases.Add(phase.Name) || phase.Value.ValueKind != JsonValueKind.Object) return false;
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var item in phase.Value.EnumerateObject())
            {
                if (!names.Add(item.Name)) return false;
                if (item.Name == "durationMs")
                {
                    if (!item.Value.TryGetDouble(out var ms) || !double.IsFinite(ms) || ms < 0 || ms > 3000) return false;
                }
                else
                {
                    if (item.Value.ValueKind != JsonValueKind.String) return false;
                    var text = item.Value.GetString();
                    if (item.Name == "type") { if (text is not ("none" or "fade" or "slide")) return false; }
                    else if (item.Name == "direction") { if (text is not ("left" or "right" or "up" or "down")) return false; }
                    else if (item.Name == "easing") { if (text is not ("linear" or "ease-in" or "ease-out" or "ease-in-out")) return false; }
                    else return false;
                }
            }
        }
        return true;
    }
    private static double Number(JsonElement value, string key) => value.TryGetProperty(key, out var item) && item.ValueKind == JsonValueKind.Number && item.TryGetDouble(out var number) ? number : -1;
    private static bool Integer(JsonElement value, double minimum) => value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var number) &&
        double.IsFinite(number) && Math.Truncate(number) == number && number >= minimum && number <= 9007199254740991d;
    private static bool Id(string value) => value.Length is > 0 and <= 64 && value[0] is >= 'a' and <= 'z' &&
        value.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '-');
    private static bool Template(string value) => value is "compact-solid" or "minimal-accent" or "broadcast";
}
