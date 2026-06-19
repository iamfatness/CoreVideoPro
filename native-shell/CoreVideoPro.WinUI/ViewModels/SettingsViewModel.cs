using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Dispatching;


namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class SettingsViewModel : ObservableObject
{
    private readonly MediaCoreBridgeService _bridge;
    private readonly ZoomOAuthService? _oauth;
    private readonly Func<bool> _captureRunning;
    private readonly Action? _onMeetingPresenceChanged;
    private readonly Func<Task>? _onBeforeLeaveMeeting;
    private readonly Action<string>? _zoomStatusChanged;
    private readonly Action? _drainOAuthCallback;
    private readonly Action? _onMeetingJoined;
    private readonly DispatcherQueue _dispatcher = DispatcherQueue.GetForCurrentThread();

    [ObservableProperty]
    private string _joinMeetingUrl = "https://zoom.us/j/123456789";

    [ObservableProperty]
    private string _displayName = "CoreVideo Producer";

    [ObservableProperty]
    private bool _isWebinar = false;

    [ObservableProperty]
    private ZoomMeetingState _meetingState = ZoomMeetingState.Idle;

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

    [ObservableProperty]
    private string _oauthStatusMessage = string.Empty;

    [ObservableProperty]
    private bool _zoomOAuthSignedIn;

    private ZoomSdkReadinessReport _sdkReadiness = CreateDefaultReport();
    private IReadOnlyList<ZoomEngineEvidenceItem> _zoomEngineEvidence = [];
    private FirstFrameValidationEvidence _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From();

    public SettingsViewModel(
        MediaCoreBridgeService bridge,
        Func<bool> captureRunning,
        ZoomOAuthService? oauth = null,
        Action? drainOAuthCallback = null,
        Action? onMeetingPresenceChanged = null,
        Func<Task>? onBeforeLeaveMeeting = null,
        Action<string>? zoomStatusChanged = null,
        Action? onMeetingJoined = null)
    {
        _bridge = bridge;
        _oauth = oauth;
        _captureRunning = captureRunning;
        _drainOAuthCallback = drainOAuthCallback;
        _onMeetingPresenceChanged = onMeetingPresenceChanged;
        _onBeforeLeaveMeeting = onBeforeLeaveMeeting;
        _zoomStatusChanged = zoomStatusChanged;
        _onMeetingJoined = onMeetingJoined;
        _ = RefreshOAuthStatusAsync();
        RefreshSdkReadiness();
    }

    public bool ShowZoomOAuthControls => _oauth?.Manifest.BrokerConfigured == true;

    public bool CanSignInWithZoom => ShowZoomOAuthControls && !ZoomOAuthSignedIn;

    public bool CanSignOutZoom => ShowZoomOAuthControls && ZoomOAuthSignedIn;

    public ZoomSdkReadinessReport SdkReadiness => _sdkReadiness;

    public IReadOnlyList<ZoomSdkReadinessCheck> SdkChecks => _sdkReadiness.Checks;

    public IReadOnlyList<string> SdkBlockers => _sdkReadiness.Blockers;

    public IReadOnlyList<ZoomEngineEvidenceItem> ZoomEngineEvidence => _zoomEngineEvidence;

    public bool IsInMeeting => MeetingState == ZoomMeetingState.InMeeting;

    public bool CanEditJoinFields => !IsInMeeting;

    public bool ShowLeaveButton => IsInMeeting;

    public bool ShowJoinButton => !IsInMeeting;

    public bool JoinBlockedBySdk => ZoomSdkReadinessService.ShouldBlockZoomJoin(_captureRunning(), _sdkReadiness);

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
            var stateLabel = MeetingState.ToString().Replace('_', ' ');
            var participantLabel = LiveParticipantCount == 1 ? "participant" : "participants";
            return IsInMeeting
                ? $"{stateLabel} · {LiveParticipantCount} {participantLabel}"
                : $"{stateLabel} · not in a meeting";
        }
    }

    public bool CanActivateLicense => !string.IsNullOrWhiteSpace(ActivationKey);

    partial void OnMeetingStateChanged(ZoomMeetingState value)
    {
        NotifyMeetingUi();
        _zoomStatusChanged?.Invoke(value == ZoomMeetingState.InMeeting ? "Zoom Live" : "Zoom Offline");
    }

    partial void OnSdkDiagnosticsExpandedChanged(bool value) => NotifySdkUi();

    partial void OnActivationKeyChanged(string value) => OnPropertyChanged(nameof(CanActivateLicense));

    partial void OnLicenseActionStatusChanged(string value) => OnPropertyChanged(nameof(ShowLicenseActionStatus));

    public async Task RefreshOAuthStatusAsync()
    {
        if (_oauth is null)
        {
            RunOnUiThread(() =>
            {
                ZoomOAuthSignedIn = false;
                NotifyOAuthUi();
            });
            return;
        }

        var status = await _oauth.GetStatusAsync().ConfigureAwait(false);
        RunOnUiThread(() =>
        {
            ZoomOAuthSignedIn = status.SignedIn;
            NotifyOAuthUi();
            RefreshSdkReadiness();
        });
    }

    public void RefreshSdkReadiness()
    {
        var input = ZoomSdkReadinessService.DeriveInputForEngine(_bridge.Running, ZoomOAuthSignedIn);
        _sdkReadiness = ZoomSdkReadinessService.Assess(input);
        RefreshZoomEngineEvidence(_bridge.LastSnapshot);
        NotifySdkUi();
        OnPropertyChanged(nameof(JoinBlockedBySdk));
        OnPropertyChanged(nameof(CanJoinZoom));
        OnPropertyChanged(nameof(JoinBlockedReason));
    }

    public void RefreshZoomEngineEvidence(NativeMediaCoreStateSnapshot? snapshot = null)
    {
        if (snapshot is not null)
        {
            _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From(
                existing: _firstFrameEvidence,
                snapshot: snapshot);
        }

        _zoomEngineEvidence = BuildZoomEngineEvidence(snapshot ?? _bridge.LastSnapshot);
        OnPropertyChanged(nameof(ZoomEngineEvidence));
    }

    public void ObserveZoomVideoFrame(ZoomVideoFrame frame)
    {
        _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From(
            existing: _firstFrameEvidence,
            zoomVideoFrame: frame);
        RefreshZoomEngineEvidence(_bridge.LastSnapshot);
    }

    public void ObserveProgramPreviewFrame(ProgramFramePreview preview)
    {
        _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From(
            existing: _firstFrameEvidence,
            programPreviewFrame: preview);
        RefreshZoomEngineEvidence(_bridge.LastSnapshot);
    }

    [RelayCommand]
    private async Task JoinZoomAsync()
    {
        if (JoinBlockedBySdk)
        {
            SetJoinFailure(JoinBlockedReason);
            LaunchLog.Write($"zoom-join: blocked ({JoinBlockedReason})");
            return;
        }

        if (ShowZoomOAuthControls && !ZoomOAuthSignedIn)
        {
            SetJoinFailure("Sign in with Zoom before joining a meeting.");
            LaunchLog.Write("zoom-join: blocked (Zoom account not signed in)");
            return;
        }

        SetJoinProgress(ZoomMeetingState.Joining, "Joining meeting…");
        LaunchLog.Write($"zoom-join: starting url={JoinMeetingUrl.Trim()}");

        try
        {
            _drainOAuthCallback?.Invoke();
            await RefreshOAuthStatusAsync().ConfigureAwait(false);

            if (!_bridge.Running)
            {
                SetJoinProgress(ZoomMeetingState.Joining, "Starting media core…");
                LaunchLog.Write("zoom-join: starting media core");
                var profile = await _bridge.StartAsync().ConfigureAwait(false);
                LaunchLog.Write(
                    $"zoom-join: media core ready profile={(profile?.Name ?? "unknown")} renderer={(profile?.Renderer ?? "unknown")}");
            }

            string? sdkJwt = null;
            string? userZak = null;
            if (_oauth is not null)
            {
                try
                {
                    var creds = await _oauth.EnsureJoinCredentialsAsync().ConfigureAwait(false);
                    sdkJwt = creds.SdkJwt;
                    userZak = creds.UserZak;
                    LaunchLog.Write(
                        $"zoom-join: credentials usePublicAppKey={creds.UsePublicAppKey} jwt={(string.IsNullOrWhiteSpace(sdkJwt) ? "none" : "set")} zak={(string.IsNullOrWhiteSpace(userZak) ? "none" : "set")}");

                    if (!creds.UsePublicAppKey && string.IsNullOrWhiteSpace(sdkJwt))
                    {
                        const string missingJwt = "Meeting SDK JWT was not returned by the OAuth broker. Sign out and sign in again.";
                        LaunchLog.Write($"zoom-join: {missingJwt}");
                        SetJoinFailure(missingJwt);
                        return;
                    }
                }
                catch (Exception authEx)
                {
                    LaunchLog.Write($"zoom-join: credential error {authEx.Message}");
                    SetJoinFailure(authEx.Message);
                    return;
                }
            }
            else
            {
                LaunchLog.Write("zoom-join: credentials oauth=unavailable (public app key only)");
            }

            SetJoinProgress(ZoomMeetingState.Joining, "Joining meeting… (up to 60s)");
            var snapshot = await _bridge.JoinZoomAsync(
                JoinMeetingUrl.Trim(),
                string.IsNullOrWhiteSpace(DisplayName) ? "CoreVideo Producer" : DisplayName.Trim(),
                IsWebinar,
                sdkJwt,
                userZak).ConfigureAwait(false);

            LaunchLog.Write(
                $"zoom-join: snapshot meetingState={snapshot.MeetingState} participants={snapshot.Participants.Count} warnings={(snapshot.Warnings?.Count ?? 0)}");

            if (!IsSuccessfulJoinState(snapshot.MeetingState))
            {
                var failure = MediaCoreBridgeService.SummarizeJoinLeaveMessage(snapshot, "Join");
                LaunchLog.Write($"zoom-join: {failure}");
                SetJoinFailure(failure);
                return;
            }

            RunOnUiThread(() =>
            {
                ApplyCaptureSnapshot(snapshot);
                JoinStatus = MediaCoreBridgeService.SummarizeJoinLeaveMessage(snapshot, "Joined");
                LaunchLog.Write($"zoom-join: {JoinStatus}");
                _zoomStatusChanged?.Invoke("Zoom Live");
                _onMeetingJoined?.Invoke();
            });
        }
        catch (Exception ex)
        {
            var message = DescribeJoinException(ex);
            LaunchLog.Write($"zoom-join: failed {ex.GetType().Name}: {message}");
            SetJoinFailure(message);
        }
    }

    [RelayCommand]
    private async Task LeaveZoomAsync()
    {
        JoinStatus = "Leaving meeting…";
        try
        {
            if (_onBeforeLeaveMeeting is not null)
            {
                await _onBeforeLeaveMeeting().ConfigureAwait(true);
            }

            if (_bridge.Running)
            {
                var snapshot = await _bridge.LeaveZoomAsync().ConfigureAwait(true);
                ApplyCaptureSnapshot(snapshot);
                JoinStatus = MediaCoreBridgeService.SummarizeJoinLeaveMessage(snapshot, "Left");
                _bridge.Stop();
            }
            else
            {
                MeetingState = ZoomMeetingState.Idle;
                LiveParticipantCount = 0;
                JoinStatus = "Left Zoom meeting.";
            }

            NotifyMeetingUi();
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
        RefreshZoomEngineEvidence(_bridge.LastSnapshot);
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

        RefreshZoomEngineEvidence(_bridge.LastSnapshot);
        NotifyMeetingUi();
    }

    [RelayCommand]
    private async Task SignInWithZoomAsync()
    {
        if (_oauth is null)
        {
            OauthStatusMessage = "Zoom OAuth is not available in this shell build.";
            return;
        }

        try
        {
            LaunchLog.Write("oauth: sign-in requested");
            OauthStatusMessage = "Opening browser for Zoom sign-in…";
            await _oauth.BeginAuthorizationAsync().ConfigureAwait(true);
            LaunchLog.Write("oauth: browser launch completed");
            OauthStatusMessage = "Approve Zoom in your browser, then return to CoreVideo Pro.";

            for (var attempt = 0; attempt < 240; attempt++)
            {
                _drainOAuthCallback?.Invoke();
                var status = await _oauth.GetStatusAsync().ConfigureAwait(true);
                if (status.SignedIn)
                {
                    ZoomOAuthSignedIn = true;
                    OauthStatusMessage = "Signed in with Zoom.";
                    LaunchLog.Write("oauth: sign-in detected via status poll");
                    await RefreshOAuthStatusAsync().ConfigureAwait(true);
                    NotifyOAuthUi();
                    return;
                }

                await Task.Delay(500).ConfigureAwait(true);
            }

            OauthStatusMessage = "Still waiting for Zoom approval. If the browser already finished, click Sign in again.";
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"oauth: sign-in failed: {ex.Message}");
            OauthStatusMessage = ex.Message;
        }

        NotifyOAuthUi();
    }

    [RelayCommand]
    private async Task SignOutZoomAsync()
    {
        if (_oauth is null)
        {
            return;
        }

        await _oauth.SignOutAsync().ConfigureAwait(true);
        OauthStatusMessage = "Signed out of Zoom.";
        await RefreshOAuthStatusAsync().ConfigureAwait(true);
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
        _onMeetingPresenceChanged?.Invoke();
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
        OnPropertyChanged(nameof(ZoomEngineEvidence));
    }

    private void NotifyLicenseUi()
    {
        OnPropertyChanged(nameof(LicensePlanLabel));
        OnPropertyChanged(nameof(LicenseStatusLabel));
        OnPropertyChanged(nameof(TrialDaysLabel));
        OnPropertyChanged(nameof(EntitlementsSummary));
    }

    private void NotifyOAuthUi()
    {
        OnPropertyChanged(nameof(ShowZoomOAuthControls));
        OnPropertyChanged(nameof(CanSignInWithZoom));
        OnPropertyChanged(nameof(CanSignOutZoom));
    }

    private static ZoomSdkReadinessReport CreateDefaultReport() =>
        ZoomSdkReadinessService.Assess(ZoomSdkReadinessService.CreateEmbeddedInput());

    private IReadOnlyList<ZoomEngineEvidenceItem> BuildZoomEngineEvidence(NativeMediaCoreStateSnapshot? snapshot)
    {
        var nativeCorePath = MediaCorePaths.ResolveNativeCoreExecutable();
        var zoomEnginePath = MediaCorePaths.ResolveZoomEngineExecutable();
        var sdkRuntimePath = MediaCorePaths.ResolvePackagedZoomRuntimeDirectory()
                             ?? MediaCorePaths.ResolveZoomSdkArchitectureRoot();
        var stagedRuntimePath = MediaCorePaths.ResolveStagedZoomRuntimeTarget();
        var meetingState = !string.IsNullOrWhiteSpace(snapshot?.MeetingState)
            ? snapshot.MeetingState!
            : MeetingState.ToString().ToLowerInvariant();
        var participantCount = Math.Max(LiveParticipantCount, snapshot?.Participants.Count ?? 0);
        var source = snapshot?.SourceSnapshot;
        var lastFrameHint = DescribeLastFrameHint();
        var readinessHint = ResolveReadinessHint();

        return
        [
            new ZoomEngineEvidenceItem
            {
                Label = "Native core",
                Value = nativeCorePath is null ? "missing" : "found",
                Detail = nativeCorePath ?? "Build native/build-dev/corevideo-native.exe.",
                Status = nativeCorePath is null ? ZoomSdkReadinessStatus.Blocked : ZoomSdkReadinessStatus.Ready
            },
            new ZoomEngineEvidenceItem
            {
                Label = "Zoom engine",
                Value = zoomEnginePath is null ? "missing" : "found",
                Detail = zoomEnginePath ?? "Build native/build-dev/corevideo-zoom-engine.exe.",
                Status = zoomEnginePath is null ? ZoomSdkReadinessStatus.Blocked : ZoomSdkReadinessStatus.Ready
            },
            new ZoomEngineEvidenceItem
            {
                Label = "SDK runtime",
                Value = sdkRuntimePath is null ? "missing" : "found",
                Detail = sdkRuntimePath ?? $"Expected staged runtime at {stagedRuntimePath}.",
                Status = sdkRuntimePath is null ? ZoomSdkReadinessStatus.Blocked : ZoomSdkReadinessStatus.Ready
            },
            new ZoomEngineEvidenceItem
            {
                Label = "SDK status",
                Value = $"{_sdkReadiness.Status.ToString().ToLowerInvariant()} | {_sdkReadiness.SdkVersion}",
                Detail = _sdkReadiness.Summary,
                Status = _sdkReadiness.Status
            },
            new ZoomEngineEvidenceItem
            {
                Label = "Media core",
                Value = _bridge.Running ? "running" : "stopped",
                Detail = _bridge.Running ? _bridge.ProfileSummary : "Start or join Zoom to launch the media core.",
                Status = _bridge.Running ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Warning
            },
            new ZoomEngineEvidenceItem
            {
                Label = "Meeting",
                Value = $"{meetingState} | {participantCount} participant{(participantCount == 1 ? "" : "s")}",
                Detail = snapshot?.ActiveSpeakerId is { Length: > 0 } activeSpeaker
                    ? $"Active speaker: {activeSpeaker}"
                    : "Active speaker not reported yet.",
                Status = IsInMeeting ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Warning
            },
            new ZoomEngineEvidenceItem
            {
                Label = "Frame evidence",
                Value = lastFrameHint.Value,
                Detail = lastFrameHint.Detail,
                Status = lastFrameHint.Status
            },
            new ZoomEngineEvidenceItem
            {
                Label = "Readiness hint",
                Value = readinessHint.Value,
                Detail = readinessHint.Detail,
                Status = readinessHint.Status
            },
            new ZoomEngineEvidenceItem
            {
                Label = "Source adapter",
                Value = source is null ? "unknown" : $"{source.Kind} | {source.Status}",
                Detail = source is null
                    ? "No source snapshot has been published yet."
                    : $"adapter={source.AdapterId}; subscribed={source.SubscribedSourceCount}; live={source.LiveFrameCount}; stale={source.StaleFrameCount}; dropped={source.DroppedFrameCount}",
                Status = source?.Status is "live" or "subscribed" ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Warning
            }
        ];
    }

    private (string Value, string Detail, ZoomSdkReadinessStatus Status) DescribeLastFrameHint()
    {
        if (_firstFrameEvidence.Evidence.Count == 0 && _firstFrameEvidence.Missing.Count == 0)
        {
            return ("no evidence", "No native media-core snapshot or frame event has been received yet.", ZoomSdkReadinessStatus.Warning);
        }

        return (
            _firstFrameEvidence.Ready ? "first-frame ready" : "waiting for first-frame proof",
            $"observed: {string.Join(", ", _firstFrameEvidence.Evidence.DefaultIfEmpty("none"))}; missing: {string.Join(", ", _firstFrameEvidence.Missing.DefaultIfEmpty("none"))}; zoom={_firstFrameEvidence.LiveZoomFrameCount}; program={_firstFrameEvidence.ProgramFrameCount}; audio={_firstFrameEvidence.AudioPacketCount}",
            _firstFrameEvidence.Ready ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Warning);
    }

    private (string Value, string Detail, ZoomSdkReadinessStatus Status) ResolveReadinessHint()
    {
        if (_sdkReadiness.Blockers.FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item)) is { } blocker)
        {
            return ("blocked", blocker, ZoomSdkReadinessStatus.Blocked);
        }

        if (_sdkReadiness.Warnings.FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item)) is { } warning)
        {
            return ("warning", warning, ZoomSdkReadinessStatus.Warning);
        }

        if (ShowZoomOAuthControls && !ZoomOAuthSignedIn)
        {
            return ("sign-in recommended", "Sign in with Zoom before joining external-account meetings.", ZoomSdkReadinessStatus.Warning);
        }

        return ("ready", "Runtime, auth path, and raw-media readiness checks are clear.", ZoomSdkReadinessStatus.Ready);
    }

    private static ZoomMeetingState ParseMeetingState(string? meetingState) =>
        meetingState?.Trim().ToLowerInvariant() switch
        {
            "in_meeting" or "in-meeting" or "joining" => ZoomMeetingState.InMeeting,
            "idle" or "leaving" => ZoomMeetingState.Idle,
            "reconnecting" => ZoomMeetingState.Reconnecting,
            "error" => ZoomMeetingState.Error,
            _ => ZoomMeetingState.Idle
        };

    private static bool IsSuccessfulJoinState(string? meetingState) =>
        ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(meetingState)
            .Equals("in_meeting", StringComparison.Ordinal);

    private static string DescribeJoinException(Exception ex)
    {
        if (!string.IsNullOrWhiteSpace(ex.Message))
        {
            return ex.Message;
        }

        if (ex is System.Runtime.InteropServices.COMException com)
        {
            return $"Media core join failed (HRESULT 0x{com.HResult:X8}).";
        }

        return $"{ex.GetType().Name}: media core join failed.";
    }

    private void RunOnUiThread(Action action)
    {
        if (_dispatcher.HasThreadAccess)
        {
            action();
            return;
        }

        _dispatcher.TryEnqueue(() => action());
    }

    private void SetJoinProgress(ZoomMeetingState state, string status) =>
        RunOnUiThread(() =>
        {
            MeetingState = state;
            JoinStatus = status;
        });

    private void SetJoinFailure(string status) =>
        RunOnUiThread(() =>
        {
            MeetingState = ZoomMeetingState.Error;
            JoinStatus = status;
            NotifyMeetingUi();
        });
}
