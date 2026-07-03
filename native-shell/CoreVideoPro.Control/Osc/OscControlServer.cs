using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;

namespace CoreVideoPro.Control.Osc;

public sealed record OscControlServerOptions
{
    /// <summary>UDP port to receive OSC commands on.</summary>
    public int ListenPort { get; init; } = 8010;

    /// <summary>Bind address. Defaults to loopback; set to <see cref="IPAddress.Any"/> for LAN
    /// control surfaces (only when the operator opts in — the app is otherwise localhost-only).</summary>
    public IPAddress BindAddress { get; init; } = IPAddress.Loopback;

    /// <summary>Optional fixed feedback target (e.g. a Companion instance's OSC listener). When
    /// null, feedback is sent back to whichever endpoints have recently sent commands.</summary>
    public IPEndPoint? FeedbackEndpoint { get; init; }

    public OscAddressMap AddressMap { get; init; } = new();
}

/// <summary>UDP OSC transport: receives OSC command datagrams, routes them through
/// <see cref="OscControlRouter"/> onto the control surface, and pushes <c>/cvp/state/*</c> feedback
/// (coalesced) to the configured target and/or recent senders on state change. A malformed datagram
/// is ignored, never fatal. This class is a thin socket wrapper; the routable/encodable logic lives
/// in <see cref="OscControlRouter"/>/<see cref="OscFeedback"/> and is unit-tested there.</summary>
public sealed class OscControlServer : IAsyncDisposable
{
    private readonly IControlSurface _surface;
    private readonly OscControlServerOptions _options;
    private readonly OscControlRouter _router;
    private readonly ConcurrentDictionary<IPEndPoint, DateTimeOffset> _recentSenders = new();
    private UdpClient? _udp;
    private CancellationTokenSource? _cts;
    private Task? _receiveLoop;

    public OscControlServer(IControlSurface surface, OscControlServerOptions? options = null)
    {
        _surface = surface;
        _options = options ?? new OscControlServerOptions();
        _router = new OscControlRouter(surface, _options.AddressMap);
    }

    /// <summary>The port actually bound (useful when ListenPort is 0 for an ephemeral test port).</summary>
    public int BoundPort { get; private set; }

    public void Start()
    {
        if (_udp is not null)
        {
            return;
        }

        _udp = new UdpClient(new IPEndPoint(_options.BindAddress, _options.ListenPort));
        BoundPort = ((IPEndPoint)_udp.Client.LocalEndPoint!).Port;
        _cts = new CancellationTokenSource();
        _surface.StateChanged += OnStateChanged;
        _receiveLoop = Task.Run(() => ReceiveLoopAsync(_cts.Token));
    }

    private async Task ReceiveLoopAsync(CancellationToken cancellationToken)
    {
        var udp = _udp!;
        while (!cancellationToken.IsCancellationRequested)
        {
            UdpReceiveResult received;
            try
            {
                received = await udp.ReceiveAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (ObjectDisposedException)
            {
                break;
            }
            catch (SocketException)
            {
                // Transient (e.g. ICMP port-unreachable from a prior send); keep listening.
                continue;
            }

            _recentSenders[received.RemoteEndPoint] = DateTimeOffset.UtcNow;

            foreach (var message in OscCodec.Decode(received.Buffer))
            {
                try
                {
                    await _router.RouteAsync(message, cancellationToken).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    return;
                }
                catch
                {
                    // A single bad action must never take down the listener.
                }
            }
        }
    }

    private void OnStateChanged(object? sender, ControlState state)
    {
        // Fire-and-forget; feedback loss is acceptable (the next change re-sends full state).
        _ = SendFeedbackAsync(state);
    }

    private async Task SendFeedbackAsync(ControlState state)
    {
        var udp = _udp;
        if (udp is null)
        {
            return;
        }

        var targets = ResolveFeedbackTargets();
        if (targets.Count == 0)
        {
            return;
        }

        foreach (var message in OscFeedback.Encode(state, _options.AddressMap))
        {
            var datagram = OscCodec.EncodeMessage(message);
            foreach (var target in targets)
            {
                try
                {
                    await udp.SendAsync(datagram, datagram.Length, target).ConfigureAwait(false);
                }
                catch
                {
                    // Best-effort feedback.
                }
            }
        }
    }

    private IReadOnlyList<IPEndPoint> ResolveFeedbackTargets()
    {
        if (_options.FeedbackEndpoint is not null)
        {
            return new[] { _options.FeedbackEndpoint };
        }

        // Fall back to endpoints that have sent a command in the last 5 minutes.
        var cutoff = DateTimeOffset.UtcNow.AddMinutes(-5);
        var stale = _recentSenders.Where(pair => pair.Value < cutoff).Select(pair => pair.Key).ToList();
        foreach (var endpoint in stale)
        {
            _recentSenders.TryRemove(endpoint, out _);
        }

        return _recentSenders.Keys.ToList();
    }

    public async ValueTask DisposeAsync()
    {
        _surface.StateChanged -= OnStateChanged;
        _cts?.Cancel();
        _udp?.Close();
        if (_receiveLoop is not null)
        {
            try
            {
                await _receiveLoop.ConfigureAwait(false);
            }
            catch
            {
                // ignore shutdown races
            }
        }

        _udp?.Dispose();
        _cts?.Dispose();
        _udp = null;
    }
}
