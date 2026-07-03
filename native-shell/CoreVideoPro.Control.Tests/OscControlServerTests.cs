using System.Net;
using System.Net.Sockets;
using CoreVideoPro.Control.Osc;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class OscControlServerTests
{
    [Fact]
    public async Task Server_ReceivesUdpDatagram_AndInvokesTheAction()
    {
        var surface = new FakeControlSurface();
        await using var server = new OscControlServer(surface, new OscControlServerOptions
        {
            ListenPort = 0,  // ephemeral
            BindAddress = IPAddress.Loopback
        });
        server.Start();

        using var client = new UdpClient();
        var datagram = OscCodec.EncodeMessage(new OscMessage("/cvp/transport/take"));
        await client.SendAsync(datagram, datagram.Length, new IPEndPoint(IPAddress.Loopback, server.BoundPort));

        // Wait (bounded) for the async receive loop to route it.
        var deadline = DateTime.UtcNow.AddSeconds(3);
        while (surface.Invocations.Count == 0 && DateTime.UtcNow < deadline)
        {
            await Task.Delay(10);
        }

        var (actionId, _) = Assert.Single(surface.Invocations);
        Assert.Equal("transport.take", actionId);
    }

    [Fact]
    public async Task Server_PushesFeedback_ToTheSenderOnStateChange()
    {
        var surface = new FakeControlSurface();
        await using var server = new OscControlServer(surface, new OscControlServerOptions
        {
            ListenPort = 0,
            BindAddress = IPAddress.Loopback
        });
        server.Start();

        using var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        // Send a command so the server learns our endpoint as a feedback target.
        var cmd = OscCodec.EncodeMessage(new OscMessage("/cvp/transport/take"));
        await client.SendAsync(cmd, cmd.Length, new IPEndPoint(IPAddress.Loopback, server.BoundPort));

        // Give the server a moment to register the sender, then raise a state change.
        await Task.Delay(100);
        surface.RaiseStateChanged(ControlState.Empty with { Recording = true });

        var receive = client.ReceiveAsync();
        var completed = await Task.WhenAny(receive, Task.Delay(3000));
        Assert.Same(receive, completed);

        var messages = OscCodec.Decode(receive.Result.Buffer);
        Assert.Contains(messages, m => m.Address == "/cvp/state/recording" && Equals(m.Args[0], 1));
    }
}
