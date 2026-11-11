using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Comms wire format between the shell and the core (docs/reference/comms-talkback.md).</summary>
public sealed class ZoomCommsProtocolTests
{
    [Fact]
    public void VerbsAreEdgesPlusReleaseAll()
    {
        Assert.Equal(
            ["comms-assign", "comms-gain", "comms-key", "comms-latch", "comms-mic", "comms-private", "comms-release-all",
             "comms-sidetone", "comms-sidetone-out", "comms-tone"],
            ZoomCommsProtocol.Verbs.Order(StringComparer.Ordinal));
    }

    [Fact]
    public void ARefusalCarriesTheCoreMessageVerbatim()
    {
        using var refused = JsonDocument.Parse(
            """{"id":"7","ok":false,"error":{"code":"comms-refused","message":"Join a meeting first."}}""");
        var r = ZoomCommsProtocol.ParseReply(refused);
        Assert.False(r.Ok);
        Assert.Equal("Join a meeting first.", r.Error);

        using var accepted = JsonDocument.Parse("""{"id":"8","ok":true,"type":"comms-key"}""");
        Assert.True(ZoomCommsProtocol.ParseReply(accepted).Ok);
    }

    [Fact]
    public void AStateFactWithAnIdIsAResponseNotAFact()
    {
        Assert.False(ZoomCommsProtocol.TryParseStateLine("""{"id":"1","type":"zoom-comms-state","comms":null}""", out _));
        Assert.False(ZoomCommsProtocol.TryParseStateLine("""{"type":"zoom-comms-state","comms":7}""", out _));
        Assert.True(ZoomCommsProtocol.TryParseStateLine("""{"type":"zoom-comms-state","comms":{"available":false,"reason":"Not in a meeting."}}""", out var u));
        Assert.False(u.State!.Available);
        Assert.Equal("Not in a meeting.", u.State.Reason);
        Assert.Empty(u.State.Channels);
        Assert.Null(u.State.Mic.PeakDbfs);  // nothing unmeasured is invented
        Assert.False(u.State.Private);
        Assert.True(ZoomCommsProtocol.TryParseStateLine("""{"type":"zoom-comms-state","comms":{"available":true,"private":true}}""", out var p));
        Assert.True(p.State!.Private);
    }

    [Theory]
    [InlineData("comms-key")]
    [InlineData("comms-latch")]
    [InlineData("comms-release-all")]
    [InlineData("comms-private")]
    public void KeyUpsUseTheReservedLane(string type) =>
        Assert.True(EngineHostWire.IsUrgent(JsonSerializer.SerializeToElement(new { type })));

    [Fact]
    public void CommsVerbsAreMutationsNotObservations() =>
        Assert.False(EngineHostWire.IsObservation(JsonSerializer.SerializeToElement(new { type = "comms-key" })));
}
