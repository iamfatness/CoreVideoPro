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
        // left 100 -> 96 (|4|); width 1720 makes right 1820 -> 1824 (|4|): safe candidates tie, left wins (first).
        // Centre 960 vs left 96 tie: x=-264 w=720 -> left -264 (far), centre 96 (far)... use a direct tie:
        // left at 100 (delta -4) and centre at 964 (delta -4) with width 1728: left 100, centre 964.
        var r = GraphicsSnap.Move(new(100, 300, 1728, 100));
        Assert.Equal(-4d, r.Dx);
        Assert.Equal("safe", r.Guides.First(g => g.Axis == 'x').Kind);
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
