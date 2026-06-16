using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class SettingsViewModel : ObservableObject
{
    private readonly MediaCoreBridgeService _bridge;
    private readonly Func<bool> _engineRunning;
    private readonly Action<string>? _zoomStatusChanged;

    [ObservableProperty]
    private string _joinMeetingUrl = "https://zoom.us/j/123456789";

    [ObservableProperty]
    private string _displayName = "CoreVideo Producer";

    [ObservableProperty]
    private bool _isWebinar = true;

    [ObservableProperty]
    private ZoomMeetingState _meetingState = ZoomMeetingState.InMeeting;

    [ObservableProperty]
    private string _joinStatus = "Ready";

    [ObservableProperty]
    private int _liveParticipantCount;

    [ObservableProperty]
    private bool _sdkDiagnosticsExpanded;

    [ObservableProperty]
    private string _activationKey = string.Empty;

    [ObservableProperty]
    private LicenseTier _licenseTier = LicenseTier.Trial;

    [ObservableProperty]
    private LicenseStatus _licenseStatus = LicenseStatus.Trial;

    [ObservableProperty]
    private string _licenseActionStatus = string.Empty;

    private ZoomSdkReadinessReport _sdkReadiness = CreateDefaultReport();

    public SettingsViewModel(
        MediaCoreBridgeService bridge,
        Func<bool> engineRunning,
        Action<string>? zoomStatusChanged = null)
    {
        _bridge = bridge;
        _engineRunning = engineRunning;
        _zoomStatusChanged = zoomStatusChanged;
        RefreshSdkReadiness();
    }

    public ZoomSdkReadinessReport SdkReadiness => _sdkReadiness;

    public IReadOnlyList<ZoomSdkReadinessCheck> SdkChecks => _sdkReadiness.Checks;

    public IReadOnlyList<string> SdkBlockers => _sdkReadiness.Blockers;

    public bool IsInMeeting => MeetingState == ZoomMeetingState.InMeeting;

    public bool CanEditJoinFields => !IsInMeeting;

    public bool ShowLeaveButton => IsInMeeting;

    public bool ShowJoinButton => !IsInMeeting;

    public bool JoinBlockedBySdk => ZoomSdkReadinessService.ShouldBlockZoomJoin(_engineRunning(), _sdkReadiness);

    public bool CanJoinZoom => !JoinBlockedBySdk;

    public string JoinBlockedReason => _sdkReadiness.Blockers.FirstOrDefault() ?? _sdkReadiness.Summary;

    public bool ShowLicenseActionStatus => !string.IsNullOrWhiteSpace(LicenseActionStatus);

    public string SdkChipLabel =>
        _sdkReadiness.Status == ZoomSdkReadinessStatus.Ready ? "SDK ready" : _sdkReadiness.Summary;

    public bool SdkIsReady => _sdkReadiness.Status == ZoomSdkReadinessStatus.Ready;

    public bool SdkIsWarning => _sdkReadiness.Status == ZoomSdkReadinessStatus.Warning;

    public bool SdkIsBlocked => _sdkReadiness.Status == ZoomSdkReadinessStatus.Blocked;

    public bool ShowSdkChecklist => !SdkIsReady || SdkDiagnosticsExpanded;

    public bool ShowSdkDetailsButton => !SdkIsReady || SdkDiagnosticsExpanded;

    public string SdkDetailsButtonLabel => SdkDiagnosticsExpanded ? "Hide details" : "Show details";

    public string LicensePlanLabel => LicenseCatalog.TierLabel(LicenseTier);

    public string LicenseStatusLabel => LicenseStatus.ToString().ToLowerInvariant();

    public string TrialDaysLabel
    {
        get
        {
            if (LicenseStatus != LicenseStatus.Trial)
            {
                return string.Empty;
            }

            var trialEnd = DateTimeOffset.UtcNow.AddDays(14).ToUnixTimeMilliseconds();
            var days = Math.Max(0, (int)Math.Ceiling((trialEnd - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()) / 86_400_000d));
            return $" — {days} days left";
        }
    }

    public string EntitlementsSummary
    {
        get
        {
            var entitlements = LicenseCatalog.DeriveEntitlements(
                new LicenseState { Tier = LicenseTier, Status = LicenseStatus },
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());

            var parts = new List<string>
            {
                $"up to {entitlements.MaxZoomParticipants} participants",
                $"{entitlements.MaxOutputHeight}p output"
            };

            if (entitlements.Watermark)
            {
                parts.Add("watermark");
            }

            if (!entitlements.SetAndForget)
            {
                parts.Add("Set & Forget locked");
            }

            if (!entitlements.ChromaKey)
            {
                parts.Add("chroma key locked");
            }

            if (!entitlements.Phase2Outputs)
            {
                parts.Add("NDI/SRT/WebRTC locked");
            }

            return $"Limits: {string.Join(", ", parts)}";
        }
    }

    public string MeetingStatusLine
    {
        get
        {
            var participantCount = _engineRunning() && LiveParticipantCount > 0
                ? LiveParticipantCount
                : DemoProduction.Participants.Count;
            var room = DemoProduction.CurrentRoom();
            var stateLabel = MeetingState.ToString().Replace('_', ' ');
            return $"{stateLabel} - {participantCount} participants - {room.Name} - Screen share active - Captions on - 1920×1080 60fps - 28:47";
        }
    }

    public bool CanActivateLicense => !string.IsNullOrWhiteSpace(ActivationKey);

    partial void OnMeetingStateChanged(ZoomMeetingState value)
    {
        NotifyMeetingUi();
        _zoomStatusChanged?.Invoke(value == ZoomMeetingState.InMeeting ? "Zoom Connected" : "Zoom Offline");
    }

    partial void OnSdkDiagnosticsExpandedChanged(bool value) => NotifySdkUi();

    partial void OnActivationKeyChanged(string value) => OnPropertyChanged(nameof(CanActivateLicense));

    partial void OnLicenseActionStatusChanged(string value) => OnPropertyChanged(nameof(ShowLicenseActionStatus));

    public void RefreshSdkReadiness()
    {
        var input = ZoomSdkReadinessService.DeriveInputForEngine(_engineRunning());
        _sdkReadiness = ZoomSdkReadinessService.Assess(input);
        NotifySdkUi();
        OnPropertyChanged(nameof(JoinBlockedBySdk));
        OnPropertyChanged(nameof(CanJoinZoom));
        OnPropertyChanged(nameof(JoinBlockedReason));
    }

    [RelayCommand]
    private async Task JoinZoomAsync()
    {
        if (JoinBlockedBySdk)
        {
            JoinStatus = JoinBlockedReason;
            return;
        }

        MeetingState = ZoomMeetingState.Joining;
        JoinStatus = "Joining meeting…";

        try
        {
            if (!_engineRunning())
            {
                MeetingState = ZoomMeetingState.InMeeting;
                LiveParticipantCount = DemoProduction.Participants.Count;
                JoinStatus = $"Join queued (demo): {DisplayName.Trim()} → {JoinMeetingUrl.Trim()}";
                NotifyMeetingUi();
                return;
            }

            var snapshot = await _bridge.JoinZoomAsync(
                JoinMeetingUrl.Trim(),
                string.IsNullOrWhiteSpace(DisplayName) ? "CoreVideo Producer" : DisplayName.Trim(),
                IsWebinar).ConfigureAwait(true);
            ApplyCaptureSnapshot(snapshot);
            JoinStatus = MediaCoreBridgeService.SummarizeJoinLeaveMessage(snapshot, "Joined");
        }
        catch (Exception ex)
        {
            MeetingState = ZoomMeetingState.Error;
            JoinStatus = ex.Message;
            NotifyMeetingUi();
        }
    }

    [RelayCommand]
    private async Task LeaveZoomAsync()
    {
        JoinStatus = "Leaving meeting…";
        try
        {
            if (!_engineRunning())
            {
                MeetingState = ZoomMeetingState.Idle;
                LiveParticipantCount = 0;
                JoinStatus = "Left Zoom meeting (demo).";
                NotifyMeetingUi();
                return;
            }

            var snapshot = await _bridge.LeaveZoomAsync().ConfigureAwait(true);
            ApplyCaptureSnapshot(snapshot);
            JoinStatus = MediaCoreBridgeService.SummarizeJoinLeaveMessage(snapshot, "Left");
        }
        catch (Exception ex)
        {
            MeetingState = ZoomMeetingState.Error;
            JoinStatus = ex.Message;
            NotifyMeetingUi();
        }
    }

    public void ApplyCaptureSnapshot(RawCaptureSnapshot snapshot)
    {
        MeetingState = ParseMeetingState(snapshot.MeetingState);
        LiveParticipantCount = snapshot.Participants.Count;
        NotifyMeetingUi();
    }

    public void ApplyMeetingStateLabel(string? meetingStateLabel, int participantCount = 0)
    {
        if (string.IsNullOrWhiteSpace(meetingStateLabel))
        {
            return;
        }

        MeetingState = ParseMeetingState(meetingStateLabel);
        if (participantCount > 0)
        {
            LiveParticipantCount = participantCount;
        }

        NotifyMeetingUi();
    }

    [RelayCommand]
    private void ToggleSdkDiagnostics() => SdkDiagnosticsExpanded = !SdkDiagnosticsExpanded;

    [RelayCommand]
    private void ActivateLicense()
    {
        if (!CanActivateLicense)
        {
            return;
        }

        LicenseActionStatus = $"Activation queued for key {ActivationKey.Trim()}";
    }

    [RelayCommand]
    private void StartTrial()
    {
        LicenseTier = LicenseTier.Trial;
        LicenseStatus = LicenseStatus.Trial;
        LicenseActionStatus = "Trial started.";
        NotifyLicenseUi();
    }

    [RelayCommand]
    private void Upgrade(string tier)
    {
        if (!Enum.TryParse<LicenseTier>(tier, ignoreCase: true, out var parsed))
        {
            return;
        }

        LicenseTier = parsed;
        LicenseStatus = LicenseStatus.Active;
        LicenseActionStatus = $"Upgrade to {LicenseCatalog.TierLabel(parsed)} queued.";
        NotifyLicenseUi();
    }

    private void NotifyMeetingUi()
    {
        OnPropertyChanged(nameof(IsInMeeting));
        OnPropertyChanged(nameof(CanEditJoinFields));
        OnPropertyChanged(nameof(ShowLeaveButton));
        OnPropertyChanged(nameof(ShowJoinButton));
        OnPropertyChanged(nameof(MeetingStatusLine));
    }

    private void NotifySdkUi()
    {
        OnPropertyChanged(nameof(SdkReadiness));
        OnPropertyChanged(nameof(SdkChecks));
        OnPropertyChanged(nameof(SdkBlockers));
        OnPropertyChanged(nameof(SdkChipLabel));
        OnPropertyChanged(nameof(SdkIsReady));
        OnPropertyChanged(nameof(SdkIsWarning));
        OnPropertyChanged(nameof(SdkIsBlocked));
        OnPropertyChanged(nameof(ShowSdkChecklist));
        OnPropertyChanged(nameof(ShowSdkDetailsButton));
        OnPropertyChanged(nameof(SdkDetailsButtonLabel));
    }

    private void NotifyLicenseUi()
    {
        OnPropertyChanged(nameof(LicensePlanLabel));
        OnPropertyChanged(nameof(LicenseStatusLabel));
        OnPropertyChanged(nameof(TrialDaysLabel));
        OnPropertyChanged(nameof(EntitlementsSummary));
    }

    private static ZoomSdkReadinessReport CreateDefaultReport() =>
        ZoomSdkReadinessService.Assess(ZoomSdkReadinessService.CreateEmbeddedInput());

    private static ZoomMeetingState ParseMeetingState(string? meetingState) =>
        meetingState?.Trim().ToLowerInvariant() switch
        {
            "in_meeting" or "in-meeting" or "joining" => ZoomMeetingState.InMeeting,
            "idle" or "leaving" => ZoomMeetingState.Idle,
            "reconnecting" => ZoomMeetingState.Reconnecting,
            "error" => ZoomMeetingState.Error,
            _ => ZoomMeetingState.Idle
        };
}