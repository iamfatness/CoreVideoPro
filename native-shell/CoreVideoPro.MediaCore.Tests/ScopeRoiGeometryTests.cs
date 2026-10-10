using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class ScopeRoiGeometryTests
{
    [Fact] public void NativeCoordinateFormattingDoesNotInvalidateCurrentMeasurementButStaleRevisionDoes()
    {
        var roi=new GradeScopeRoi(true,.123456789,.19999999999999998,.22499999999999998,.4,7,"circle");
        var wire=roi with {X=.123457,Y=.2,Width=.225};
        Assert.True(roi.MatchesMeasurement(wire));
        Assert.False(roi.MatchesMeasurement(wire with {Revision=6}));
        Assert.False(roi.MatchesMeasurement(wire with {Shape="rectangle"}));
        Assert.False(roi.MatchesMeasurement(wire with {Width=.226}));
        var edge=new GradeScopeRoi(true,1279d/1280,0,1d/1280,.1,7,"rectangle");
        var reported=edge with {X=.999219};
        Assert.False(reported.IsValid); // Rounded native reply may extend a fraction of a pixel past the edge.
        Assert.True(edge.MatchesMeasurement(reported));
    }
    [Theory] [InlineData(800,800,0,175,800,450)] [InlineData(1000,450,100,0,800,450)]
    public void ImageGeometryExcludesLetterboxAndPillarbox(double hostW,double hostH,double x,double y,double w,double h)
    {
        Assert.Equal((x,y,w,h),ScopeRoiGeometry.FitImage(hostW,hostH,1920,1080));
        Assert.Equal(default,ScopeRoiGeometry.FitImage(hostW,hostH,0,1080));
    }
    [Theory]
    [InlineData(.2,.2,.6,.7)] [InlineData(.6,.7,.2,.2)]
    [InlineData(.2,.7,.6,.2)] [InlineData(.6,.2,.2,.7)]
    public void CircleIsRoundInSourcePixelsInEveryDrawingDirection(double x,double y,double endX,double endY)
    {
        var roi=ScopeRoiGeometry.Draw(x,y,endX,endY,1920,1080,"circle");
        Assert.True(roi.IsValid); Assert.Equal("circle",roi.Shape);
        Assert.Equal(540,roi.Width*1920,8); Assert.Equal(540,roi.Height*1080,8);
        Assert.True(roi.X>=Math.Min(x,endX)); Assert.True(roi.Y>=Math.Min(y,endY));
        Assert.True(roi.X+roi.Width<=Math.Max(x,endX)+1e-9);
        Assert.True(roi.Y+roi.Height<=Math.Max(y,endY)+1e-9);
    }
    [Fact] public void DrawingAtEdgesAndOutsideImageClampsToValidOnePixelMinimum()
    {
        foreach(var shape in new[]{"circle","rectangle"}) {
            var roi=ScopeRoiGeometry.Draw(1,1,1,1,1920,1080,shape);
            Assert.True(roi.IsValid);Assert.Equal(1,roi.Width*1920,8);Assert.Equal(1,roi.Height*1080,8);
            Assert.True(ScopeRoiGeometry.Draw(-1,-1,2,2,1920,1080,shape).IsValid);
        }
        Assert.Throws<ArgumentException>(()=>ScopeRoiGeometry.Draw(double.NaN,0,1,1,1920,1080,"circle"));
        Assert.Throws<ArgumentException>(()=>ScopeRoiGeometry.Draw(0,0,1,1,1920,1080,"polygon"));
    }
    [Fact] public void CirclePrecisionResizeFitsImageAndLinksAxes()
    {
        var roi=ScopeRoiGeometry.CircleSize(new(true,.8,.8,.1,.1,3,"circle"),1000,1920,1080);
        Assert.True(roi.IsValid);Assert.Equal(216,roi.Width*1920,8);Assert.Equal(216,roi.Height*1080,8);
        Assert.Equal(3,roi.Revision);
    }
    [Fact] public void ShapePersistsAndOldPreferencesRemainRectangle()
    {
        var roi=new GradeScopeRoi(true,.1,.2,.2,.3,4,"circle");
        Assert.Equal(roi,JsonSerializer.Deserialize<GradeScopeRoi>(JsonSerializer.Serialize(roi)));
        Assert.Equal("rectangle",JsonSerializer.Deserialize<GradeScopeRoi>("{\"Enabled\":true}")!.Shape);
        Assert.False((roi with {Shape="ellipse"}).IsValid);
        Assert.Equal(28960,roi.ExpectedSampleCount);
        Assert.Equal(36864,(roi with {Enabled=false}).ExpectedSampleCount);
    }
}
