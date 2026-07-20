using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels.MagicScene;

/// <summary>
/// A minimal UI-thread timer seam so <see cref="MagicSceneCoordinator"/> can drive the
/// automation policy loop without a hard dependency on <c>DispatcherQueueTimer</c>. The
/// production implementation (in StudioViewModel) wraps a UI-thread
/// <c>DispatcherQueueTimer</c>; tests supply a fake so the coordinator is constructible
/// without a dispatcher.
/// </summary>
public interface IAutomationTimer
{
    bool IsRunning { get; }

    void Start();

    void Stop();
}

/// <summary>
/// The cross-cluster surface <see cref="MagicSceneCoordinator"/> needs from the owning
/// <c>StudioViewModel</c>. StudioViewModel implements this over <c>this</c> (the
/// Overlays/Transport sub-VM pattern). Keeping it an interface is what makes the
/// coordinator independently constructible for the characterization tests the god object
/// never allowed.
/// </summary>
public interface IMagicSceneHost
{
    // --- meeting / scene snapshot ---
    int RoomVideoParticipantCount { get; }

    IReadOnlyList<Scene> Scenes { get; }

    string ActiveSceneId { get; }

    string PreviewSceneId { get; set; }

    bool CanTake { get; }

    bool IsInMeeting { get; }

    // --- command status + take ---
    string CommandStatus { set; }

    Task TakeAsync();

    // --- scene refresh ---
    void SchedulePreviewRoutingRefresh();

    void RefreshSceneItems();

    void RefreshProductionReadouts();

    // --- transport + license gate ---
    void ApplyTransportAutomationState(ProductionMode mode, bool canToggle);

    bool IsSetAndForgetEntitled();

    // --- overlay policy hooks ---
    bool SetAutomationGraphic(string kind, bool enabled);

    void RefreshProgramLowerThirdKeyPosition();

    string CaptionText { get; set; }

    string CaptionSpeaker { get; set; }

    void NotifyEnabledGraphicsChanged();

    void RequestMediaCoreSync();

    // --- show inputs + display readouts ---
    void ReapplyShowInputAutoAssign();

    void NotifyAutomationPolicySummaries();

    // --- timer factory (UI-thread DispatcherQueueTimer in production) ---
    IAutomationTimer CreateAutomationTimer(Action onTick);
}
