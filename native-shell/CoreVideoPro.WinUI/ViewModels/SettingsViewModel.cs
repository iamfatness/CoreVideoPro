using System.Diagnostics;
using System.Collections.ObjectModel;
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
    private readonly IRecentZoomMeetingStore _recentMeetingStore;
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
    private IReadOnlyList<ZoomEngineEvidenceItem> _diagnosticsReadout = [];
    private Func<IReadOnlyList<SupportBundleOutputDestination>>? _outputDestinationsFactory;
    private FirstFrameValidationEvidence _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From();
    private Stopwatch? _firstFrameStopwatch;

    [ObservableProperty]
    private string _supportBundleStatus = string.Empty;

    public ObservableCollection<RecentZoomMeeting> RecentMeetings { get; } = [];

    public SettingsViewModel(
        MediaCoreBridgeService bridge,
        Func<bool> captureRunning,
        ZoomOAuthService? oauth = null,
        Action? drainOAuthCallback = null,
        Action? onMeetingPresenceChanged = null,
        Func<Task>? onBeforeLeaveMeeting = null,
        Action<string>? zoomStatusChanged = null,
        Action? onMeetingJoined = null,
        IRecentZoomMeetingStore? recentMeetingStore = null)
    {
        _bridge = bridge;
        _oauth = oauth;
        _captureRunning = captureRunning;
        _drainOAuthCallback = drainOAuthCallback;
        _onMeetingPresenceChanged = onMeetingPresenceChanged;
        _onBeforeLeaveMeeting = onBeforeLeaveMeeting;
        _zoomStatusChanged = zoomStatusChanged;
        _onMeetingJoined = onMeetingJoined;
        _recentMeetingStore = recentMeetingStore ?? new FileRecentZoomMeetingStore(FileRecentZoomMeetingStore.DefaultStorePath());
        _ = RefreshOAuthStatusAsync();
        _ = LoadRecentMeetingsAsync();
        RefreshSdkReadiness();
        RefreshDiagnosticsReadout();
    }

    /// <summary>
    /// Lets the shell supply redactable output destinations (RTMP/SRT/NDI endpoint
    /// + stream key) for the support bundle. Endpoints/keys are redacted on export.
    /// </summary>
    public void ConfigureOutputDestinations(Func<IReadOnlyList<SupportBundleOutputDestination>>? factory) =>
        _outputDestinationsFactory = factory;

    public IReadOnlyList<ZoomEngineEvidenceItem> DiagnosticsReadout => _diagnosticsReadout;

    public bool ShowSupportBundleStatus => !string.IsNullOrWhiteSpace(SupportBundleStatus);

    partial void OnSupportBundleStatusChanged(string value) =>
        OnPropertyChanged(nameof(ShowSupportBundleStatus));

    public bool ShowZoomOAuthControls => _oauth?.Manifest.BrokerConfigured == true;

    public bool CanSignInWithZoom => ShowZoomOAuthControls && !ZoomOAuthSignedIn;

    public bool CanSignOutZoom => ShowZoomOAuthControls && ZoomOAuthSignedIn;

    public ZoomSdkReadinessReport SdkReadiness => _sdkReadiness;

    public IReadOnlyList<ZoomSdkReadinessCheck> SdkChecks => _sdkReadiness.Checks;

    public IReadOnlyList<string> SdkBlockers =>
        _sdkReadiness.Blockers.Select(FormatActionableBlocker).ToList();

    public IReadOnlyList<ZoomEngineEvidenceItem> ZoomEngineEvidence => _zoomEngineEvidence;

    public bool IsInMeeting => MeetingState == ZoomMeetingState.InMeeting;

    public bool CanEditJoinFields => !IsInMeeting;

    public bool ShowLeaveButton => IsInMeeting;

    public bool ShowJoinButton => !IsInMeeting;

    public bool JoinBlockedBySdk => ZoomSdkReadinessService.ShouldBlockZoomJoin(_captureRunning(), _sdkReadiness);

    public bool CanJoinZoom => ShowJoinButton;

    public bool CanOpenMeetingExternally => CanEditJoinFields && !string.IsNullOrWhiteSpace(JoinMeetingUrl);

    public string JoinActionHint
    {
        get
        {
            if (JoinBlockedBySdk)
            {
                return JoinBlockedReason;
            }

            if (ShowZoomOAuthControls && !ZoomOAuthSignedIn)
            {
                return "Sign in with Zoom before joining this meeting in CoreVideo.";
            }

            return "Join with CoreVideo, or open this meeting in the Zoom app.";
        }
    }

    public bool ShowRecentMeetings => RecentMeetings.Count > 0;

    public string JoinBlockedReason =>
        _sdkReadiness.Status == ZoomSdkReadinessStatus.Blocked
            ? "Zoom is unavailable. Open Health for details."
            : "Zoom needs attention. Open Health for details.";

    public bool ShowLicenseActionStatus => !string.IsNullOrWhiteSpace(LicenseActionStatus);

    public string SdkChipLabel =>
        _sdkReadiness.Status switch
        {
            ZoomSdkReadinessStatus.Ready => IsInMeeting ? "Connected to Zoom" : "Ready to join Zoom",
            ZoomSdkReadinessStatus.Warning => "Zoom needs attention",
            _ => "Zoom is unavailable"
        };

    public string ZoomStatusGuidance =>
        _sdkReadiness.Status switch
        {
            ZoomSdkReadinessStatus.Ready => IsInMeeting
                ? "Meeting controls and participant video are available."
                : "Enter a meeting link or ID above when you are ready to connect.",
            ZoomSdkReadinessStatus.Warning => "Some Zoom features may be unavailable. Open Health for details.",
            _ => "CoreVideo cannot join Zoom yet. Open Health for details."
        };

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
            var participantLabel = LiveParticipantCount == 1 ? "participant" : "participants";
            return MeetingState switch
            {
                ZoomMeetingState.InMeeting => $"Connected · {LiveParticipantCount} {participantLabel}",
                ZoomMeetingState.Joining => "Joining Zoom…",
                ZoomMeetingState.Reconnecting => "Reconnecting to Zoom…",
                ZoomMeetingState.Error => "Connection needs attention",
                _ => "Not connected"
            };
        }
    }

    public bool CanActivateLicense => !string.IsNullOrWhiteSpace(ActivationKey);

    partial void OnMeetingStateChanged(ZoomMeetingState value)
    {
        NotifyMeetingUi();
        _zoomStatusChanged?.Invoke(value == ZoomMeetingState.InMeeting ? "Zoom Live" : "Zoom Offline");
    }

    partial void OnJoinMeetingUrlChanged(string value)
    {
        OnPropertyChanged(nameof(CanOpenMeetingExternally));
        OnPropertyChanged(nameof(JoinActionHint));
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
        OnPropertyChanged(nameof(JoinActionHint));
    }

    private DateTime _lastEvidenceRefreshUtc = DateTime.MinValue;

    public void RefreshZoomEngineEvidence(NativeMediaCoreStateSnapshot? snapshot = null, bool throttle = false)
    {
        // The evidence rows carry live counters (frame/byte counts) that change on
        // every snapshot (~10/s). Rebuilding + re-notifying that often makes a
        // wrapping row flip height each tick, which flickers the page's scrollbar
        // and visibly vibrates the layout. It's diagnostic info — coalesce the
        // snapshot-driven path to ~1/s. Explicit refreshes (user/state actions)
        // still update immediately.
        if (throttle && (DateTime.UtcNow - _lastEvidenceRefreshUtc) < TimeSpan.FromMilliseconds(1000))
        {
            return;
        }

        _lastEvidenceRefreshUtc = DateTime.UtcNow;

        if (snapshot is not null)
        {
            _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From(
                existing: _firstFrameEvidence,
                snapshot: snapshot);
        }

        // May be called from the background frame-dispatch thread; the evidence is
        // data-bound, so raise the change notifications on the UI dispatcher. (No-op
        // marshal when already on the UI thread.)
        RunOnUiThread(() =>
        {
            _zoomEngineEvidence = BuildZoomEngineEvidence(snapshot ?? _bridge.LastSnapshot);
            OnPropertyChanged(nameof(ZoomEngineEvidence));
            RefreshDiagnosticsReadout();
        });
    }

    /// <summary>
    /// Recovery / output / feed / encoder warnings + recent operator actions and
    /// event log surfaced as an actionable readout. Sourced from the latest
    /// media-core snapshot warnings and supervisor health.
    /// </summary>
    public void RefreshDiagnosticsReadout()
    {
        RunOnUiThread(() =>
        {
            _diagnosticsReadout = BuildDiagnosticsReadout(_bridge.LastSnapshot, _bridge.Health);
            OnPropertyChanged(nameof(DiagnosticsReadout));
        });
    }

    [RelayCommand]
    private async Task ExportSupportBundleAsync()
    {
        RefreshDiagnosticsReadout();
        try
        {
            var path = SupportBundleBuilder.DefaultBundlePath();
            var outputs = _outputDestinationsFactory?.Invoke();
            await SupportBundleBuilder.WriteAsync(
                path,
                _bridge.LastSnapshot,
                _bridge.Health,
                new SupportBundleAppInfo(),
                outputs).ConfigureAwait(true);
            SupportBundleStatus = $"Support bundle exported to {path}";
            LaunchLog.Write($"support-bundle: exported to {path}");
        }
        catch (Exception ex)
        {
            SupportBundleStatus = $"Support bundle export failed: {ex.Message}";
            LaunchLog.Write($"support-bundle: export failed {ex.GetType().Name}: {ex.Message}");
        }
    }

    private static IReadOnlyList<ZoomEngineEvidenceItem> BuildDiagnosticsReadout(
        NativeMediaCoreStateSnapshot? snapshot,
        MediaCoreHealth health)
    {
        var items = new List<ZoomEngineEvidenceItem>();

        var recovering = health.Recovering;
        var recoveryStatus = recovering
            ? ZoomSdkReadinessStatus.Warning
            : health.RestartCount > 0
                ? ZoomSdkReadinessStatus.Warning
                : ZoomSdkReadinessStatus.Ready;
        var latestCrash = health.CrashEvents.Count > 0 ? health.CrashEvents[^1] : null;
        items.Add(new ZoomEngineEvidenceItem
        {
            Label = "Recovery",
            Value = recovering
                ? "recovering"
                : health.RestartCount > 0 ? $"recovered ({health.RestartCount} restart{(health.RestartCount == 1 ? "" : "s")})" : "stable",
            Detail = latestCrash is null
                ? "No media-core restarts observed this session."
                : $"Last exit {(latestCrash.ExitCode?.ToString() ?? "unknown")} at {latestCrash.At}; total restarts {health.RestartCount}.",
            Status = recoveryStatus
        });

        if (snapshot is null)
        {
            items.Add(new ZoomEngineEvidenceItem
            {
                Label = "Snapshot",
                Value = "unavailable",
                Detail = "Join a Zoom meeting or start the media core to collect diagnostics.",
                Status = ZoomSdkReadinessStatus.Warning
            });
            return items;
        }

        var outputWarnings = snapshot.OutputSenderSession.Warnings
            .Concat(snapshot.EncoderSession.Warnings)
            .ToList();
        items.Add(new ZoomEngineEvidenceItem
        {
            Label = "Output / encoder",
            Value = outputWarnings.Count == 0
                ? $"{snapshot.OutputSenderSession.Status} | {snapshot.EncoderSession.Status}"
                : $"{outputWarnings.Count} warning{(outputWarnings.Count == 1 ? "" : "s")}",
            Detail = outputWarnings.Count == 0
                ? $"senders active={snapshot.OutputSenderSession.ActiveSenderCount}; encoder targets={snapshot.EncoderSession.Targets.Count}"
                : string.Join(" · ", outputWarnings),
            Status = outputWarnings.Count == 0 ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Warning
        });

        var feedWarnings = snapshot.SourceSnapshot.Warnings.ToList();
        items.Add(new ZoomEngineEvidenceItem
        {
            Label = "Feeds",
            Value = feedWarnings.Count == 0
                ? $"{snapshot.SourceSnapshot.Status}"
                : $"{feedWarnings.Count} warning{(feedWarnings.Count == 1 ? "" : "s")}",
            Detail = feedWarnings.Count == 0
                ? $"subscribed={snapshot.SourceSnapshot.SubscribedSourceCount}; live={snapshot.SourceSnapshot.LiveFrameCount}; stale={snapshot.SourceSnapshot.StaleFrameCount}; dropped={snapshot.SourceSnapshot.DroppedFrameCount}"
                : string.Join(" · ", feedWarnings),
            Status = feedWarnings.Count == 0 ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Warning
        });

        if (snapshot.Warnings.Count > 0)
        {
            items.Add(new ZoomEngineEvidenceItem
            {
                Label = "Media-core warnings",
                Value = $"{snapshot.Warnings.Count}",
                Detail = string.Join(" · ", snapshot.Warnings),
                Status = ZoomSdkReadinessStatus.Warning
            });
        }

        foreach (var action in snapshot.OperatorActions.Take(5))
        {
            items.Add(new ZoomEngineEvidenceItem
            {
                Label = $"Action · {action.Area}",
                Value = action.Title,
                Detail = action.Detail,
                Status = action.Severity == "critical"
                    ? ZoomSdkReadinessStatus.Blocked
                    : action.Severity == "warning"
                        ? ZoomSdkReadinessStatus.Warning
                        : ZoomSdkReadinessStatus.Ready
            });
        }

        foreach (var entry in snapshot.EventLog.TakeLast(5).Reverse())
        {
            items.Add(new ZoomEngineEvidenceItem
            {
                Label = $"Event · {entry.Area}",
                Value = entry.Title,
                Detail = entry.Detail,
                Status = entry.Severity == "critical"
                    ? ZoomSdkReadinessStatus.Blocked
                    : entry.Severity == "warning"
                        ? ZoomSdkReadinessStatus.Warning
                        : ZoomSdkReadinessStatus.Ready
            });
        }

        return items;
    }

    public void ObserveZoomVideoFrame(ZoomVideoFrame frame)
    {
        EnsureFirstFrameStopwatch();
        _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From(
            existing: _firstFrameEvidence,
            zoomVideoFrame: frame,
            zoomFrameObservedElapsedMs: _firstFrameStopwatch?.Elapsed.TotalMilliseconds);
        // Called on the background frame-dispatch thread (~60fps). Throttle so the
        // bound evidence rebuild runs ~1/s, and it marshals to the UI thread.
        RefreshZoomEngineEvidence(_bridge.LastSnapshot, throttle: true);
    }

    public void ObserveProgramPreviewFrame(ProgramFramePreview preview)
    {
        _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From(
            existing: _firstFrameEvidence,
            programPreviewFrame: preview);
        RefreshZoomEngineEvidence(_bridge.LastSnapshot, throttle: true);
    }

    [RelayCommand]
    private async Task JoinZoomAsync()
    {
        var joinDetails = ZoomMeetingUrlParser.Parse(JoinMeetingUrl);
        if (!joinDetails.CanJoin)
        {
            SetJoinFailure(joinDetails.ValidationError ?? "Enter a valid Zoom meeting URL or meeting ID before joining.");
            LaunchLog.Write($"zoom-join: blocked ({JoinStatus})");
            return;
        }

        if (JoinBlockedBySdk)
        {
            SdkDiagnosticsExpanded = true;
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
        LaunchLog.Write($"zoom-join: starting meetingNumber={joinDetails.MeetingNumber ?? "unknown"}");

        try
        {
            _drainOAuthCallback?.Invoke();
            await RefreshOAuthStatusAsync().ConfigureAwait(false);

            if (!_bridge.Running)
            {
                SetJoinProgress(ZoomMeetingState.Joining, "Preparing Zoom…");
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
                        const string missingJwt = "Zoom sign-in needs to be refreshed. Sign out and sign in again.";
                        LaunchLog.Write($"zoom-join: {missingJwt}");
                        SetJoinFailure(missingJwt);
                        return;
                    }
                }
                catch (Exception authEx)
                {
                    LaunchLog.Write($"zoom-join: credential error {authEx.Message}");
                    SetJoinFailure("Zoom sign-in could not be completed. Sign out, sign in, and try again.");
                    return;
                }
            }
            else
            {
                LaunchLog.Write("zoom-join: credentials oauth=unavailable (public app key only)");
            }

            SetJoinProgress(ZoomMeetingState.Joining, "Joining meeting… (up to 60s)");
            var snapshot = await _bridge.JoinZoomAsync(
                joinDetails.MeetingUrl,
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
                SetJoinFailure("Could not join Zoom. Open Health for details.");
                return;
            }

            RunOnUiThread(() =>
            {
                BeginFirstFrameTiming();
                ApplyCaptureSnapshot(snapshot);
                JoinStatus = "Connected to Zoom.";
                LaunchLog.Write($"zoom-join: {JoinStatus}");
                _zoomStatusChanged?.Invoke("Zoom Live");
                _onMeetingJoined?.Invoke();
            });
            await RememberRecentMeetingAsync(JoinMeetingUrl, DisplayName, IsWebinar).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            var message = DescribeJoinException(ex);
            LaunchLog.Write($"zoom-join: failed {ex.GetType().Name}: {message}");
            SetJoinFailure(message);
        }
    }

    [RelayCommand]
    private async Task OpenMeetingExternallyAsync()
    {
        var joinDetails = ZoomMeetingUrlParser.Parse(JoinMeetingUrl);
        if (!joinDetails.CanJoin)
        {
            JoinStatus = joinDetails.ValidationError ?? "Enter a valid Zoom meeting URL or meeting ID before opening Zoom.";
            return;
        }

        try
        {
            var zoomAppUri = joinDetails.ZoomAppUri;
            if (!string.IsNullOrWhiteSpace(zoomAppUri))
            {
                LaunchLog.Write($"zoom-open-external: opening zoom app meetingNumber={joinDetails.MeetingNumber}");
                await ExternalUriLauncher.OpenAsync(zoomAppUri).ConfigureAwait(true);
                JoinStatus = "Opened meeting with the Zoom app.";
                return;
            }

            LaunchLog.Write("zoom-open-external: opening meeting URL fallback");
            await ExternalUriLauncher.OpenAsync(joinDetails.MeetingUrl).ConfigureAwait(true);
            JoinStatus = "Opened meeting URL with Windows. Zoom may launch through the browser handoff.";
        }
        catch (Exception ex)
        {
            JoinStatus = "Could not open this meeting in Zoom.";
            LaunchLog.Write($"zoom-open-external: failed {ex.GetType().Name}: {ex.Message}");
        }
    }

    [RelayCommand]
    private void UseRecentMeeting(RecentZoomMeeting? meeting)
    {
        if (meeting is null || IsInMeeting)
        {
            return;
        }

        JoinMeetingUrl = meeting.MeetingNumber;
        DisplayName = meeting.DisplayName;
        IsWebinar = meeting.Webinar;
        JoinStatus = $"Ready to rejoin {meeting.MeetingNumber}";
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
                JoinStatus = "Disconnected from Zoom.";
                _bridge.Stop();
            }
            else
            {
                MeetingState = ZoomMeetingState.Idle;
                LiveParticipantCount = 0;
                JoinStatus = "Left Zoom meeting.";
            }

            ResetFirstFrameEvidence();
            RefreshZoomEngineEvidence(_bridge.LastSnapshot);
            NotifyMeetingUi();
        }
        catch (Exception ex)
        {
            MeetingState = ZoomMeetingState.Error;
            LaunchLog.Write($"zoom-leave: failed {ex.GetType().Name}: {ex.Message}");
            JoinStatus = "Could not leave the Zoom meeting cleanly. Open Health for details.";
            NotifyMeetingUi();
        }
    }

    public void ApplyCaptureSnapshot(RawCaptureSnapshot snapshot)
    {
        MeetingState = ParseMeetingState(snapshot.MeetingState);
        LiveParticipantCount = snapshot.Participants.Count;
        RefreshZoomEngineEvidence(_bridge.LastSnapshot, throttle: true);
        NotifyMeetingUi();
    }

    private async Task LoadRecentMeetingsAsync()
    {
        var meetings = await _recentMeetingStore.LoadAsync().ConfigureAwait(false);
        RunOnUiThread(() => ReplaceRecentMeetings(meetings));
    }

    private async Task RememberRecentMeetingAsync(string meetingUrl, string displayName, bool webinar)
    {
        var meetings = await _recentMeetingStore.RememberAsync(
            meetingUrl,
            string.IsNullOrWhiteSpace(displayName) ? "CoreVideo Producer" : displayName.Trim(),
            webinar).ConfigureAwait(false);
        RunOnUiThread(() => ReplaceRecentMeetings(meetings));
    }

    private void ReplaceRecentMeetings(IEnumerable<RecentZoomMeeting> meetings)
    {
        RecentMeetings.Clear();
        foreach (var meeting in meetings)
        {
            RecentMeetings.Add(meeting);
        }

        OnPropertyChanged(nameof(RecentMeetings));
        OnPropertyChanged(nameof(ShowRecentMeetings));
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

        RefreshZoomEngineEvidence(_bridge.LastSnapshot, throttle: true);
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
            OauthStatusMessage = "Zoom sign-in could not be completed. Try again or open Health for details.";
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
        OnPropertyChanged(nameof(CanJoinZoom));
        OnPropertyChanged(nameof(CanOpenMeetingExternally));
        OnPropertyChanged(nameof(JoinActionHint));
        OnPropertyChanged(nameof(MeetingStatusLine));
        OnPropertyChanged(nameof(SdkChipLabel));
        OnPropertyChanged(nameof(ZoomStatusGuidance));
        _onMeetingPresenceChanged?.Invoke();
    }

    private void NotifySdkUi()
    {
        OnPropertyChanged(nameof(SdkReadiness));
        OnPropertyChanged(nameof(SdkChecks));
        OnPropertyChanged(nameof(SdkBlockers));
        OnPropertyChanged(nameof(SdkChipLabel));
        OnPropertyChanged(nameof(ZoomStatusGuidance));
        OnPropertyChanged(nameof(SdkIsReady));
        OnPropertyChanged(nameof(SdkIsWarning));
        OnPropertyChanged(nameof(SdkIsBlocked));
        OnPropertyChanged(nameof(ShowSdkChecklist));
        OnPropertyChanged(nameof(ShowSdkDetailsButton));
        OnPropertyChanged(nameof(SdkDetailsButtonLabel));
        OnPropertyChanged(nameof(ZoomEngineEvidence));
        OnPropertyChanged(nameof(JoinBlockedBySdk));
        OnPropertyChanged(nameof(CanJoinZoom));
        OnPropertyChanged(nameof(JoinBlockedReason));
        OnPropertyChanged(nameof(JoinActionHint));
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
            return (
                "no evidence",
                IsInMeeting
                    ? "Joined meeting but no zoom-video-frame event or media-core snapshot has arrived yet. Confirm raw video subscriptions are active and wait a few seconds."
                    : "No native media-core snapshot or frame event has been received yet. Join a Zoom meeting to collect first-frame proof.",
                ZoomSdkReadinessStatus.Warning);
        }

        var timing = _firstFrameEvidence.FirstZoomFrameElapsedMs is >= 0
            ? $"first-frame={_firstFrameEvidence.FirstZoomFrameElapsedMs:0}ms"
            : "first-frame=not-timed";
        var dimensions = _firstFrameEvidence.LastZoomFrameWidth is > 0 && _firstFrameEvidence.LastZoomFrameHeight is > 0
            ? $"{_firstFrameEvidence.LastZoomFrameWidth}x{_firstFrameEvidence.LastZoomFrameHeight}"
            : "unknown-size";
        var participant = string.IsNullOrWhiteSpace(_firstFrameEvidence.LastZoomParticipantId)
            ? "unknown-participant"
            : $"participant={_firstFrameEvidence.LastZoomParticipantId}";

        return (
            _firstFrameEvidence.Ready ? "first-frame ready" : "waiting for first-frame proof",
            $"{timing}; {participant}; frame={dimensions}; observed: {string.Join(", ", _firstFrameEvidence.Evidence.DefaultIfEmpty("none"))}; missing: {string.Join(", ", _firstFrameEvidence.Missing.DefaultIfEmpty("none"))}; zoom={_firstFrameEvidence.LiveZoomFrameCount}; program={_firstFrameEvidence.ProgramFrameCount}; audio={_firstFrameEvidence.AudioPacketCount}",
            _firstFrameEvidence.Ready ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Warning);
    }

    private (string Value, string Detail, ZoomSdkReadinessStatus Status) ResolveReadinessHint()
    {
        if (_sdkReadiness.Blockers.FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item)) is { } blocker)
        {
            return ("blocked", FormatActionableBlocker(blocker), ZoomSdkReadinessStatus.Blocked);
        }

        if (_sdkReadiness.Warnings.FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item)) is { } warning)
        {
            return ("warning", $"{warning} {DescribeWarningRemediation(warning)}".Trim(), ZoomSdkReadinessStatus.Warning);
        }

        if (ShowZoomOAuthControls && !ZoomOAuthSignedIn)
        {
            return (
                "sign-in recommended",
                "Sign in with Zoom before joining external-account meetings. Use the Sign in with Zoom button above.",
                ZoomSdkReadinessStatus.Warning);
        }

        if (IsInMeeting && !_firstFrameEvidence.ZoomVideoFrameObserved)
        {
            return (
                "awaiting zoom frame",
                "Meeting is live but no zoom-video-frame proof has arrived yet. Keep the meeting open and confirm participant video is on.",
                ZoomSdkReadinessStatus.Warning);
        }

        return ("ready", "Runtime, auth path, and raw-media readiness checks are clear.", ZoomSdkReadinessStatus.Ready);
    }

    private void EnsureFirstFrameStopwatch()
    {
        _firstFrameStopwatch ??= Stopwatch.StartNew();
    }

    private void BeginFirstFrameTiming()
    {
        _firstFrameStopwatch = Stopwatch.StartNew();
        _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From();
    }

    private void ResetFirstFrameEvidence()
    {
        _firstFrameStopwatch = null;
        _firstFrameEvidence = FirstFrameValidationEvidenceBuilder.From();
    }

    private static string FormatActionableBlocker(string blocker)
    {
        if (string.IsNullOrWhiteSpace(blocker))
        {
            return blocker;
        }

        var remediation = DescribeBlockerRemediation(blocker);
        return string.IsNullOrWhiteSpace(remediation) ? blocker : $"{blocker} Next step: {remediation}";
    }

    private static string DescribeBlockerRemediation(string blocker)
    {
        var normalized = blocker.ToLowerInvariant();
        if (normalized.Contains("native core executable is missing", StringComparison.Ordinal) ||
            normalized.Contains("build with .\\scripts\\build-native-dev.ps1", StringComparison.Ordinal))
        {
            return "Run .\\scripts\\build-native-dev.ps1 from the repo root.";
        }

        if (normalized.Contains("zoom meeting sdk runtime is missing", StringComparison.Ordinal) ||
            normalized.Contains("stage-zoom-sdk", StringComparison.Ordinal))
        {
            return "Run .\\scripts\\stage-zoom-sdk.ps1, then rebuild native with .\\scripts\\build-native-dev.ps1.";
        }

        if (normalized.Contains("meeting sdk app key is missing", StringComparison.Ordinal))
        {
            return "Embed src/config/zoomMeetingSdk.json or set COREVIDEO_ZOOM_PUBLIC_APP_KEY before joining.";
        }

        if (normalized.Contains("oauth pkce broker is not configured", StringComparison.Ordinal))
        {
            return "Embed src/config/zoomOAuth.json or configure the OAuth broker environment variables.";
        }

        if (normalized.Contains("raw participant video callbacks are disabled", StringComparison.Ordinal))
        {
            return "Re-stage the Zoom SDK package and confirm raw video headers are present.";
        }

        if (normalized.Contains("raw audio callbacks are disabled", StringComparison.Ordinal))
        {
            return "Re-stage the Zoom SDK package and confirm raw audio headers are present.";
        }

        if (normalized.Contains("raw screen-share callbacks are disabled", StringComparison.Ordinal))
        {
            return "Re-stage the Zoom SDK package and confirm raw screen-share headers are present.";
        }

        return string.Empty;
    }

    private static string DescribeWarningRemediation(string warning)
    {
        var normalized = warning.ToLowerInvariant();
        if (normalized.Contains("sign in with zoom", StringComparison.Ordinal))
        {
            return "Use Sign in with Zoom in Settings before joining external-account meetings.";
        }

        if (normalized.Contains("staged target", StringComparison.Ordinal))
        {
            return "Run .\\scripts\\stage-zoom-sdk.ps1 to normalize packaging.";
        }

        return string.Empty;
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
        if (ex is TimeoutException or TaskCanceledException)
        {
            return "Zoom took too long to respond. Check the connection and try again.";
        }

        return "Could not join Zoom. Open Health for details.";
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
