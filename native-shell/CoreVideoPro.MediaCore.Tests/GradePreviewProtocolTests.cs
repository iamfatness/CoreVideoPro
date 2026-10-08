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
    [Theory] [InlineData("preparing")] [InlineData("unavailable")]
    public void StatusNeedsNoSurface(string status)
    {
        var line = $$$"""{"type":"grade-preview","instanceId":"a","sourceId":"p1","revision":0,"status":"{{{status}}}"}""";
        Assert.NotNull(GradePreviewProtocol.Parse(line));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("\"a\"", "null")));
        Assert.Null(GradePreviewProtocol.Parse(line.Replace("\"revision\":0", "\"revision\":-1")));
    }
}
