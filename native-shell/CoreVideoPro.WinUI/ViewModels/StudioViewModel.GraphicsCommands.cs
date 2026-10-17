using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private GraphicsCommandGateway? _graphicsCommands;

    public Task<NativeGraphicsOperationResult> InvokeGraphicsActionAsync(string action, string request,
        CancellationToken cancellationToken = default) =>
        (_graphicsCommands ??= new GraphicsCommandGateway(_bridge)).InvokeAsync(action, request, cancellationToken);
}
