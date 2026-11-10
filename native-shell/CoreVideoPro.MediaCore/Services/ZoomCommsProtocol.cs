using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Comms (Zoom talkback) wire format between the shell and the media core
/// (docs/reference/comms-talkback.md). Requests are ordinary core requests whose
/// <c>type</c> is one of <see cref="Verbs"/> with the arguments in <c>payload</c>;
/// the core answers ok or <c>comms-refused</c> with operator text. State arrives
/// unsolicited as <c>{"type":"zoom-comms-state","comms":{...}|null}</c>.
/// The verbs and the state object are CoreVideo Edge's (docs/PROTOCOL.md "Comms").
/// </summary>
public static class ZoomCommsProtocol
{
    public static readonly IReadOnlySet<string> Verbs = new HashSet<string>(StringComparer.Ordinal)
    {
        "comms-key", "comms-latch", "comms-assign", "comms-mic",
        "comms-sidetone-out", "comms-sidetone", "comms-tone", "comms-gain", "comms-release-all", "comms-private",
    };

    public static bool TryParseStateLine(string line, out ZoomCommsUpdate update)
    {
        update = new ZoomCommsUpdate(null);
        // Cheap reject first: the same stdout carries large thumbnails.
        if (line.Length > 1_048_576 || !line.Contains("\"zoom-comms-state\"", StringComparison.Ordinal)) return false;
        try
        {
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || root.TryGetProperty("id", out _) ||
                !root.TryGetProperty("type", out var type) || type.ValueKind != JsonValueKind.String ||
                type.GetString() != "zoom-comms-state" || !root.TryGetProperty("comms", out var comms))
                return false;
            if (comms.ValueKind == JsonValueKind.Null) return true;
            if (comms.ValueKind != JsonValueKind.Object) return false;
            update = new ZoomCommsUpdate(ParseState(comms));
            return true;
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or FormatException)
        {
            return false;
        }
    }

    public static ZoomCommsState ParseState(JsonElement cm)
    {
        var lamps = Obj(cm, "lamps");
        var mic = Obj(cm, "mic");
        var stats = Obj(cm, "stats");
        var channels = cm.TryGetProperty("channels", out var ca) && ca.ValueKind == JsonValueKind.Array
            ? ca.EnumerateArray().Where(c => c.ValueKind == JsonValueKind.Object).Select(c => new ZoomCommsChannel(
                (int)L(c, "slot"), SN(c, "label"), (int)L(c, "listeners"), B(c, "ready"), B(c, "keyed"), B(c, "latched"),
                SN(c, "room"), (int)L(c, "reachOk"), (int)L(c, "reachDark"))).ToList()
            : new List<ZoomCommsChannel>();
        var roster = cm.TryGetProperty("roster", out var ra) && ra.ValueKind == JsonValueKind.Array
            ? ra.EnumerateArray().Where(r => r.ValueKind == JsonValueKind.Object).Select(r => new ZoomCommsPerson(
                (uint)Math.Max(0, L(r, "participantId")), SN(r, "name") ?? "", B(r, "talkback"), SN(r, "room"),
                r.TryGetProperty("channels", out var rc) && rc.ValueKind == JsonValueKind.Array
                    ? rc.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.Number).Select(x => (int)x.GetDouble()).ToList()
                    : new List<int>())).ToList()
            : new List<ZoomCommsPerson>();
        var hasMic = mic.ValueKind == JsonValueKind.Object;
        var hasStats = stats.ValueKind == JsonValueKind.Object;
        return new ZoomCommsState(
            B(cm, "available"), SN(cm, "reason"),
            B(lamps, "link"), B(lamps, "meetingMic"), B(lamps, "channel"), B(lamps, "tx"),
            SN(cm, "room"), channels, roster,
            new ZoomCommsMic(
                hasMic ? SN(mic, "device") : null, hasMic ? Strings(mic, "devices") : [],
                hasMic ? DN(mic, "peakDbfs") : null, hasMic ? (int)L(mic, "gainDb") : 0, B(mic, "sidetone"),
                hasMic ? SN(mic, "sidetoneDevice") : null, hasMic ? Strings(mic, "sidetoneDevices") : [], B(mic, "tone")),
            hasStats ? L(stats, "framesSent") : 0, hasStats ? L(stats, "channelSends") : 0,
            hasStats ? L(stats, "underruns") : 0, hasStats ? L(stats, "sendErrors") : 0,
            Strings(cm, "log"),
            B(cm, "private"));
    }

    /// <summary>The core's reply to a comms-* request.</summary>
    public static ZoomCommsCommandResult ParseReply(JsonDocument response)
    {
        var root = response.RootElement;
        if (root.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True) return ZoomCommsCommandResult.Success;
        if (root.TryGetProperty("error", out var error))
        {
            if (error.ValueKind == JsonValueKind.Object && error.TryGetProperty("message", out var message) &&
                message.ValueKind == JsonValueKind.String && message.GetString() is { Length: > 0 } text)
                return new ZoomCommsCommandResult(false, text);
            if (error.ValueKind == JsonValueKind.String && error.GetString() is { Length: > 0 } plain)
                return new ZoomCommsCommandResult(false, plain);
        }
        return new ZoomCommsCommandResult(false, "The media core refused the Comms request.");
    }

    private static JsonElement Obj(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Object ? v : default;

    private static bool B(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;

    private static long L(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number
            ? (long)v.GetDouble() : 0;

    private static double? DN(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number
            ? v.GetDouble() : null;

    private static string? SN(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() : null;

    private static IReadOnlyList<string> Strings(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()!).ToList()
            : [];
}

/// <summary>The shell's seam for Comms: the state stream and the verbs. Implemented by
/// <see cref="MediaCoreBridgeService"/>; tests supply a fake.</summary>
public interface IZoomCommsTransport
{
    event Action<ZoomCommsUpdate>? ZoomCommsStateReceived;

    Task<ZoomCommsCommandResult> SendCommsAsync(string verb, IReadOnlyDictionary<string, object?> payload,
        CancellationToken cancellationToken = default);
}
