using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class GraphicsDesignSurfacesTests
{
    [Theory]
    [InlineData("playout", true, true, GraphicsDesignSurface.Playout)]
    [InlineData("design", true, true, GraphicsDesignSurface.Designer)]
    [InlineData("design", false, true, GraphicsDesignSurface.Workspace)]
    [InlineData("design", false, false, GraphicsDesignSurface.Builder)]
    public void OneDesignSurfaceAtATime(string mode, bool designer, bool workspace, GraphicsDesignSurface expected) =>
        Assert.Equal(expected, GraphicsDesignSurfaces.For(mode, designer, workspace));
}

public sealed class GraphicsDesignPreviewLeaseTests
{
    [Theory]
    [InlineData(GraphicsDesignSurface.Playout, "Builder,Designer,Workspace")]
    [InlineData(GraphicsDesignSurface.Builder, "Designer,Workspace")]
    [InlineData(GraphicsDesignSurface.Designer, "Builder,Workspace")]
    [InlineData(GraphicsDesignSurface.Workspace, "Builder,Designer")]
    public void EveryOtherDesignSurfaceGivesUpThePreview(GraphicsDesignSurface active, string suspended) =>
        Assert.Equal(suspended, string.Join(",", GraphicsDesignSurfaces.SuspendFor(active)));
}
