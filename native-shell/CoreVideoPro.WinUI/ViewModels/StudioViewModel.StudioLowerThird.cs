using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private StudioLowerThirdController? _studioLowerThird;
    public StudioLowerThirdController StudioLowerThird => _studioLowerThird ??= CreateStudioLowerThird();
    public string StudioAssignedLowerThirdLabel => StudioLowerThird.Label;
    public string StudioAssignedLowerThirdDetail => StudioLowerThird.Detail;
    public bool StudioLowerThirdBusy => StudioLowerThird.Busy || GraphicsLibrary.Busy;

    private StudioLowerThirdController CreateStudioLowerThird()
    {
        var controller = new StudioLowerThirdController(GraphicsLibrary, InvokeGraphicsActionAsync, () => _bridge.LastSnapshot?.Graphics);
        controller.Changed += (_, _) => {
            OnPropertyChanged(nameof(StudioAssignedLowerThirdLabel));
            OnPropertyChanged(nameof(StudioAssignedLowerThirdDetail));
            OnPropertyChanged(nameof(StudioLowerThirdBusy));
        };
        _ = GraphicsLibrary.LoadAsync();
        return controller;
    }

    public async Task<bool> AssignGraphicsLookToStudioAsync(bool followProgramSource)
    {
        var result = await GraphicsLibrary.AssignStudioLowerThirdAsync(followProgramSource);
        CommandStatus = GraphicsLibrary.Status;
        return result;
    }

    public async Task<NativeGraphicsOperationResult> ApplyStudioLowerThirdIntentAsync(bool visible)
    {
        await GraphicsLibrary.LoadAsync();
        var current = ManagedLowerThirdAuthority ? ManagedLowerThirdVisible : ProgramLowerThirdKey.IsVisible || ProgramLowerThirdEnabled;
        if (visible && StudioLowerThird.Assignment is not null) {
            if (current) return new() { Accepted = true, Reason = "The current lower third remains on air. The assigned look applies on the next Show." };
            return await ShowAssignedStudioLowerThirdAsync();
        }
        if (ManagedLowerThirdAuthority) return await ApplyManagedLowerThirdIntentAsync(visible);
        if (current != visible) ToggleLegacyProgramLowerThird();
        var applied = ProgramLowerThirdEnabled;
        return new() { Accepted = applied == visible, Reason = CommandStatus };
    }

    private async Task<NativeGraphicsOperationResult> ShowAssignedStudioLowerThirdAsync()
    {
        var result = await StudioLowerThird.ShowAsync(ActiveSceneId, sourceId => {
            var state = _bridge.LastSnapshot?.Graphics;
            var id = sourceId ?? ResolveProgramLowerThirdSource(ProgramSceneRoutes)?.SourceId;
            var source = state?.Sources.FirstOrDefault(s => s.VideoOn && s.Identity?.SourceId == id);
            if (source?.Identity is not { } identity) return null;
            var text = GraphicsSourceText(identity.SourceId, source.DisplayName);
            return new(identity, text.Name, text.Title);
        });
        CommandStatus = result.Reason;
        // Native observation, not an accepted command, owns on-air state.
        return result;
    }
}
