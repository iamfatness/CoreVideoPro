using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Graphics Designer slice 1: the show's designs. One <see cref="GraphicsDesignService"/> and
/// one <see cref="GraphicsDesignWorkflow"/> serve every surface (builder list, start dialog, workspace
/// Save). The decisions live in MediaCore and are unit-tested there; this partial only switches the
/// Graphics tab to the editor when a design opens and wires the core-restart reinstall.</summary>
public sealed partial class StudioViewModel
{
    private GraphicsDesignService? _graphicsDesigns;
    private GraphicsDesignWorkflow? _graphicsDesignWorkflow;
    private readonly GraphicsDesignResync _graphicsDesignResync = new();

    public GraphicsDesignService GraphicsDesigns => _graphicsDesigns ??= new(GraphicsLibrary, InvokeGraphicsActionAsync, () => _bridge.LastSnapshot?.Graphics);

    private GraphicsDesignWorkflow GraphicsDesignWorkflow
    {
        get {
            if (_graphicsDesignWorkflow is not null) return _graphicsDesignWorkflow;
            _graphicsDesignWorkflow = new(GraphicsDesigns, GraphicsWorkspace, GraphicsLibrary, () => _bridge.LastSnapshot?.Graphics);
            _graphicsDesignWorkflow.Opened += (_, _) => {
                GraphicsMode = GraphicsModeDesign; GraphicsDesignerOpen = true; GraphicsDesignAdvanced = false;
                GraphicsDesignOpened?.Invoke(this, EventArgs.Empty);
            };
            return _graphicsDesignWorkflow;
        }
    }

    /// <summary>Raised after a design became the workspace's private draft (Design → the Designer).</summary>
    public event EventHandler? GraphicsDesignOpened;

    /// <summary>The design a draft belongs to (by template id), or null for a built-in or package look.</summary>
    public GraphicsDesign? GraphicsDesignFor(NativeGraphicsDraft? draft) => GraphicsDesignWorkflow.DesignFor(draft);

    public Task<GraphicsDesignResult> CreateGraphicsDataTableAsync(string name, GraphicsDataSource source, GraphicsDataBinding binding) =>
        GraphicsDesignWorkflow.CreateDataTableAsync(name, source, binding);

    public Task<GraphicsDesignResult> CreateGraphicsDesignAsync(string? kind, string? lookTemplateId, int lookTemplateVersion, string name) =>
        GraphicsDesignWorkflow.CreateAsync(kind, lookTemplateId, lookTemplateVersion, name);
    /// <summary>The design a playlist row shows; null for a built-in or imported look.</summary>
    public GraphicsDesign? GraphicsDesignForRow(GraphicsPlaylistRow row) => GraphicsDesignWorkflow.DesignForRow(row);
    /// <summary>"Open in Designer" for a design: show the Designer when the design is already the private draft
    /// (unsaved edits kept), else open it (its Opened event shows the Designer). Program is unchanged.</summary>
    public Task<GraphicsDesignResult> OpenGraphicsDesignInDesignerAsync(GraphicsDesign design) => GraphicsDesignWorkflow.OpenInDesignerAsync(design);
    /// <summary>A design message raised outside the builder (Operate's "Open in Design" that could not open the
    /// design); the builder shows it in its design status. Session-only.</summary>
    [CommunityToolkit.Mvvm.ComponentModel.ObservableProperty] private string _graphicsDesignNotice = "";
    public Task<GraphicsDesignResult> SaveGraphicsDesignAsync() => GraphicsDesignWorkflow.SaveAsync();
    public Task<GraphicsDesignResult> SaveGraphicsDesignToPlaylistAsync() => GraphicsDesignWorkflow.SaveToPlaylistAsync();
    /// <summary>Save the open design and put it on the first Guest row: the Zoom lower third (LT, lower-third hotkeys, automation).</summary>
    public Task<GraphicsDesignResult> UseGraphicsDesignForZoomLowerThirdsAsync() => GraphicsDesignWorkflow.UseForZoomLowerThirdsAsync();
    /// <summary>The F-key Save to playlist will use for <paramref name="design"/> ("" when none is free).</summary>
    public string GraphicsDesignPlaylistHotkey(GraphicsDesign design) => GraphicsDesignWorkflow.PlaylistHotkey(design, GraphicsLibrary.Document.Playlist);

    public Task<GraphicsDesignResult> DuplicateGraphicsDesignAsync(GraphicsDesign design)
    {
        var name = design.Name + " copy";
        return GraphicsDesigns.DuplicateAsync(design.DesignId, name.Length > GraphicsDesignIds.MaxNameLength ? name[..GraphicsDesignIds.MaxNameLength].TrimEnd() : name);
    }

    public Task<GraphicsDesignResult> DeleteGraphicsDesignAsync(string designId) => GraphicsDesignWorkflow.DeleteAsync(designId);

    // ---- core restart -----------------------------------------------------------------------
    /// <summary>Called from the core-generation hook (OnBridgeProfileChanged, which also fires on the
    /// first handshake): the native registry is in memory, so design versions the playlist or saved
    /// cues pin are installed again once the new core's graphics state arrives.</summary>
    private void ArmGraphicsDesignResync()
    {
        if (_graphicsDesignResync.Arm(_bridge.LastSnapshot?.Graphics)) _ = EnsureGraphicsDesignsInstalledAsync();
    }

    private void ResyncGraphicsDesigns(NativeGraphicsState? state)
    {
        if (_graphicsDesignResync.Observe(state)) _ = EnsureGraphicsDesignsInstalledAsync();
    }

    private async Task EnsureGraphicsDesignsInstalledAsync()
    {
        try {
            var result = await GraphicsDesigns.EnsureInstalledForPlaylistAsync();
            if (!result.Ok) CommandStatus = "Graphics designs: " + result.Message;
        } catch (Exception ex) when (ex is not OutOfMemoryException) { CommandStatus = "Graphics designs could not be reinstalled: " + ex.Message; }
    }
}
