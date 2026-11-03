namespace CoreVideoPro.WinUI.Services;

/// <summary>The one Graphics surface the tab shows (and the one that holds the private preview).</summary>
public enum GraphicsDesignSurface { Playout, Builder, Designer, Workspace }

/// <summary>Graphics Designer slice 2: Operate shows the playlist; Design shows the Designer while a design is open
/// in it, else the advanced workspace when chosen, else the template-first builder.</summary>
public static class GraphicsDesignSurfaces
{
    public static GraphicsDesignSurface For(string mode, bool designerOpen, bool workspaceOpen) =>
        mode != "design" ? GraphicsDesignSurface.Playout
        : designerOpen ? GraphicsDesignSurface.Designer
        : workspaceOpen ? GraphicsDesignSurface.Workspace
        : GraphicsDesignSurface.Builder;

    /// <summary>The Design surfaces that must give up the private preview while <paramref name="active"/> shows (all three in
    /// Playout). The page suspends these before resuming the active one, so two surfaces never hold the lease.</summary>
    public static IReadOnlyList<GraphicsDesignSurface> SuspendFor(GraphicsDesignSurface active) =>
        new[] { GraphicsDesignSurface.Builder, GraphicsDesignSurface.Designer, GraphicsDesignSurface.Workspace }.Where(s => s != active).ToArray();
}
