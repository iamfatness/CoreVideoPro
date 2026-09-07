using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.ShowEngine;

/// <summary>
/// The C# half of the JSON-line wire in <c>show-engine/src/host/protocol.ts</c>. Pure and static: no
/// I/O, no state, no threading — everything here is safe to call from the reader loop.
///
/// The shapes it decodes, verbatim from the TypeScript host:
/// <list type="bullet">
/// <item>response — <c>{id, ok:true, ...}</c> or <c>{id, ok:false, error:{message}}</c>; an
///   <c>invoke</c> response also carries <c>result</c>, a <c>ping</c> response carries
///   <c>revision</c>. <c>id</c> may be <c>null</c> (a line so malformed the host could not echo one).</item>
/// <item>event — <c>{event:"handshake"|"snapshot"|"hostCommand"|"log", ...}</c>. The startup handshake
///   is an EVENT with no id; the host ALSO answers an explicit <c>handshake</c> request with
///   <c>{id, ok:true}</c> followed by that same event, which is why
///   <see cref="TryParseHandshake"/> accepts either envelope.</item>
/// </list>
///
/// Unknown <c>event</c> names are <see cref="LineKind.Unknown"/>, not
/// <see cref="LineKind.Malformed"/>: a newer engine adding an event must not make an older shell log
/// warnings on every line of a healthy show.
/// </summary>
public static class ShowEngineProtocol
{
    /// <summary>The one protocol version this shell speaks (<c>PROTOCOL_VERSION</c> in protocol.ts).</summary>
    public const int SupportedProtocolVersion = 1;

    /// <summary>Matches <c>HttpControlRouter.JsonOptions</c>: camelCase out, case-insensitive in,
    /// nulls omitted (so an optional request field simply does not appear on the wire).</summary>
    public static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    private static readonly IReadOnlyList<ShowEngineActionParam> NoParams = Array.Empty<ShowEngineActionParam>();

    /// <summary>
    /// Build one request line: <c>{"id":…,"type":…, &lt;payload's public properties merged at top
    /// level&gt;}</c>. The host's <c>decodeRequest</c> reads <c>action</c>/<c>args</c>/<c>event</c>/
    /// <c>participantId</c>/<c>capacity</c> as TOP-LEVEL fields, never nested under a wrapper, so the
    /// payload is flattened rather than written as a property. Never contains a raw newline (JSON
    /// escapes them), which is what keeps one message on one line.
    /// </summary>
    public static string EncodeRequest(string id, string type, object? payload = null)
    {
        using var buffer = new MemoryStream(256);
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteString("id", id);
            writer.WriteString("type", type);

            if (payload is not null)
            {
                using var doc = JsonSerializer.SerializeToDocument(payload, payload.GetType(), Json);
                if (doc.RootElement.ValueKind == JsonValueKind.Object)
                {
                    foreach (var property in doc.RootElement.EnumerateObject())
                    {
                        // id/type are the envelope's own; a payload may never overwrite them.
                        if (property.NameEquals("id") || property.NameEquals("type")) continue;
                        property.WriteTo(writer);
                    }
                }
            }

            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(buffer.GetBuffer(), 0, (int)buffer.Length);
    }

    public enum LineKind
    {
        Response,
        Handshake,
        Snapshot,
        HostCommand,
        Log,
        Unknown,
        Malformed
    }

    /// <summary>Name a parsed stdout line. A line that would not parse at all never reaches here —
    /// the reader loop catches <see cref="JsonException"/> and treats it as
    /// <see cref="LineKind.Malformed"/> itself.</summary>
    public static LineKind Classify(JsonDocument doc)
    {
        var root = doc.RootElement;
        if (root.ValueKind != JsonValueKind.Object) return LineKind.Malformed;

        if (root.TryGetProperty("ok", out var ok) &&
            (ok.ValueKind == JsonValueKind.True || ok.ValueKind == JsonValueKind.False))
        {
            return LineKind.Response;
        }

        if (!root.TryGetProperty("event", out var evt) || evt.ValueKind != JsonValueKind.String)
        {
            return LineKind.Unknown;
        }

        return evt.GetString() switch
        {
            "handshake" => LineKind.Handshake,
            "snapshot" => LineKind.Snapshot,
            "hostCommand" => LineKind.HostCommand,
            "log" => LineKind.Log,
            _ => LineKind.Unknown
        };
    }

    /// <summary>Parse a handshake payload from either envelope (the startup event, or a response that
    /// carries the same fields). Returns false with a reason rather than throwing: a malformed
    /// handshake must fail the supervisor loudly, not crash the reader loop.</summary>
    public static bool TryParseHandshake(JsonElement root, out ShowEngineHandshake handshake, out string? error)
    {
        handshake = null!;
        error = null;

        if (root.ValueKind != JsonValueKind.Object)
        {
            error = "handshake is not a JSON object";
            return false;
        }

        if (!root.TryGetProperty("protocolVersion", out var version) ||
            version.ValueKind != JsonValueKind.Number ||
            !version.TryGetInt32(out var protocolVersion))
        {
            error = "handshake is missing a numeric 'protocolVersion'";
            return false;
        }

        var actions = new List<ShowEngineActionDefinition>();
        if (root.TryGetProperty("actions", out var actionsElement) &&
            actionsElement.ValueKind == JsonValueKind.Array)
        {
            foreach (var action in actionsElement.EnumerateArray())
            {
                if (action.ValueKind != JsonValueKind.Object) continue;
                actions.Add(new ShowEngineActionDefinition(
                    String(action, "id"),
                    String(action, "title"),
                    String(action, "description"),
                    ParseParams(action)));
            }
        }

        var templates = new List<string>();
        if (root.TryGetProperty("fieldTemplates", out var templatesElement) &&
            templatesElement.ValueKind == JsonValueKind.Array)
        {
            foreach (var template in templatesElement.EnumerateArray())
            {
                if (template.ValueKind == JsonValueKind.String) templates.Add(template.GetString()!);
            }
        }

        handshake = new ShowEngineHandshake(
            protocolVersion,
            String(root, "engineVersion"),
            Int32(root, "generation"),
            actions,
            templates,
            Clone(root, "snapshot"),
            ParseFields(root));
        return true;
    }

    /// <summary>Read a <c>snapshot</c> event. THROWS <see cref="FormatException"/> when the line
    /// carries no <c>snapshot</c> node: <see cref="Clone"/> would otherwise hand back
    /// <c>default(JsonElement)</c> (ValueKind <c>Undefined</c>), which survives all the way to
    /// <c>ControlState.Ohg</c> and only then throws — inside the control server's JSON writer, on a
    /// thread with no idea what produced it. A malformed line must be named where it is read; the
    /// reader loop logs it exactly like a line that failed to parse at all.</summary>
    public static ShowEngineSnapshot ParseSnapshot(JsonElement root) => new(
        Int32(root, "generation"),
        Int64(root, "revision"),
        RequiredNode(root, "snapshot"),
        ParseFields(root));

    public static ShowEngineHostCommand ParseHostCommand(JsonElement root) => new(
        Int32(root, "generation"),
        Int64(root, "seq"),
        String(root, "name"),
        Clone(root, "args"));

    public static ShowEngineLogLine ParseLog(JsonElement root) => new(
        String(root, "level", fallback: "info"),
        String(root, "message"));

    /// <summary>Read the <c>result</c> node of an <c>invoke</c> response. A response with no
    /// <c>result</c> is reported as an error rather than silently as success — a host that answered
    /// <c>ok</c> without a result did not run the action.</summary>
    public static ShowEngineActionResult ParseActionResult(JsonElement responseRoot)
    {
        if (responseRoot.ValueKind != JsonValueKind.Object ||
            !responseRoot.TryGetProperty("result", out var result) ||
            result.ValueKind != JsonValueKind.Object)
        {
            return new ShowEngineActionResult("error", null, "show engine response carried no 'result'");
        }

        var kind = String(result, "kind", fallback: "error");
        return kind switch
        {
            "ok" => new ShowEngineActionResult("ok", null, null),
            "refused" => new ShowEngineActionResult("refused", String(result, "reason"), null),
            "error" => new ShowEngineActionResult("error", null, String(result, "message")),
            _ => new ShowEngineActionResult("error", null, $"unknown action result kind '{kind}'")
        };
    }

    // ------------------------------------------------------------------ helpers

    private static IReadOnlyList<ShowEngineActionParam> ParseParams(JsonElement action)
    {
        if (!action.TryGetProperty("params", out var parameters) || parameters.ValueKind != JsonValueKind.Array)
        {
            return NoParams;
        }

        var list = new List<ShowEngineActionParam>();
        foreach (var parameter in parameters.EnumerateArray())
        {
            if (parameter.ValueKind != JsonValueKind.Object) continue;
            list.Add(new ShowEngineActionParam(
                String(parameter, "name"),
                String(parameter, "type", fallback: "string"),
                parameter.TryGetProperty("required", out var required) && required.ValueKind == JsonValueKind.True,
                String(parameter, "description")));
        }

        return list.Count == 0 ? NoParams : list;
    }

    private static IReadOnlyDictionary<string, JsonElement> ParseFields(JsonElement root)
    {
        var fields = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (root.TryGetProperty("fields", out var element) && element.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in element.EnumerateObject())
            {
                fields[property.Name] = property.Value.Clone();
            }
        }

        return fields;
    }

    /// <summary>Detach a child node from its owning <see cref="JsonDocument"/> so it stays readable
    /// after the reader loop disposes the line's document.</summary>
    private static JsonElement Clone(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) ? value.Clone() : default;

    /// <summary>Like <see cref="Clone"/>, but a missing (or explicitly Undefined) node is a
    /// <see cref="FormatException"/> rather than a silent <c>default(JsonElement)</c>.</summary>
    private static JsonElement RequiredNode(JsonElement root, string name)
    {
        if (!root.TryGetProperty(name, out var value) || value.ValueKind == JsonValueKind.Undefined)
        {
            throw new FormatException($"show engine line carried no '{name}'");
        }

        return value.Clone();
    }

    private static string String(JsonElement root, string name, string fallback = "") =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? fallback
            : fallback;

    private static int Int32(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var parsed)
            ? parsed
            : 0;

    private static long Int64(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt64(out var parsed)
            ? parsed
            : 0;
}
