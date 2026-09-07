using System.Text.Json;
using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

public sealed class ShowEngineProtocolTests
{
    [Fact]
    public void EncodeRequest_MergesThePayloadAtTopLevel_InCamelCase()
    {
        var line = ShowEngineProtocol.EncodeRequest("se-1", "invoke",
            new { action = "ohg.look.set", args = new object?[] { "wide" } });

        using var doc = JsonDocument.Parse(line);
        Assert.Equal("se-1", doc.RootElement.GetProperty("id").GetString());
        Assert.Equal("invoke", doc.RootElement.GetProperty("type").GetString());
        Assert.Equal("ohg.look.set", doc.RootElement.GetProperty("action").GetString());
        Assert.Equal("wide", doc.RootElement.GetProperty("args")[0].GetString());
        Assert.DoesNotContain('\n', line);
    }

    [Fact]
    public void EncodeRequest_WithNoPayload_IsIdAndTypeOnly()
    {
        var line = ShowEngineProtocol.EncodeRequest("se-2", "ping");
        Assert.Equal("{\"id\":\"se-2\",\"type\":\"ping\"}", line);
    }

    [Fact]
    public void EncodeRequest_NullPropertiesAreOmitted()
    {
        var line = ShowEngineProtocol.EncodeRequest("se-3", "activeSpeaker", new { participantId = (string?)null });
        Assert.Equal("{\"id\":\"se-3\",\"type\":\"activeSpeaker\"}", line);
    }

    [Theory]
    [InlineData("{\"id\":\"a\",\"ok\":true}", ShowEngineProtocol.LineKind.Response)]
    [InlineData("{\"id\":null,\"ok\":false,\"error\":{\"message\":\"x\"}}", ShowEngineProtocol.LineKind.Response)]
    [InlineData("{\"event\":\"handshake\",\"protocolVersion\":1}", ShowEngineProtocol.LineKind.Handshake)]
    [InlineData("{\"event\":\"snapshot\",\"generation\":1}", ShowEngineProtocol.LineKind.Snapshot)]
    [InlineData("{\"event\":\"hostCommand\",\"generation\":1}", ShowEngineProtocol.LineKind.HostCommand)]
    [InlineData("{\"event\":\"log\",\"level\":\"info\"}", ShowEngineProtocol.LineKind.Log)]
    [InlineData("{\"event\":\"somethingNew\"}", ShowEngineProtocol.LineKind.Unknown)]
    [InlineData("{\"nothing\":1}", ShowEngineProtocol.LineKind.Unknown)]
    [InlineData("[1,2,3]", ShowEngineProtocol.LineKind.Malformed)]
    [InlineData("\"hello\"", ShowEngineProtocol.LineKind.Malformed)]
    public void Classify_NamesEveryLineShapeTheHostEmits(string line, ShowEngineProtocol.LineKind expected)
    {
        using var doc = JsonDocument.Parse(line);
        Assert.Equal(expected, ShowEngineProtocol.Classify(doc));
    }

    [Fact]
    public void TryParseHandshake_ReadsTheFullManifest()
    {
        using var doc = JsonDocument.Parse(TestLines.HandshakeEvent(generation: 4));
        Assert.True(ShowEngineProtocol.TryParseHandshake(doc.RootElement, out var handshake, out var error));
        Assert.Null(error);
        Assert.Equal(1, handshake.ProtocolVersion);
        Assert.Equal("0.1.0", handshake.EngineVersion);
        Assert.Equal(4, handshake.Generation);
        Assert.Equal(2, handshake.Actions.Count);
        Assert.Equal("ohg.program.cut", handshake.Actions[0].Id);
        Assert.Equal("Cut", handshake.Actions[0].Title);
        Assert.Empty(handshake.Actions[0].Params);
        var p = Assert.Single(handshake.Actions[1].Params);
        Assert.Equal("lookId", p.Name);
        Assert.Equal("string", p.Type);
        Assert.True(p.Required);
        Assert.Equal("Look id", p.Description);
        Assert.Equal(new[] { "ohg/look", "ohg/slot/*/name" }, handshake.FieldTemplates);
        Assert.Equal(3, handshake.Snapshot.GetProperty("revision").GetInt32());
        Assert.True(handshake.Fields["ohg/onAir"].GetBoolean());
    }

    [Fact]
    public void TryParseHandshake_AlsoReadsAHandshakeDeliveredAsAResponse()
    {
        using var doc = JsonDocument.Parse(TestLines.HandshakeResponse("se-1", generation: 2));
        Assert.True(ShowEngineProtocol.TryParseHandshake(doc.RootElement, out var handshake, out _));
        Assert.Equal(2, handshake.Generation);
    }

    [Fact]
    public void TryParseHandshake_FailsLoudlyOnAMissingProtocolVersion()
    {
        using var doc = JsonDocument.Parse("{\"event\":\"handshake\",\"engineVersion\":\"1\"}");
        Assert.False(ShowEngineProtocol.TryParseHandshake(doc.RootElement, out _, out var error));
        Assert.Contains("protocolVersion", error);
    }

    [Fact]
    public void ParseSnapshot_ReadsGenerationRevisionAndFields()
    {
        using var doc = JsonDocument.Parse(TestLines.SnapshotEvent(generation: 3, revision: 88));
        var snapshot = ShowEngineProtocol.ParseSnapshot(doc.RootElement);
        Assert.Equal(3, snapshot.Generation);
        Assert.Equal(88, snapshot.Revision);
        Assert.Equal(88, snapshot.Snapshot.GetProperty("revision").GetInt32());
        Assert.Equal("tight", snapshot.Fields["ohg/look"].GetString());
    }

    [Fact]
    public void ParseHostCommand_ReadsGenerationSeqNameAndArgs()
    {
        using var doc = JsonDocument.Parse(TestLines.HostCommandEvent(2, 17, "assignSlot", "[3,\"p1\"]"));
        var command = ShowEngineProtocol.ParseHostCommand(doc.RootElement);
        Assert.Equal(2, command.Generation);
        Assert.Equal(17, command.Seq);
        Assert.Equal("assignSlot", command.Name);
        Assert.Equal(3, command.Args[0].GetInt32());
        Assert.Equal("p1", command.Args[1].GetString());
    }

    [Fact]
    public void ParseLog_ReadsLevelAndMessage()
    {
        using var doc = JsonDocument.Parse(TestLines.LogEvent("error", "boom"));
        var log = ShowEngineProtocol.ParseLog(doc.RootElement);
        Assert.Equal("error", log.Level);
        Assert.Equal("boom", log.Message);
    }

    [Theory]
    [InlineData("{\"kind\":\"ok\"}", "ok", null, null)]
    [InlineData("{\"kind\":\"refused\",\"reason\":\"no look selected\"}", "refused", "no look selected", null)]
    [InlineData("{\"kind\":\"error\",\"message\":\"unknown action\"}", "error", null, "unknown action")]
    public void ParseActionResult_ReadsTheInvokeResult(string resultJson, string kind, string? reason, string? message)
    {
        using var doc = JsonDocument.Parse("{\"id\":\"se-1\",\"ok\":true,\"result\":" + resultJson + "}");
        var result = ShowEngineProtocol.ParseActionResult(doc.RootElement);
        Assert.Equal(kind, result.Kind);
        Assert.Equal(reason, result.Reason);
        Assert.Equal(message, result.Message);
    }

    [Fact]
    public void ParseActionResult_WithoutAResultNodeIsAnError()
    {
        using var doc = JsonDocument.Parse("{\"id\":\"se-1\",\"ok\":true}");
        var result = ShowEngineProtocol.ParseActionResult(doc.RootElement);
        Assert.Equal("error", result.Kind);
        Assert.Contains("result", result.Message);
    }
}
