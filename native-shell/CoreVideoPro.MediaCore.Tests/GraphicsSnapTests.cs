using Xunit;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsSnapTests
{
    private static (double, double) Delta((double Dx, double Dy, IReadOnlyList<GraphicsGuide> Guides) r) => (r.Dx, r.Dy);

    [Fact]
    public void MoveSnapsToTheNearestSafeEdgeOrCentre()
    {
        Assert.Equal((-4d, 0d), Delta(GraphicsSnap.Move(new(100, 300, 400, 100))));
        Assert.Equal(new GraphicsGuide('x', 96, "safe"), GraphicsSnap.Move(new(100, 300, 400, 100)).Guides.Single());
        Assert.Equal((4d, 0d), Delta(GraphicsSnap.Move(new(1420, 300, 400, 100))));
        var centred = GraphicsSnap.Move(new(757, 488, 410, 100));
        Assert.Equal((-2d, 2d), Delta(centred));
        Assert.All(centred.Guides, g => Assert.Equal("centre", g.Kind));
        Assert.Empty(GraphicsSnap.Move(new(105, 300, 400, 100)).Guides);
    }

    [Fact]
    public void MoveTopAndBottomSnapToSafeEdges()
    {
        Assert.Equal((0d, -3d), Delta(GraphicsSnap.Move(new(300, 57, 400, 100))));
        Assert.Equal(new GraphicsGuide('y', 54, "safe"), GraphicsSnap.Move(new(300, 57, 400, 100)).Guides.Single());
        Assert.Equal((0d, 2d), Delta(GraphicsSnap.Move(new(300, 924, 400, 100)))); // bottom 1024 -> 1026
    }

    [Fact]
    public void TiesPreferTheSafeEdge()
    {
        // x=100, width 1728: left 100 -> 96, right 1828 -> 1824 and centre 964 -> 960 are all 4 px away; the safe left edge (listed first) wins.
        var r = GraphicsSnap.Move(new(100, 300, 1728, 100));
        Assert.Equal(-4d, r.Dx);
        Assert.Equal(new GraphicsGuide('x', 96, "safe"), r.Guides.Single(g => g.Axis == 'x'));
    }

    [Fact]
    public void TheThresholdIsInclusive()
    {
        Assert.Equal(-8d, GraphicsSnap.Move(new(104, 300, 400, 100)).Dx);
        var outside = GraphicsSnap.Move(new(104.01, 300, 400, 100));
        Assert.Equal(0d, outside.Dx); Assert.Empty(outside.Guides);
    }

    [Fact]
    public void ResizeSnapsTheRightEdgeToTheCentreLine()
    {
        var r = GraphicsSnap.Resize(new(500, 300, 455, 100));
        Assert.Equal(5d, r.Dw);
        Assert.Equal(new GraphicsGuide('x', 960, "centre"), r.Guides.Single());
    }

    [Theory]
    [InlineData(0.5, 200, 192)] // 4 canvas px = 8 layer px at half scale
    [InlineData(2, 50, 48)]     // 4 canvas px = 2 layer px at double scale
    public void MoveLayerConvertsTheCanvasSnapIntoLayerUnits(double scale, double localX, double expectedX)
    {
        var start = new GraphicsRect(100, 300, 400, 100);
        var r = GraphicsSnap.MoveLayer(start, localX, 70, localX, 70, scale);
        Assert.Equal((expectedX, 70d), (r.X, r.Y));
        Assert.Equal(new GraphicsGuide('x', 96, "safe"), r.Guides.Single());
        // A drag of 10 layer px moves the canvas rect by 10 * scale (105 or 120): past the threshold, so no snap.
        var dragged = GraphicsSnap.MoveLayer(start, localX, 70, localX + 10, 70, scale);
        Assert.Equal(localX + 10, dragged.X); Assert.Empty(dragged.Guides);
    }

    [Theory]
    [InlineData(0.5, 3440, 3448)] // 1720 canvas px wide, right edge 1820 -> 1824: +4 canvas = +8 layer px
    [InlineData(2, 860, 862)]
    public void ResizeLayerConvertsTheCanvasSnapIntoLayerUnits(double scale, double width, double expectedWidth)
    {
        var start = new GraphicsRect(100, 300, width * scale, 100);
        var r = GraphicsSnap.ResizeLayer(start, width, 100 / scale, scale);
        Assert.Equal(expectedWidth, r.X, 6);
        Assert.Equal(new GraphicsGuide('x', 1824, "safe"), r.Guides.Single());
    }

    [Fact]
    public void AGuideIsOnlyDrawnWhenTheClampedLayerReachesIt()
    {
        // The layer's own x is already 0 (a group offset puts it at canvas 100): snapping left to 96 would need x = -4.
        var r = GraphicsSnap.MoveLayer(new GraphicsRect(100, 300, 400, 100), 0, 70, 0, 70, 1);
        Assert.Equal(0d, r.X);
        Assert.Empty(r.Guides);
        // Within half a pixel still counts as reached.
        var near = GraphicsSnap.MoveLayer(new GraphicsRect(96.4, 300, 400, 100), 0, 70, 0, 70, 1);
        Assert.Equal(0d, near.X); Assert.Single(near.Guides);
    }

    [Fact]
    public void MoveGraphicSnapsThePlacementOneToOne()
    {
        var r = GraphicsSnap.MoveGraphic(new GraphicsRect(100, 900, 400, 100), 100, -80, 0, 0);
        Assert.Equal((96d, -80d), (r.X, r.Y));
        var bottom = GraphicsSnap.MoveGraphic(new GraphicsRect(100, 900, 400, 100), 100, -80, 0, 24); // bottom 1024 -> 1026
        Assert.Equal((96d, -54d), (bottom.X, bottom.Y));
        Assert.Equal(2, bottom.Guides.Count);
    }

    [Fact]
    public void ResizeSnapsOnlyRightAndBottomEdges()
    {
        var r = GraphicsSnap.Resize(new(100, 100, 1720, 920));
        Assert.Equal((4d, 6d), (r.Dw, r.Dh));
        Assert.Equal(2, r.Guides.Count);
        var left = GraphicsSnap.Resize(new(100, 300, 400, 100)); // left near 96 must not snap
        Assert.Equal(0d, left.Dw); Assert.Empty(left.Guides);
    }
}
