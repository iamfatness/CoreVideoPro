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

    // ---- Graphics playout routing (issue #1076, Stage C1) --------------------------------
    /// <summary>True when the Graphics playlist has a Guest row: every legacy lower-third input
    /// (footer LT button, graphics.lowerThird.*, automation.lowerThirds.set) then routes into it.</summary>
    public bool HasPlaylistGuestRow => GraphicsPlayout.HasGuestRow;
    /// <summary>Canonical source id of the current Program lower-third subject (same resolver as the legacy key), or null.</summary>
    public string? ProgramLowerThirdSubjectSourceId => ResolveProgramLowerThirdSource(ProgramSceneRoutes)?.SourceId;
    /// <summary>The Guest-row entry for the current Program subject; null when it is not bound to a Show Input.</summary>
    public GraphicsPlaylistEntry? ProgramSubjectGuestEntry => GuestEntryFor(ProgramLowerThirdSubjectSourceId);
    /// <summary>Strip evidence (Stage C2): a lower-third chip is on Program. Never local intent.</summary>
    public bool GraphicsLowerThirdOnAir => GraphicsPlayout.LowerThirdOnAir;
    /// <summary>What "LT is on" means for a toggle, by authority: the playlist strip when a Guest
    /// row exists, managed instances under instances authority, else the legacy key.</summary>
    public bool LowerThirdIntentVisible => HasPlaylistGuestRow ? GraphicsLowerThirdOnAir
        : ManagedLowerThirdAuthority ? ManagedLowerThirdVisible : ProgramLowerThirdKey.IsVisible || ProgramLowerThirdEnabled;

    public async Task<NativeGraphicsOperationResult> ApplyStudioLowerThirdIntentAsync(bool visible)
    {
        await GraphicsLibrary.LoadAsync();
        if (HasPlaylistGuestRow) {
            // Playlist route: Guest row of the Program subject. Authority flips to instances only
            // after the first accepted Play In (see the plan's Risks); the legacy path stays
            // reachable for shows without a playlist.
            var routed = await GraphicsPlayout.ApplyLowerThirdIntentAsync(visible, ProgramSubjectGuestEntry, ManagedLowerThirdAuthority,
                _ => GraphicsWorkspace.SetAuthorityAsync("lower-third", "instances"));
            CommandStatus = routed.Reason;
            if (routed.Accepted) _programLowerThirdAutomationSuppressed = !visible;
            return routed;
        }
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
