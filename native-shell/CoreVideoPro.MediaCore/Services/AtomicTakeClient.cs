using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record AtomicTakeClientCapability(bool LocallyEnabled = false, int AdvertisedVersion = 0, bool PeerEnabled = false)
{
    public bool Available => LocallyEnabled && PeerEnabled && AdvertisedVersion == 1;
}

public sealed record AtomicTakeClientReply(string Error, NativeTakeOutcome? Outcome = null,
    bool Replayed = false, bool Retryable = false, bool ReconcileRequired = false);

// Standalone adapter: no bridge registration, UI Take replacement, or automatic retry.
// The transport must preserve its own process-generation fence. A lost response is
// reconciled using the original request and operation ID, never a newly minted ID.
public sealed class AtomicTakeClient
{
    private readonly Func<string, CancellationToken, Task<string>> _transport;
    private readonly AtomicTakeClientCapability _capability;
    public AtomicTakeClient(Func<string, CancellationToken, Task<string>> transport,
        AtomicTakeClientCapability? capability = null)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
        _capability = capability ?? new();
    }
    public async Task<AtomicTakeClientReply> TakeAsync(NativeTakeRequest request, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (!_capability.Available) return new("capability-disabled");
        cancellationToken.ThrowIfCancellationRequested();
        var payload = request.Serialize();
        try
        {
            var response = await _transport(payload, cancellationToken).ConfigureAwait(false);
            return AtomicTakeReplyCodec.Decode(response, request);
        }
        catch (Exception ex) when (ex is not (OutOfMemoryException or StackOverflowException or AccessViolationException))
        {
            return new("transport-unknown", ReconcileRequired: true);
        }
    }
}

public static class AtomicTakeReplyCodec
{
    private static readonly HashSet<string> OutcomeErrors = ["none", "invalid", "authorityEpoch", "operationConflict",
        "operationExpired", "capacity", "staleRevision", "stalePreview", "notPrepared", "busy",
        "revisionExhausted", "applyFailed", "notApplied", "staleObservation"];
    public static AtomicTakeClientReply Decode(string json, NativeTakeRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        try
        {
            if (json is null || json.Length > 64 * 1024) return Unknown();
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            UniqueObject(root);
            if (root.GetProperty("type").GetString() != "take-result") return Unknown();
            var ok = root.GetProperty("ok").GetBoolean();
            var reconcile = root.GetProperty("reconcileRequired").GetBoolean();
            if (!root.TryGetProperty("outcome", out var raw))
            {
                var error = root.GetProperty("error").GetString();
                if (ok || error is not ("capability-disabled" or "invalid-request" or "outcome-unavailable")) return Unknown();
                if (reconcile != (error == "outcome-unavailable")) return Unknown();
                return new(error, ReconcileRequired: reconcile);
            }
            UniqueObject(raw);
            // Explicit required booleans prevent omitted evidence becoming false defaults.
            foreach (var field in new[] { "pending", "accepted", "applied", "rendered", "delivered" })
                _ = raw.GetProperty(field).GetBoolean();
            _ = raw.GetProperty("resultRevision").GetInt64();
            foreach (var field in new[] { "authorityEpoch", "operationId", "error", "failure" })
                if (raw.GetProperty(field).ValueKind != JsonValueKind.String) return Unknown();
            var outcome = JsonSerializer.Deserialize<NativeTakeOutcome>(raw.GetRawText())!;
            var replayed = root.GetProperty("replayed").GetBoolean();
            var retryable = root.GetProperty("retryable").GetBoolean();
            if (!OutcomeErrors.Contains(outcome.Error) || outcome.AuthorityEpoch != request.AuthorityEpoch ||
                outcome.OperationId != request.OperationId || ok != (outcome.Error == "none") ||
                reconcile != outcome.Pending || (retryable && outcome.Error != "busy") ||
                (outcome.Error == "none" && (!outcome.Accepted || (!outcome.Pending && !outcome.Applied))) ||
                (outcome.Pending && outcome.Error != "none") ||
                (outcome.Accepted && outcome.Error != "none" && outcome.Error != "applyFailed") ||
                (outcome.Accepted && outcome.ResultRevision != request.Fingerprint.ExpectedRevision +
                    (outcome.Error == "applyFailed" ? 0 : 1))) return Unknown();
            return new(outcome.Error, outcome, replayed, retryable, reconcile);
        }
        catch (Exception ex) when (ex is JsonException or ArgumentException or InvalidOperationException or KeyNotFoundException or FormatException or OverflowException)
        { return Unknown(); }
    }
    private static AtomicTakeClientReply Unknown() => new("invalid-response", ReconcileRequired: true);
    private static void UniqueObject(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object) throw new JsonException();
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var property in value.EnumerateObject()) if (!names.Add(property.Name)) throw new JsonException();
    }
}
