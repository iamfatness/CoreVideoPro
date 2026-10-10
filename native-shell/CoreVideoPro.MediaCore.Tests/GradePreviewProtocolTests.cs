using CoreVideoPro.MediaCore.Services;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class GradePreviewProtocolTests
{
    [Fact] public void CircleResponseRequiresMaskedSampleCountAndKnownShape()
    {
        var line="""{"type":"grade-preview","instanceId":"a","sourceId":"p1","revision":2,"sourceEpoch":8,"status":"ready","texture":{"width":64,"height":64,"sharedHandleHex":"0x1234"},"scopes":{"status":"ready","revision":2,"sourceEpoch":8,"view":1,"roi":{"enabled":true,"shape":"circle","width":1,"height":1},"sampleWidth":256,"sampleHeight":144,"sampleCount":28960,"colorSpace":"rec709-sdr-assumed","texture":{"width":1536,"height":512,"sharedHandleHex":"0x5678"}}}""";
        Assert.Equal("circle",GradePreviewProtocol.Parse(line)!.Scopes!.Roi.Shape);
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("28960","36864")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("circle","triangle")));
        Assert.NotNull(GradePreviewProtocol.Parse(line.Replace("\"enabled\":true","\"enabled\":false").Replace("28960","36864")));
        Assert.NotNull(GradePreviewProtocol.Parse(line.Replace("\"width\":1,\"height\":1","\"x\":0.999219,\"width\":0.00078125,\"height\":1")));
    }
    [Theory] [InlineData("ready")] [InlineData("held")] [InlineData("stale")]
    public void ParsesAttributableNativeSurface(string status)
    {
        var line = $$$"""{"type":"grade-preview","instanceId":"a","sourceId":"p1","revision":2,"sourceEpoch":8,"sourceFrameId":77,"status":"{{{status}}}","texture":{"width":1920,"height":1080,"sharedHandleHex":"0x1234"}}""";
        var result = GradePreviewProtocol.Parse(line); Assert.NotNull(result);
        Assert.Equal(77, result!.SourceFrameId); Assert.Equal(2, result.Revision);
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("0x1234", "0x0")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("1920", "99999")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("grade-preview", "program-frame")));
    }
    [Fact] public void ScopeTextureRequiresMatchingEpochRevisionKnownSamplingAndColorSpace()
    {
        var line="""{"type":"grade-preview","instanceId":"a","sourceId":"p1","revision":2,"sourceEpoch":8,"status":"ready","texture":{"width":64,"height":64,"sharedHandleHex":"0x1234"},"scopes":{"status":"ready","revision":2,"sourceEpoch":8,"view":1,"sampleWidth":256,"sampleHeight":144,"sampleCount":36864,"colorSpace":"rec709-sdr-assumed","completionObservedAtUnixMs":1790000000000,"texture":{"width":1536,"height":512,"sharedHandleHex":"0x5678"}}}""";
        var result=GradePreviewProtocol.Parse(line);Assert.NotNull(result);Assert.Equal(1790000000000,result!.Scopes!.CompletionObservedAtUnixMs);
        Assert.Equal(1536, result.Scopes.Texture!.Width);
        Assert.Equal(512, result.Scopes.Texture.Height);
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("\"width\":1536,\"height\":512", "\"width\":768,\"height\":256")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("\"sampleCount\":36864","\"sampleCount\":0")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("rec709-sdr-assumed","unknown")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("\"view\":1","\"view\":9")));
        var offset=line.IndexOf("\"scopes\"",StringComparison.Ordinal);
        Assert.Null(GradePreviewProtocol.Parse(line[..offset]+line[offset..].Replace("\"sourceEpoch\":8","\"sourceEpoch\":7")));
        Assert.Null(GradePreviewProtocol.Parse(line[..offset]+line[offset..].Replace("\"revision\":2","\"revision\":1")));
    }
    [Theory] [InlineData("preparing")] [InlineData("unavailable")]
    public void StatusNeedsNoSurface(string status)
    {
        var line = $$$"""{"type":"grade-preview","instanceId":"a","sourceId":"p1","revision":0,"status":"{{{status}}}"}""";
        Assert.NotNull(GradePreviewProtocol.Parse(line));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("\"a\"", "null")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("\"revision\":0", "\"revision\":-1")));
    }
}
