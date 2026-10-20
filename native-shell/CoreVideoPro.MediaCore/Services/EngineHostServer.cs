using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed class EngineHostServer(MediaCoreSupervisor supervisor, string endpoint)
{
    private readonly SemaphoreSlim _commands = new(1, 1);
    private readonly SemaphoreSlim _urgent = new(1, 1);
    private readonly object _state = new();
    private readonly EngineHostSession _session = new();
    private readonly long _started = Stopwatch.GetTimestamp();
    private readonly Dictionary<string, long> _sequences = new();
    private readonly HashSet<string> _clients = new();
    private readonly CancellationTokenSource _exit = new();
    private int _corePid = supervisor.HostedProcessId;
    private long _lastClientLog;
    private bool _stopping;

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _exit.Token);
        // Separate endpoint reserves admission even when all ordinary clients stop reading/writing.
        await Task.WhenAll(Enumerable.Range(0, 4).Select(_ => ListenAsync(false, lifetime.Token))
            .Concat(Enumerable.Range(0, 2).Select(_ => ListenAsync(true, lifetime.Token))));
    }

    private async Task ListenAsync(bool reserved, CancellationToken lifetime)
    {
        while (!lifetime.IsCancellationRequested)
        {
            using var pipe = new NamedPipeServerStream(endpoint + (reserved ? ".urgent" : ""), PipeDirection.InOut, reserved ? 2 : 4,
                PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            try
            {
                await pipe.WaitForConnectionAsync(lifetime);
                EngineHostEndpoint.VerifyClient(pipe);
                using var deadline = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
                deadline.CancelAfter(TimeSpan.FromSeconds(reserved ? 2 : 10));
                var request = await EngineHostWire.ReadAsync<EngineHostRequest>(pipe, deadline.Token,
                    reserved ? EngineHostWire.MaxUrgentBytes : EngineHostWire.MaxBytes);
                if (reserved && !EngineHostWire.UsesReservedLane(request))
                    throw new IOException("Ordinary work cannot use reserved admission.");
                var lane = reserved ? _urgent : _commands;
                // Limit queue wait; after admission client disconnect cannot cancel an action.
                if (!await lane.WaitAsync(TimeSpan.FromSeconds(2), deadline.Token))
                    throw new IOException("Engine host is busy; reconcile before retrying.");
                EngineHostReply reply;
                try
                {
                    Task<EngineHostReply> action;
                    lock (_state) action = HandleAsync(request, lifetime);
                    reply = await action;
                }
                catch (Exception ex) when (ex is InvalidOperationException or TimeoutException or IOException)
                { lock (_state) reply = Reply(request, false, error: "operation-outcome-unknown: " + ex.GetType().Name); }
                finally { lane.Release(); }
                // Output serialization/write never holds the media or command lane.
                using var writeDeadline = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
                writeDeadline.CancelAfter(TimeSpan.FromSeconds(2));
                await EngineHostWire.WriteAsync(pipe, reply, writeDeadline.Token);
                if (request.Operation == "shutdown" && reply.Ok) _exit.Cancel();
            }
            catch (Exception ex) when (ex is IOException or OperationCanceledException or JsonException or InvalidOperationException)
            {
                var now = Environment.TickCount64;
                if (now - Interlocked.Read(ref _lastClientLog) >= 1000)
                {
                    Interlocked.Exchange(ref _lastClientLog, now);
                    DiagnosticLog.Write("engine-host.log", "Client ended: " + ex.GetType().Name);
                }
            }
        }
    }

    private EngineHostReply Reply(EngineHostRequest request, bool ok, JsonElement? payload = null, string? error = null) =>
        new(ok, _session.Epoch, _session.Fence, _session.Revision,
            _session.Writer != request.ClientId, payload, error, supervisor.HostedProcessId,
            supervisor.HostedResourceEvents());

    private async Task<EngineHostReply> HandleAsync(EngineHostRequest request, CancellationToken lifetime)
    {
        if (_stopping) return Reply(request, false, error: "host-stopping");
        if (_corePid != supervisor.HostedProcessId)
        {
            _corePid = supervisor.HostedProcessId;
            if (_session.Writer is { } writer) _session.Detach(writer);
            _sequences.Clear();
            _session.ReserveMutation(); // Explicit reconciliation after core replacement.
        }
        if (request.Version != EngineHostWire.Version) return Reply(request, false, error: "incompatible-host-protocol");
        if (!Guid.TryParseExact(request.ClientId, "N", out _)) return Reply(request, false, error: "invalid-client-id");
        if (request.Operation == "attach")
        {
            if (!_clients.Contains(request.ClientId) && _clients.Count >= 64)
            {
                // Retire an observer, never the writer. A long-lived show must not
                // become impossible to reconnect to after 64 console incarnations.
                var retired = _clients.First(client => client != _session.Writer);
                _clients.Remove(retired);
                _sequences.Remove(retired);
            }
            _clients.Add(request.ClientId);
            _session.Attach(request.ClientId);
            return Reply(request, true, JsonSerializer.SerializeToElement(supervisor.Profile));
        }
        if (!_clients.Contains(request.ClientId) || request.Epoch != _session.Epoch)
            return Reply(request, false, error: "reattach-required");
        if (request.Operation == "detach")
        {
            _session.Detach(request.ClientId); _clients.Remove(request.ClientId); _sequences.Remove(request.ClientId);
            return Reply(request, true);
        }
        if (request.Operation == "take-control")
        {
            if (request.Revision != _session.Revision) return Reply(request, false, error: "revision-conflict");
            _session.TakeControl(request.ClientId);
            _sequences.Clear();
            return Reply(request, true);
        }
        if (request.Operation == "shutdown")
        {
            if (_session.Validate(request) is { } denied) return Reply(request, false, error: denied);
            // Explicit administrative action only, never called on client disposal.
            _stopping = true;
            await Task.Run(() => supervisor.StopForAppExit(TimeSpan.FromSeconds(10)), lifetime);
            // Shutdown is committed independently of the client reading its ACK.
            _exit.CancelAfter(TimeSpan.FromSeconds(3));
            lock (_state) return Reply(request, true);
        }
        if (request.Operation != "command" || request.Payload is not { ValueKind: JsonValueKind.Object } payload)
            return Reply(request, false, error: "invalid-operation");
        var observation = EngineHostWire.IsObservation(payload);
        var fingerprint = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(payload.GetRawText())));
        if (!observation)
        {
            if (request.CommandId is null || !long.TryParse(request.CommandId, out var sequence) || sequence <= 0)
                return Reply(request, false, error: "invalid-command-id");
            // Result identity includes the writer incarnation. Old commands cannot
            // become executable again after client retirement or explicit takeover.
            if (request.ClientId != _session.Writer || request.Fence != _session.Fence)
                return Reply(request, false, error: "writer-fenced");
            if (_session.TryResult(request.ClientId + ":" + request.Fence, request.CommandId, fingerprint, out var cached))
                return Reply(request, true, cached);
            if (sequence <= _sequences.GetValueOrDefault(request.ClientId))
                return Reply(request, false, error: "command-outcome-unknown");
            if (_session.Validate(request) is { } denied) return Reply(request, false, error: denied);
            // Reserve before forwarding. A timeout must never make an ID reusable.
            _sequences[request.ClientId] = sequence;
            _session.ReserveMutation();
        }
        var command = JsonSerializer.Deserialize<Dictionary<string, object?>>(payload.GetRawText())!;
        command["elapsedMs"] = Stopwatch.GetElapsedTime(_started).TotalMilliseconds;
        using var response = await supervisor.ForwardHostedCommandAsync(command, lifetime);
        var result = response.RootElement.Clone();
        lock (_state)
        {
            if (!observation) _session.Commit(request.ClientId + ":" + request.Fence, request.CommandId!, fingerprint, result);
            return Reply(request, true, result);
        }
    }
}
