using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    [ObservableProperty]
    private bool _lowerThirdPhaseSyncPending;

    partial void OnLowerThirdPhaseSyncPendingChanged(bool value)
    {
        OnPropertyChanged(nameof(LowerThirdKeyStatus));
        OnPropertyChanged(nameof(LowerThirdKeySummary));
        OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
        OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
        OnPropertyChanged(nameof(StudioLowerThirdToolTip));
    }

    private void RecoverLowerThirdPhaseSyncFailure(Exception error)
    {
        LowerThirdPhaseSyncPending = true;
        ProgramLowerThirdKey = LowerThirdPhaseRecovery.SettleDesiredState(ProgramLowerThirdKey);
        if (!ProgramLowerThirdKey.IsVisible) _lowerThirdTargetSourceId = string.Empty;
        CommandStatus = $"Lower third sync pending: {error.Message}";
        LaunchLog.Write($"lower-third: phase sync failed; desired phase={ProgramLowerThirdKey.Phase}; {error.Message}");
    }

    private void ReconcileLowerThirdPhaseSync(NativeMediaCoreStateSnapshot snapshot)
    {
        if (!LowerThirdPhaseSyncPending || !LowerThirdPhaseRecovery.IsObserved(ProgramLowerThirdKey, snapshot.OverlayState)) return;
        LowerThirdPhaseSyncPending = false;
        if (CommandStatus.StartsWith("Lower third sync pending:", StringComparison.Ordinal))
            CommandStatus = "Lower third synchronized with media core";
    }
}
