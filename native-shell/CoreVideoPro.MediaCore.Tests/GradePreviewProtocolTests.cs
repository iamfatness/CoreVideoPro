using CoreVideoPro.MediaCore.Services;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class GradePreviewProtocolTests
{
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
        var line="""{"type":"grade-preview","instanceId":"a","sourceId":"p1","revision":2,"sourceEpoch":8,"status":"ready","texture":{"width":64,"height":64,"sharedHandleHex":"0x1234"},"scopes":{"status":"ready","revision":2,"sourceEpoch":8,"view":1,"sampleWidth":256,"sampleHeight":144,"sampleCount":36864,"colorSpace":"rec709-sdr-assumed","completionObservedAtUnixMs":1790000000000,"texture":{"width":768,"height":256,"sharedHandleHex":"0x5678"}}}""";
        var result=GradePreviewProtocol.Parse(line);Assert.NotNull(result);Assert.Equal(1790000000000,result!.Scopes!.CompletionObservedAtUnixMs);
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
