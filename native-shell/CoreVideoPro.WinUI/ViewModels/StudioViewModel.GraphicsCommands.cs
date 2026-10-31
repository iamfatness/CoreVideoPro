using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private GraphicsCommandGateway? _graphicsCommands;
    private GraphicsWorkspaceSession? _graphicsWorkspace;
    private GraphicsWorkspaceLibrary? _graphicsLibrary;
    public (string Name, string Title) GraphicsSourceText(string sourceId, string fallback)
    {
        var participant = RoomVideoParticipants.FirstOrDefault(p => "zoom:" + p.Id == sourceId);
        var capture = CaptureDevices.FirstOrDefault(d => "capture:" + d.Id == sourceId);
        var name = ResolveSourceDisplayName(sourceId, participant?.Name ?? capture?.Name ?? fallback);
        var title = participant is null ? "" : string.IsNullOrWhiteSpace(participant.Title) ? participant.RoleLabel : participant.Title;
        var org = participant is null ? "" : string.IsNullOrWhiteSpace(participant.BreakoutRoomName) ? participant.RoleLabel : participant.BreakoutRoomName;
        var resolved = Services.SourceLowerThirdText.Resolve(_sourceSecondaryLines, sourceId, title, org);
        return (name, Services.SourceLowerThirdText.Preview(resolved.Title, resolved.Org));
    }
    public GraphicsAutomationConfiguration GraphicsAutomationConfiguration(IReadOnlyList<GraphicsAutomationRule> rules)
    {
        var texts=new Dictionary<string,GraphicsAutomationSourceText>(StringComparer.Ordinal);
        foreach(var source in GraphicsWorkspace.State?.Sources ?? []) {
            if(source.Identity is not { } identity || !source.VideoOn) continue;
            var text=GraphicsSourceText(identity.SourceId,source.DisplayName);
            texts[identity.SourceId]=new() {Binding=identity,Name=identity.SourceId.StartsWith("capture:",StringComparison.Ordinal)?text.Name:_sourceDisplayNames.TryGetValue(identity.SourceId,out var name)?name:null,Secondary=text.Title};
        }
        return new() {Rules=rules,SourceTexts=texts};
    }
    public GraphicsWorkspaceSession GraphicsWorkspace => _graphicsWorkspace ??= CreateGraphicsWorkspace();
    public GraphicsWorkspaceLibrary GraphicsLibrary => _graphicsLibrary ??= new(new GraphicsDocumentStore(
        Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH") ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro", "graphics", "default.json")), GraphicsWorkspace);

    private GraphicsWorkspaceSession CreateGraphicsWorkspace()
    {
        var workspace = new GraphicsWorkspaceSession(InvokeGraphicsActionAsync, () => _bridge.LastSnapshot?.Graphics);
        // Seed the session from the latest snapshot: ObserveGraphicsState only feeds a
        // workspace that already exists, so a first access from Playout (no Design tab
        // opened yet) otherwise reads State == null for up to one snapshot tick and the
        // playlist install answers "Native graphics is unavailable" (owner, 2026-10-31).
        workspace.Observe(_bridge.LastSnapshot?.Graphics);
        return workspace;
    }

    public Task<NativeGraphicsOperationResult> InvokeGraphicsActionAsync(string action, string request,
        CancellationToken cancellationToken = default) =>
        (_graphicsCommands ??= new GraphicsCommandGateway(_bridge)).InvokeAsync(action, request, cancellationToken);
}
