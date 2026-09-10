using System.Text.Json;

namespace CoreVideoPro.Control.Http;

public sealed record HttpControlResponse(int Status, string ContentType, string Body)
{
    public static HttpControlResponse Json(int status, string body) => new(status, "application/json; charset=utf-8", body);
}

/// <summary>Transport-independent HTTP request handler: turns a (method, path, body) into a
/// response, invoking the <see cref="IControlSurface"/> for POST /invoke. Unit-testable without a
/// real <see cref="System.Net.HttpListener"/>. The socket + WebSocket wrapper is
/// <see cref="HttpControlServer"/>.
///
/// Routes:
///   GET  /manifest        → the full action/feedback contract (ControlManifest JSON)
///   GET  /state           → the current ControlState (camelCase JSON)
///   GET  /snapshot        → the last media-core session snapshot, verbatim (see below)
///   POST /invoke          → { "action": "&lt;id&gt;", "args": [ ... ] } → { "ok": bool, "error"?: string }
///
/// <c>GET /snapshot</c> is read-only observability: it serves the snapshot the shell ALREADY
/// received on its existing cadence, so an external judge can watch the core the operator is
/// actually running instead of spawning its own. It never triggers a core round-trip. It is
/// answered only when the surface implements <see cref="INativeSnapshotObserver"/>, and it sits
/// behind exactly the same authorization as every other route (loopback by default; a LAN bind
/// refuses to start without a token — see <c>HttpControlServerOptions.Validate</c>).
/// </summary>
public sealed class HttpControlRouter
{
    internal static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true
    };

    private readonly IControlSurface _surface;
    private readonly ControlCatalog _catalog;

    public HttpControlRouter(IControlSurface surface, ControlCatalog? catalog = null)
    {
        _surface = surface;
        _catalog = catalog ?? ControlCatalog.StaticOnly;
    }

    public async Task<HttpControlResponse> HandleAsync(string method, string path, string? body, CancellationToken cancellationToken = default)
    {
        var route = NormalizePath(path);

        if (method == "GET" && route == "/manifest")
        {
            return HttpControlResponse.Json(200, ControlManifest.Build(catalog: _catalog).ToJson());
        }

        if (method == "GET" && route == "/state")
        {
            return HttpControlResponse.Json(200, JsonSerializer.Serialize(_surface.GetState(), JsonOptions));
        }

        if (method == "GET" && route == "/snapshot")
        {
            return HttpControlResponse.Json(200, BuildSnapshotEnvelope(DateTimeOffset.UtcNow));
        }

        if (method == "POST" && route == "/invoke")
        {
            return await HandleInvokeAsync(body, cancellationToken).ConfigureAwait(false);
        }

        return HttpControlResponse.Json(404, "{\"ok\":false,\"error\":\"Not found\"}");
    }

    /// <summary>How old the shell's last snapshot may be before the response marks it stale. The
    /// shell syncs the core several times a second, so anything beyond this means the pipeline
    /// stopped feeding the shell — the consumer must not read it as current.</summary>
    public static readonly TimeSpan SnapshotStaleAfter = TimeSpan.FromMilliseconds(2000);

    /// <summary>Serves the shell's most recent core snapshot inside an envelope that always
    /// states availability and age. The snapshot itself is written through verbatim — no
    /// re-shaping, no field picking — so a consumer sees what the core said.</summary>
    private string BuildSnapshotEnvelope(DateTimeOffset servedUtc)
    {
        if (_surface is not INativeSnapshotObserver observer)
        {
            return SnapshotUnavailable(servedUtc, "not-observed",
                "This control surface does not observe a media core.");
        }

        NativeSnapshotObservation observation;
        try
        {
            observation = observer.GetNativeSnapshot();
        }
        catch (Exception error)
        {
            return SnapshotUnavailable(servedUtc, "observer-failed", error.Message);
        }

        if (string.IsNullOrWhiteSpace(observation.Json))
        {
            return SnapshotUnavailable(servedUtc, "no-snapshot",
                observation.UnavailableReason ?? "The shell has not received a core snapshot yet.");
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(observation.Json!);
        }
        catch (JsonException error)
        {
            return SnapshotUnavailable(servedUtc, "unparseable", error.Message);
        }

        using (document)
        {
            var root = document.RootElement;
            var received = observation.ReceivedUtc;
            var ageMs = received is null ? (double?)null : (servedUtc - received.Value).TotalMilliseconds;

            using var stream = new MemoryStream();
            using (var writer = new Utf8JsonWriter(stream))
            {
                writer.WriteStartObject();
                writer.WriteBoolean("ok", true);
                writer.WriteBoolean("available", true);
                writer.WriteString("servedUtc", servedUtc);
                if (received is null)
                {
                    // A snapshot with no receipt time cannot be aged; say so rather than
                    // implying freshness by omission.
                    writer.WriteNull("receivedUtc");
                    writer.WriteNull("ageMs");
                    writer.WriteBoolean("stale", true);
                    writer.WriteString("staleReason", "The shell did not record when this snapshot arrived.");
                }
                else
                {
                    writer.WriteString("receivedUtc", received.Value);
                    writer.WriteNumber("ageMs", Math.Round(ageMs!.Value, 3));
                    writer.WriteBoolean("stale", ageMs.Value > SnapshotStaleAfter.TotalMilliseconds);
                }

                writer.WriteNumber("staleAfterMs", SnapshotStaleAfter.TotalMilliseconds);
                writer.WriteBoolean("redacted", true);
                writer.WriteStartArray("nodes");
                if (root.ValueKind == JsonValueKind.Object)
                {
                    foreach (var property in root.EnumerateObject())
                    {
                        writer.WriteStringValue(property.Name);
                    }
                }

                writer.WriteEndArray();
                writer.WritePropertyName("snapshot");
                root.WriteTo(writer);
                writer.WriteEndObject();
            }

            return System.Text.Encoding.UTF8.GetString(stream.ToArray());
        }
    }

    private static string SnapshotUnavailable(DateTimeOffset servedUtc, string reasonCode, string reason) =>
        JsonSerializer.Serialize(new
        {
            ok = true,
            available = false,
            servedUtc,
            receivedUtc = (DateTimeOffset?)null,
            ageMs = (double?)null,
            stale = true,
            staleAfterMs = SnapshotStaleAfter.TotalMilliseconds,
            reasonCode,
            reason,
            nodes = Array.Empty<string>(),
            snapshot = (object?)null
        }, JsonOptions);

    private async Task<HttpControlResponse> HandleInvokeAsync(string? body, CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(body))
        {
            return Fail(400, "Request body is required.");
        }

        string? actionId;
        List<object?> args;
        try
        {
            using var doc = JsonDocument.Parse(body);
            var root = doc.RootElement;
            actionId = root.TryGetProperty("action", out var actionEl) && actionEl.ValueKind == JsonValueKind.String
                ? actionEl.GetString()
                : null;
            args = new List<object?>();
            if (root.TryGetProperty("args", out var argsEl) && argsEl.ValueKind == JsonValueKind.Array)
            {
                foreach (var element in argsEl.EnumerateArray())
                {
                    args.Add(JsonElementToValue(element));
                }
            }
        }
        catch (JsonException ex)
        {
            return Fail(400, $"Invalid JSON: {ex.Message}");
        }

        if (string.IsNullOrWhiteSpace(actionId))
        {
            return Fail(400, "Missing 'action'.");
        }

        if (!_catalog.TryBind(actionId!, args, out var bound, out var bindError))
        {
            return Fail(400, bindError!);
        }

        var result = await _surface.InvokeAsync(actionId!, bound, cancellationToken).ConfigureAwait(false);
        if (!result.Ok)
        {
            return Fail(422, result.Error ?? "Invocation failed.");
        }

        return HttpControlResponse.Json(200, "{\"ok\":true}");
    }

    private static object? JsonElementToValue(JsonElement element) => element.ValueKind switch
    {
        JsonValueKind.String => element.GetString(),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        JsonValueKind.Number => element.TryGetInt64(out var l) ? l : element.GetDouble(),
        JsonValueKind.Null => null,
        _ => element.GetRawText()
    };

    private static HttpControlResponse Fail(int status, string error) =>
        HttpControlResponse.Json(status, JsonSerializer.Serialize(new { ok = false, error }, JsonOptions));

    private static string NormalizePath(string path)
    {
        var trimmed = (path ?? "/").Split('?')[0].TrimEnd('/');
        return trimmed.Length == 0 ? "/" : trimmed;
    }
}
