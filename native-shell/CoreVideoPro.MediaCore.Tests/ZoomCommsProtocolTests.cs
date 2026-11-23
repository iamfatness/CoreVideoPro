using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
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

    /// <summary>contracts/comms/zoom-comms-state.fixtures.json is read by this test AND the Mac shell's
    /// ShellTestsComms.swift with the same counts and strings: the drift guard between the parsers.</summary>
    [Fact]
    public void SharedFixtureParses()
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(ControlRepoPaths.Contract(Path.Combine("comms", "zoom-comms-state.fixtures.json"))));
        var lines = doc.RootElement.GetProperty("facts").EnumerateArray()
            .ToDictionary(f => f.GetProperty("name").GetString()!, f => f.GetProperty("line").GetString()!);
        ZoomCommsUpdate Fact(string name)
        {
            Assert.True(ZoomCommsProtocol.TryParseStateLine(lines[name], out var update), name);
            return update;
        }

        var desk = Fact("desk").State!;
        Assert.Equal(7, desk.Channels.Count);
        Assert.Equal("Ana Lucia Ferreira", desk.Channels[0].Label);
        Assert.Equal(-18, desk.Mic.PeakDbfs);
        Assert.Equal(2, desk.SendErrors);
        Assert.Equal(new uint[] { 201, 202, 205 }, desk.Roster.Select(p => p.ParticipantId));
        Assert.False(desk.Roster[2].Talkback);
        Assert.Equal("21:40:15 audio flowing into CH 1", desk.Log[^1]);

        var notHost = Fact("not-host").State!;
        Assert.False(notHost.Available);
        Assert.Equal("Make this account host or co-host to bring talkback up.", notHost.Reason);

        Assert.Equal(16, Fact("probe-joined").State!.Channels.Count);
        Assert.Contains(Fact("probe-keyed").State!.Channels, c => c.Keyed);
        Assert.True(Fact("probe-private-on").State!.Private);
        Assert.Null(Fact("dropped").State);
        Assert.False(ZoomCommsProtocol.TryParseStateLine(lines["response-not-fact"], out _));
    }
}
