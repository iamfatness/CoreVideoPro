using CoreVideoPro.Control;
using CoreVideoPro.Control.Osc;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class OscControlRoutingTests
{
    [Fact]
    public void AddressMap_MapsActionIdsToAddressesAndBack()
    {
        var map = new OscAddressMap();
        Assert.Equal("/cvp/transport/take", map.ActionIdToAddress("transport.take"));
        Assert.Equal("transport.take", map.AddressToActionId("/cvp/transport/take"));
        Assert.Equal("input.assign", map.AddressToActionId("/cvp/input/assign"));

        // Not under the root, or in the feedback namespace → not an action.
        Assert.Null(map.AddressToActionId("/other/thing"));
        Assert.Null(map.AddressToActionId("/cvp/state/recording"));
    }

    [Fact]
    public async Task Router_InvokesTheMappedActionWithBoundArgs()
    {
        var surface = new FakeControlSurface();
        var router = new OscControlRouter(surface);

        var result = await router.RouteAsync(new OscMessage("/cvp/input/assign", 3, "zoom:p-1"));

        Assert.NotNull(result);
        Assert.True(result!.Ok);
        var (actionId, args) = Assert.Single(surface.Invocations);
        Assert.Equal("input.assign", actionId);
        Assert.Equal(3, args[0]);
        Assert.Equal("zoom:p-1", args[1]);
    }

    [Fact]
    public async Task Router_IgnoresUnknownAddresses()
    {
        var surface = new FakeControlSurface();
        var router = new OscControlRouter(surface);

        Assert.Null(await router.RouteAsync(new OscMessage("/cvp/state/recording", 1)));
        Assert.Null(await router.RouteAsync(new OscMessage("/not/ours", 1)));
        Assert.Empty(surface.Invocations);
    }

    [Fact]
    public async Task Router_ReturnsBindErrorWithoutInvoking()
    {
        var surface = new FakeControlSurface();
        var router = new OscControlRouter(surface);

        // scene.select requires a string arg; none supplied.
        var result = await router.RouteAsync(new OscMessage("/cvp/scene/select"));
        Assert.NotNull(result);
        Assert.False(result!.Ok);
        Assert.Empty(surface.Invocations);
    }

    [Fact]
    public async Task EndToEnd_EncodedDatagram_DecodesRoutesAndInvokes()
    {
        var surface = new FakeControlSurface();
        var router = new OscControlRouter(surface);

        // Simulate the full wire path: encode → (UDP) → decode → route.
        var datagram = OscCodec.EncodeMessage(new OscMessage("/cvp/transport/record/set", true));
        var message = Assert.Single(OscCodec.Decode(datagram));
        var result = await router.RouteAsync(message);

        Assert.True(result!.Ok);
        var (actionId, args) = Assert.Single(surface.Invocations);
        Assert.Equal("transport.record.set", actionId);
        Assert.True(Assert.IsType<bool>(args[0]));
    }

    [Fact]
    public void Feedback_EncodesStateFieldsToStateAddresses()
    {
        var map = new OscAddressMap();
        var state = ControlState.Empty with
        {
            Recording = true,
            ActiveSceneId = "interview",
            MultiviewTileCount = 6,
            AudioMonitorVolume = 0.5,
            Inputs = new[] { new ControlInputState(1, true, "ZoomParticipant", "zoom:p-1", "Host") }
        };

        var messages = OscFeedback.Encode(state, map);
        var byAddress = messages.ToDictionary(m => m.Address, m => m.Args);

        Assert.Equal(1, byAddress["/cvp/state/recording"][0]);
        Assert.Equal("interview", byAddress["/cvp/state/activeSceneId"][0]);
        Assert.Equal(6, byAddress["/cvp/state/multiviewTileCount"][0]);
        Assert.Equal(0.5f, byAddress["/cvp/state/audioMonitorVolume"][0]);
        Assert.Equal(1, byAddress["/cvp/state/input/1/inShow"][0]);
        Assert.Equal("Host", byAddress["/cvp/state/input/1/name"][0]);
    }

    [Fact]
    public void Manifest_CoversEveryActionWithAnOscAddress_AndSerializes()
    {
        var manifest = ControlManifest.Build();
        Assert.Equal(ControlActionRegistry.Actions.Count, manifest.Actions.Count);
        Assert.All(manifest.Actions, a => Assert.StartsWith("/cvp/", a.OscAddress));
        Assert.Contains(manifest.Actions, a => a.Id == "transport.take");

        var json = manifest.ToJson();
        Assert.Contains("\"transport.take\"", json);
        Assert.Contains("/cvp/transport/take", json);
    }
}
