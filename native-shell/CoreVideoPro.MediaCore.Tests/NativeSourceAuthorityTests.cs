using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeSourceAuthorityTests
{
    private const string Catalog = """
        {"version":1,"valid":true,"processEpoch":"epoch","sequence":12,"futureField":{},"sources":[
        {"sourceId":"camera-42","instanceId":"camera-incarnation","processEpoch":"epoch","generation":9007199254740991,"participantId":"42","kind":"camera","available":true},
        {"sourceId":"share-42","instanceId":"share-incarnation","processEpoch":"epoch","generation":2,"participantId":"42","kind":"share","available":false}]}
        """;

    private static NativeSourceAuthority? Map(string? catalog)
    {
        using var response = JsonDocument.Parse("{\"ok\":true,\"snapshot\":{\"health\":null,\"profile\":null"
            + (catalog is null ? "" : ",\"sourceAuthority\":" + catalog) + "}}");
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        return NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot([], 0, 0, wire).SourceAuthority;
    }

    [Fact]
    public void RealWireMappingPreservesExactDistinctCameraAndShareTokens()
    {
        var value = Map(Catalog)!;
        Assert.True(value.Valid);
        Assert.Equal("epoch", value.ProcessEpoch);
        Assert.Equal(12, value.Sequence);
        Assert.Equal(2, value.Sources!.Count);
        Assert.Equal("camera-incarnation", value.Sources[0].InstanceId);
        Assert.Equal(9_007_199_254_740_991, value.Sources[0].Generation);
        Assert.Equal("share-incarnation", value.Sources[1].InstanceId);
        Assert.False(value.Sources[1].Available);
    }

    [Fact]
    public void AbsentKnownEmptyAndInvalidAreDistinct()
    {
        Assert.Null(Map(null));
        var empty = JsonNode.Parse(Catalog)!;
        empty["sources"] = new JsonArray();
        Assert.True(Map(empty.ToJsonString())!.Valid);
        Assert.Empty(Map(empty.ToJsonString())!.Sources!);
        empty["valid"] = false;
        Assert.False(Map(empty.ToJsonString())!.Valid);
        Assert.Empty(Map(empty.ToJsonString())!.Sources!);
    }

    [Theory]
    [InlineData("generation", "0")]
    [InlineData("generation", "9007199254740992")]
    [InlineData("processEpoch", "\"other\"")]
    [InlineData("kind", "\"future-kind\"")]
    [InlineData("participantId", "\"042\"")]
    [InlineData("available", "null")]
    [InlineData("instanceId", "\"\"")]
    public void InvalidSourceNeverExposesPartialSelectableCatalog(string property, string json)
    {
        var value = JsonNode.Parse(Catalog)!;
        value["sources"]![0]![property] = JsonNode.Parse(json);
        var mapped = Map(value.ToJsonString())!;
        Assert.False(mapped.Valid);
        Assert.Empty(mapped.Sources!);
    }

    [Theory]
    [InlineData("sourceId")]
    [InlineData("instanceId")]
    [InlineData("kind")]
    public void DuplicateProviderIdentityIsRejected(string property)
    {
        var value = JsonNode.Parse(Catalog)!;
        value["sources"]![1]![property] = value["sources"]![0]![property]!.DeepClone();
        Assert.False(Map(value.ToJsonString())!.Valid);
    }

    [Theory]
    [InlineData("sources", "null")]
    [InlineData("sequence", "0")]
    [InlineData("sequence", "9007199254740992")]
    [InlineData("version", "2")]
    public void IncompleteOrUnsupportedEnvelopeIsInvalid(string property, string json)
    {
        var value = JsonNode.Parse(Catalog)!;
        value[property] = JsonNode.Parse(json);
        Assert.False(Map(value.ToJsonString())!.Valid);
    }

    [Theory]
    [InlineData("null")]
    [InlineData("[]")]
    [InlineData("42")]
    [InlineData("{\"sequence\":1.5}")]
    [InlineData("{\"sequence\":9223372036854775808}")]
    [InlineData("{\"valid\":\"yes\"}")]
    [InlineData("{\"sources\":{}}")]
    public void MalformedAuthorityPreservesOtherStateAcrossAllParserPaths(string authority)
    {
        Assert.False(Map(authority)!.Valid);
        using var captureResponse = JsonDocument.Parse("{\"ok\":true,\"type\":\"zoom-snapshot\",\"snapshot\":{\"meetingState\":\"in_meeting\",\"tick\":27,\"sourceAuthority\":" + authority + "}}");
        var capture = CoreProtocolParser.TryParseCaptureSnapshot(captureResponse, "zoom-snapshot")!;
        Assert.Equal(27, capture.Tick);
        Assert.Equal("in_meeting", capture.MeetingState);
        Assert.False(capture.SourceAuthority!.Valid);
        Assert.Empty(capture.SourceAuthority.Sources!);
        using var spineResponse = JsonDocument.Parse("{\"ok\":true,\"type\":\"zoom-media-spine-sync\",\"spineSnapshot\":{\"meetingState\":\"in_meeting\",\"participantCount\":3,\"sourceAuthority\":" + authority + "}}");
        var spine = CoreProtocolParser.TryParseZoomMediaSpineSnapshot(spineResponse)!;
        Assert.Equal(3, spine.ParticipantCount);
        Assert.False(ZoomMediaSpineSnapshotMerger.ToCaptureSnapshot(spine).SourceAuthority!.Valid);
    }

    [Fact]
    public void LeaveAbsentAndRejoinAlwaysReplacePriorAuthority()
    {
        var old = Map(Catalog)!;
        var initial = ZoomCaptureSnapshotMerger.Merge(null, new RawCaptureSnapshot
            { MeetingState = "in_meeting", SourceAuthority = old });
        Assert.True(initial.SourceAuthority!.Valid);
        var absent = ZoomCaptureSnapshotMerger.Merge(initial, new RawCaptureSnapshot { MeetingState = "idle" });
        Assert.Null(absent.SourceAuthority);
        var incoherent = ZoomCaptureSnapshotMerger.Merge(initial, new RawCaptureSnapshot
            { MeetingState = "idle", SourceAuthority = old });
        Assert.False(incoherent.SourceAuthority!.Valid);
        Assert.Empty(incoherent.SourceAuthority.Sources!);
        var empty = old with { Sources = [] };
        var left = ZoomCaptureSnapshotMerger.Merge(initial, new RawCaptureSnapshot
            { MeetingState = "idle", SourceAuthority = empty });
        Assert.True(left.SourceAuthority!.Valid);
        Assert.Empty(left.SourceAuthority.Sources!);
        var fresh = Map(Catalog.Replace("epoch", "new-epoch"))!;
        var rejoined = ZoomMediaSpineSnapshotMerger.Merge(left, new ZoomMediaSpineNativeSnapshot
            { MeetingState = "in_meeting", SourceAuthority = fresh });
        Assert.Equal("new-epoch", rejoined.SourceAuthority!.ProcessEpoch);
        Assert.True(rejoined.SourceAuthority.Valid);
        var legacy = ZoomMediaSpineSnapshotMerger.Merge(rejoined, new ZoomMediaSpineNativeSnapshot
            { MeetingState = "in_meeting" });
        Assert.Null(legacy.SourceAuthority);
    }

    [Fact]
    public void JoiningCannotMakeOldNonemptyAuthorityCoherent()
    {
        var joined = ZoomMediaSpineSnapshotMerger.Merge(null, new ZoomMediaSpineNativeSnapshot
            { MeetingState = "joining", SourceAuthority = Map(Catalog) });
        Assert.False(joined.SourceAuthority!.Valid);
    }

    [Fact]
    public void CaptureAndSpineParserPreserveValidExactTokens()
    {
        using var captureResponse = JsonDocument.Parse("{\"ok\":true,\"type\":\"zoom-snapshot\",\"snapshot\":{\"meetingState\":\"in_meeting\",\"sourceAuthority\":" + Catalog + "}}");
        var capture = CoreProtocolParser.TryParseCaptureSnapshot(captureResponse, "zoom-snapshot")!;
        Assert.Equal("camera-incarnation", capture.SourceAuthority!.Sources![0].InstanceId);
        using var spineResponse = JsonDocument.Parse("{\"ok\":true,\"type\":\"zoom-media-spine-sync\",\"spineSnapshot\":{\"meetingState\":\"in_meeting\",\"sourceAuthority\":" + Catalog + "}}");
        var spine = CoreProtocolParser.TryParseZoomMediaSpineSnapshot(spineResponse)!;
        Assert.Equal("share-incarnation", ZoomMediaSpineSnapshotMerger.ToCaptureSnapshot(spine).SourceAuthority!.Sources![1].InstanceId);
    }
}
