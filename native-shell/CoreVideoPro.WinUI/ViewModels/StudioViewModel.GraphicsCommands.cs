using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private GraphicsCommandGateway? _graphicsCommands;
    private GraphicsWorkspaceSession? _graphicsWorkspace;
    private GraphicsWorkspaceLibrary? _graphicsLibrary;
    public GraphicsWorkspaceSession GraphicsWorkspace => _graphicsWorkspace ??= CreateGraphicsWorkspace();
    public GraphicsWorkspaceLibrary GraphicsLibrary => _graphicsLibrary ??= new(new GraphicsDocumentStore(
        Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH") ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro", "graphics", "default.json")), GraphicsWorkspace);

    private GraphicsWorkspaceSession CreateGraphicsWorkspace()
    {
        var workspace = new GraphicsWorkspaceSession(InvokeGraphicsActionAsync, () => _bridge.LastSnapshot?.Graphics);
        workspace.Observe(_bridge.LastSnapshot?.Graphics);
        return workspace;
    }

    public Task<NativeGraphicsOperationResult> InvokeGraphicsActionAsync(string action, string request,
        CancellationToken cancellationToken = default) =>
        (_graphicsCommands ??= new GraphicsCommandGateway(_bridge)).InvokeAsync(action, request, cancellationToken);
}
