using System.Diagnostics;
using System.IO.Pipes;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed record EngineHostClientOptions(string Executable, string NativeCore, string Profile,
    IReadOnlyList<string>? NativeArgs = null);

public sealed class EngineHostClient(EngineHostClientOptions options) : IAsyncDisposable
{
    private readonly SemaphoreSlim _lane = new(1, 1);
    private readonly string _client = Guid.NewGuid().ToString("N");
    private string? _epoch;
    private long _fence, _revision, _sequence;
    public bool Running { get; private set; }
    public bool ReadOnly { get; private set; } = true;
    public string? Epoch => _epoch;
    public int CorePid { get; private set; }
    public event Action<string>? Resource;
    public NativeMediaCoreProfile? Profile { get; private set; }

    public async Task StartAsync(int bufferFrames, CancellationToken cancellationToken)
    {
        await _lane.WaitAsync(cancellationToken);
        try
        {
            if (Running) return;
            var request = new EngineHostRequest("attach", _client);
            EngineHostReply reply;
            try { reply = await ExchangeAsync(request, 250, cancellationToken); }
            catch (TimeoutException)
            {
                var start = new ProcessStartInfo(Path.GetFullPath(options.Executable))
                { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
                start.ArgumentList.Add("--profile"); start.ArgumentList.Add(options.Profile);
                start.ArgumentList.Add("--core"); start.ArgumentList.Add(Path.GetFullPath(options.NativeCore));
                start.ArgumentList.Add("--buffer"); start.ArgumentList.Add(bufferFrames.ToString());
                foreach (var arg in options.NativeArgs ?? [])
                { start.ArgumentList.Add("--core-arg"); start.ArgumentList.Add(arg); }
                // Host uses its local supervisor, never recursively attaches to itself.
                start.Environment.Remove("COREVIDEO_ENGINE_HOST");
                using var launched = Process.Start(start) ?? throw new IOException("Could not launch EngineHost.");
                reply = await ExchangeAsync(request, 20000, cancellationToken);
            }
            Accept(reply);
            Profile = reply.Payload?.Deserialize<NativeMediaCoreProfile>();
            Running = true;
        }
        finally { _lane.Release(); }
    }

    public async Task<JsonDocument> SendAsync(Dictionary<string, object?> payload, CancellationToken cancellationToken)
    {
        await _lane.WaitAsync(cancellationToken);
        try
        {
            if (!Running) throw new InvalidOperationException("Engine host is detached.");
            var element = JsonSerializer.SerializeToElement(payload, MediaCoreJson.Options);
            if (ReadOnly && !EngineHostWire.IsObservation(element))
                throw new InvalidOperationException("Attached to the running show read-only. Explicit host takeover is required; startup state was not applied.");
            var reply = await ExchangeAsync(new("command", _client, _epoch, _fence, _revision,
                (++_sequence).ToString(), element), 2000, cancellationToken);
            Accept(reply);
            return JsonDocument.Parse(reply.Payload?.GetRawText() ?? "{}");
        }
        finally { _lane.Release(); }
    }

    // For explicit tooling/controlled clients. WinUI stays observation-only on
    // reattach until #961 can reconstruct the complete editable show model.
    public async Task ControlAsync(string operation, CancellationToken cancellationToken)
    {
        if (operation is not ("take-control" or "shutdown" or "detach")) throw new ArgumentException(nameof(operation));
        await _lane.WaitAsync(cancellationToken);
        try
        {
            Accept(await ExchangeAsync(new(operation, _client, _epoch, _fence, _revision), 2000, cancellationToken));
            if (operation is "shutdown" or "detach") Running = false;
        }
        finally { _lane.Release(); }
    }

    private void Accept(EngineHostReply reply)
    {
        if (reply.Version != EngineHostWire.Version) throw new IOException("Incompatible EngineHost protocol.");
        _epoch = reply.Epoch; _fence = reply.Fence; _revision = reply.Revision;
        ReadOnly = reply.ReadOnly; CorePid = reply.CorePid;
        if (!reply.Ok) throw new InvalidOperationException(reply.Error ?? "EngineHost rejected the request.");
        foreach (var resource in reply.Events ?? []) Resource?.Invoke(resource);
    }

    private async Task<EngineHostReply> ExchangeAsync(EngineHostRequest request, int connectMs, CancellationToken cancellationToken)
    {
        using var pipe = new NamedPipeClientStream(".", EngineHostEndpoint.Name(options.Profile),
            PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        await pipe.ConnectAsync(connectMs, cancellationToken);
        EngineHostEndpoint.VerifyServer(pipe, options.Executable);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(TimeSpan.FromSeconds(70));
        await EngineHostWire.WriteAsync(pipe, request, deadline.Token);
        return await EngineHostWire.ReadAsync<EngineHostReply>(pipe, deadline.Token);
    }

    public async ValueTask DisposeAsync()
    {
        if (!Running) return;
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        try { await ControlAsync("detach", deadline.Token); }
        catch (Exception ex) when (ex is IOException or OperationCanceledException or TimeoutException or InvalidOperationException) { }
        Running = false;
    }
}
