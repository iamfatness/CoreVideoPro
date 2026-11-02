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
