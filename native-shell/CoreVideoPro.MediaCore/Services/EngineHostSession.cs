using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

// All access is serialized by the server's state gate. Retained outcomes are
// bounded by count AND bytes; an expired ID is never automatically executed again.
public sealed class EngineHostSession
{
    public string Epoch { get; } = Guid.NewGuid().ToString("N");
    public long Revision { get; private set; }
    public long Fence { get; private set; }
    public string? Writer { get; private set; }
    private bool _everAttached;
    private readonly Dictionary<string, (string Fingerprint, JsonElement Result)> _results = new();
    private readonly Queue<string> _order = new();
    private int _resultBytes;
    public bool Attach(string client)
    {
        if (Writer == client) return false;
        if (_everAttached) return true; // Reattached console observes until explicit takeover.
        _everAttached = true;
        Writer = client;
        Fence++;
        return false;
    }
    public void TakeControl(string client) { Writer = client; Fence++; }
    public void ReserveMutation() { Revision++; }
    public void Detach(string client) { if (Writer == client) { Writer = null; Fence++; } }
    public string? Validate(EngineHostRequest request)
    {
        if (request.Epoch != Epoch) return "host-epoch-changed";
        if (request.ClientId != Writer || request.Fence != Fence) return "writer-fenced";
        if (request.Revision != Revision) return "revision-conflict";
        return null;
    }
    public bool TryResult(string client, string id, string fingerprint, out JsonElement result)
    {
        if (_results.TryGetValue(client + ":" + id, out var cached))
        {
            if (cached.Fingerprint != fingerprint) throw new InvalidOperationException("command-id-reused");
            result = cached.Result;
            return true;
        }
        result = default;
        return false;
    }
    public void Commit(string client, string id, string fingerprint, JsonElement result)
    {
        var bytes = result.GetRawText().Length * 2;
        while (_order.Count > 0 && (_order.Count >= 128 || _resultBytes + bytes > EngineHostWire.MaxBytes))
        {
            var key = _order.Dequeue();
            _resultBytes -= _results[key].Result.GetRawText().Length * 2;
            _results.Remove(key);
        }
        if (bytes > EngineHostWire.MaxBytes) return;
        var cacheKey = client + ":" + id;
        _results.Add(cacheKey, (fingerprint, result.Clone()));
        _order.Enqueue(cacheKey);
        _resultBytes += bytes;
    }
}
