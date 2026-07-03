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
///   POST /invoke          → { "action": "&lt;id&gt;", "args": [ ... ] } → { "ok": bool, "error"?: string }
/// </summary>
public sealed class HttpControlRouter
{
    internal static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true
    };

    private readonly IControlSurface _surface;

    public HttpControlRouter(IControlSurface surface)
    {
        _surface = surface;
    }

    public async Task<HttpControlResponse> HandleAsync(string method, string path, string? body, CancellationToken cancellationToken = default)
    {
        var route = NormalizePath(path);

        if (method == "GET" && route == "/manifest")
        {
            return HttpControlResponse.Json(200, ControlManifest.Build().ToJson());
        }

        if (method == "GET" && route == "/state")
        {
            return HttpControlResponse.Json(200, JsonSerializer.Serialize(_surface.GetState(), JsonOptions));
        }

        if (method == "POST" && route == "/invoke")
        {
            return await HandleInvokeAsync(body, cancellationToken).ConfigureAwait(false);
        }

        return HttpControlResponse.Json(404, "{\"ok\":false,\"error\":\"Not found\"}");
    }

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

        if (!ControlActionRegistry.TryBind(actionId!, args, out var bound, out var bindError))
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
