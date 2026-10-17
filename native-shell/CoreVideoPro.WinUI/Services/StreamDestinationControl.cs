using System.Text.Json;
using CoreVideoPro.Control;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Validates a complete destination edit before persistence or any bound setter runs.</summary>
internal static class StreamDestinationControl
{
    internal static ControlInvokeResult Configure(bool srt, IReadOnlyList<object?> args,
        bool busy, ProductionOutputPreferences current, Action<ProductionOutputPreferences> save,
        Action<ProductionOutputPreferences> apply)
    {
        if (busy) return ControlInvokeResult.Fail("Stop streaming and wait for the transition to finish before configuring destinations.");
        var action = srt ? "transport.stream.srt.configure" : "transport.stream.rtmp.configure";
        if (!ControlActionRegistry.TryBind(action, args, out var values, out _))
            return ControlInvokeResult.Fail("Destination arguments do not match the action schema; see GET /manifest.");

        // Never mutate the caller's captured preferences on validation or durable-write failure.
        var next = JsonSerializer.Deserialize<ProductionOutputPreferences>(JsonSerializer.Serialize(current))!;
        string Text(int index) => ((string?)values[index] ?? "").Trim();
        var enabled = (bool)values[0]!;
        string? error;
        if (srt)
        {
            next.StreamSrtEnabled = enabled;
            next.StreamSrtMode = Text(1).ToLowerInvariant();
            next.StreamSrtHost = Text(2);
            next.StreamSrtPort = Text(3);
            next.StreamSrtLatencyMs = Text(4);
            next.StreamSrtStreamId = Text(5);
            next.StreamSrtKeyLength = Text(6);
            next.StreamSrtPassphrase = values[7] is null ? current.StreamSrtPassphrase : Text(7);
            if (next.StreamSrtMode is not ("caller" or "listener" or "rendezvous"))
                return ControlInvokeResult.Fail("SRT mode must be caller, listener, or rendezvous.");
            if (next.StreamSrtHost.Length > 0 && Uri.CheckHostName(next.StreamSrtHost.Trim('[', ']')) == UriHostNameType.Unknown)
                return ControlInvokeResult.Fail("SRT host must be a hostname or IP address without a URL scheme or query.");
            error = enabled ? StudioStreamOutputValidation.ValidateSrt(next.StreamSrtMode, next.StreamSrtHost,
                next.StreamSrtPort, next.StreamSrtLatencyMs, next.StreamSrtStreamId,
                next.StreamSrtKeyLength, next.StreamSrtPassphrase) : null;
        }
        else
        {
            next.StreamRtmpEnabled = enabled;
            next.StreamRtmpProtocol = Text(1).ToLowerInvariant();
            next.StreamRtmpServerUrl = Text(2);
            next.StreamRtmpStreamKey = values[3] is null ? current.StreamRtmpStreamKey : Text(3);
            if (next.StreamRtmpProtocol is not ("rtmp" or "rtmps"))
                return ControlInvokeResult.Fail("RTMP protocol must be rtmp or rtmps.");
            if (Uri.TryCreate(StudioStreamOutputValidation.BuildRtmpUrl(next.StreamRtmpProtocol, next.StreamRtmpServerUrl), UriKind.Absolute, out var endpoint)
                && (endpoint.UserInfo.Length > 0 || endpoint.Query.Length > 0 || endpoint.Fragment.Length > 0))
                return ControlInvokeResult.Fail("Put RTMP credentials in streamKey; serverUrl cannot contain user info, a query, or a fragment.");
            error = enabled ? StudioStreamOutputValidation.ValidateRtmp(next.StreamRtmpProtocol,
                next.StreamRtmpServerUrl, next.StreamRtmpStreamKey) : null;
        }
        if (error is not null) return ControlInvokeResult.Fail(error);
        try { save(next); }
        catch { return ControlInvokeResult.Fail("Destination settings were not saved. Previous settings remain active; retry after resolving preference storage."); }
        try { apply(next); }
        catch { return ControlInvokeResult.Fail("Destination settings were saved but could not be applied. Retry configuration before starting streaming."); }
        return ControlInvokeResult.Success;
    }
}
