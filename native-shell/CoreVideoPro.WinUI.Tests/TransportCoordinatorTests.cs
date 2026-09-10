using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Characterization tests for <see cref="TransportCoordinator"/> — the record / stream / take /
/// engine orchestration extracted from the StudioViewModel god object (PR2 strangler). These are
/// the FIRST-EVER coverage of transport orchestration: they exist because the coordinator is
/// constructible in isolation (fake <see cref="IMediaCoreBridge"/> + fake <see cref="ITransportHost"/>
/// + inline <see cref="ITransportDispatcher"/>), which StudioViewModel itself is not. They lock the
/// golden-path behavior — arm, in-flight guard, #286-class rollback, spine-callback install, Take
/// promotion — BEFORE anyone refactors it further.
/// </summary>
public sealed class TransportCoordinatorTests
{
    private static (TransportCoordinator Coordinator, FakeMediaCoreBridge Bridge, FakeTransportHost Host) Build(
        int recordingSyncRetryAttempts = 120,
        int recordingSyncRetryDelayMs = 250)
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeTransportHost();
        var coordinator = new TransportCoordinator(
            bridge,
            host,
            host,
            recordingSyncRetryAttempts,
            recordingSyncRetryDelayMs);
        return (coordinator, bridge, host);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ExplicitStop_RetriesFailedStopWithoutRearming(bool recording)
    {
        var (coordinator, _, host) = Build();
        host.Recording = recording;
        host.Streaming = !recording;
        host.SyncThrows = new InvalidOperationException("stop did not reach core");
        Func<bool, Task> set = recording ? coordinator.SetRecordingAsync : coordinator.SetStreamingAsync;
        await set(false);
        Assert.False(recording ? host.Recording : host.Streaming);
        Assert.Equal(1, host.SyncCallCount);

        host.SyncThrows = null;
        host.HoldSync = true;
        var retry = StudioControlSurface.RunOutputSet(false, true, false, _ => true, set, "output");
        Assert.Equal(2, host.SyncCallCount);
        Assert.False(recording ? host.Recording : host.Streaming);
        host.ReleaseSync();
        await retry;
        Assert.False(recording ? host.Recording : host.Streaming);
        Assert.Contains(recording ? "Recording stop requested" : "Streaming stopped", host.OutputStatus);
    }

    [Theory]
    [InlineData(false, false, false, 0)]
    [InlineData(true, false, true, 0)]
    [InlineData(false, true, false, 1)]
    [InlineData(true, true, false, 1)]
    [InlineData(false, false, true, 1)]
    public async Task ExplicitOutputSet_PreservesIdempotencyAndPassesTarget(bool requested, bool live, bool target, int expectedCalls)
    {
        var calls = new List<bool>();
        await StudioControlSurface.RunOutputSet(requested, live, target, _ => true,
            value => { calls.Add(value); return Task.CompletedTask; }, "output");
        Assert.Equal(expectedCalls, calls.Count);
        Assert.All(calls, value => Assert.Equal(target, value));
    }

    [Fact]
    public async Task ExplicitOutputSet_DoesNotRunWhileUnavailable()
    {
        var called = false;
        await StudioControlSurface.RunOutputSet(false, true, false, _ => false,
            _ => { called = true; return Task.CompletedTask; }, "output");
        Assert.False(called);
    }

    // ---------------------------------------------------------------- Recording

    [Fact]
    public async Task ToggleRecording_ArmsViaBridgeSync_AndSetsInFlightDuringTheToggle()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        host.HoldSync = true; // keep the arming sync pending so the toggle stays in-flight

        var toggle = coordinator.ToggleRecordingAsync();

        // Mid-flight: recording armed (host.Recording flipped true) and the guard is set.
        Assert.True(host.Recording);
        Assert.True(coordinator.RecordingToggleInFlight);
        Assert.Equal(1, host.SyncCallCount);

        host.ReleaseSync();
        await toggle;

        Assert.True(host.Recording);
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.Contains("Recording start requested.", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_DoubleToggle_IsGuardedByTheInFlightFlag()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        host.HoldSync = true;

        var first = coordinator.ToggleRecordingAsync();       // arms, then blocks on the sync gate
        var second = coordinator.ToggleRecordingAsync();       // must early-return on the in-flight guard
        await second;                                          // second returns immediately

        Assert.Equal(1, host.SyncCallCount);                   // only the first toggle reached the sync

        host.ReleaseSync();
        await first;
        Assert.False(coordinator.RecordingToggleInFlight);
    }

    [Fact]
    public async Task ToggleRecording_RollsBackAndClearsInFlight_WhenSyncThrows()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        host.SyncThrows = new InvalidOperationException("media core rejected the recording request");

        await coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);                          // rolled back to the previous state
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.StartsWith("Recording start failed:", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_RollsBack_OnHealthFailureSnapshot_286Class()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        // Sync SUCCEEDS but the snapshot reports a failed recording output (the #286 shape:
        // a video-only / broken recording must never look healthy).
        host.SyncResult = new NativeMediaCoreStateSnapshot
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "recording",
                    Status = "failed",
                    Message = "recorder could not open the target file"
                }
            ]
        };

        await coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);                          // health-proof rollback
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.StartsWith("Recording start failed:", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_StopWaitsForBusyStartAndKeepsCommandGuarded()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 6, recordingSyncRetryDelayMs: 1);
        host.Recording = true;
        host.SyncFailuresRemaining = 3;

        var toggle = coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);                           // stop intent is sticky immediately
        Assert.True(coordinator.RecordingToggleInFlight);       // second click cannot re-arm

        await toggle;

        Assert.False(host.Recording);
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.Equal(4, host.SyncCallCount);
        Assert.Equal("Recording stop requested — finalizing.", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_StopRetryExhaustionNeverRearmsRecording()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 2, recordingSyncRetryDelayMs: 1);
        host.Recording = true;
        host.SyncFailuresRemaining = int.MaxValue;

        await coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.Equal(3, host.SyncCallCount);                    // initial attempt plus two retries
        Assert.Contains("stop remains armed", host.OutputStatus, StringComparison.OrdinalIgnoreCase);
    }

    // ---------------------------------------------------------------- Streaming

    [Fact]
    public async Task ToggleStreaming_FailedStopKeepsDesiredStateDisarmed()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = true;
        host.SyncThrows = new InvalidOperationException("connection lost during stop");

        await coordinator.ToggleStreamingAsync();

        Assert.False(host.Streaming);
        Assert.StartsWith("Streaming stop failed:", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleStreaming_ArmsAndProvesStart_WhenSenderGoesLive()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamDestinations = ["rtmp://live.example/app/key"];
        host.SyncResult = new NativeMediaCoreStateSnapshot
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "live",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "s1",
                        Destination = "rtmp://live.example/app/key",
                        Status = "live"
                    }
                ]
            }
        };

        await coordinator.ToggleStreamingAsync();

        Assert.True(host.Streaming);
        Assert.False(coordinator.StreamToggleInFlight);
        Assert.Contains("Streaming start requested.", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleStreaming_BlockedBeforeArming_WhenDestinationsInvalid()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamValidationError = "Select at least one stream destination before streaming.";

        await coordinator.ToggleStreamingAsync();

        Assert.False(host.Streaming);                          // never armed
        Assert.Equal(0, host.SyncCallCount);                   // no core round-trip
        Assert.StartsWith("Streaming start failed:", host.OutputStatus);
        Assert.False(coordinator.StreamToggleInFlight);
    }

    [Fact]
    public async Task ToggleStreaming_DoubleToggle_IsGuardedByTheInFlightFlag()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamDestinations = ["rtmp://live.example/app/key"];
        host.HoldSync = true;

        var first = coordinator.ToggleStreamingAsync();
        var second = coordinator.ToggleStreamingAsync();
        await second;

        Assert.Equal(1, host.SyncCallCount);

        host.ReleaseSync();
        await first;
        Assert.False(coordinator.StreamToggleInFlight);
    }

    [Fact]
    public async Task ToggleStreaming_RollsBackAndClearsInFlight_WhenSyncThrows()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamDestinations = ["rtmp://live.example/app/key"];
        host.SyncThrows = new InvalidOperationException("rtmp output sender failed: connection refused");

        await coordinator.ToggleStreamingAsync();

        Assert.False(host.Streaming);                          // rolled back
        Assert.False(coordinator.StreamToggleInFlight);
        Assert.StartsWith("Streaming start failed:", host.OutputStatus);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("deleted-scene")]
    public async Task Take_InvalidPreviewCannotPoisonProgram(string? preview)
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = preview!;
        await coordinator.TakeAsync();
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal(0, host.SyncCallCount);
        Assert.Equal(0, host.PromoteCallCount);
        Assert.Contains("unavailable", host.CommandStatus);
    }

    [Fact]
    public async Task Take_BusySyncRetriesSameProgramWithoutSwappingAgain()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 3, recordingSyncRetryDelayMs: 1);
        host.PreviewSceneId = "interview";
        host.SyncFailuresRemaining = 2;
        await coordinator.TakeAsync();
        Assert.Equal(new[] { "interview", "interview", "interview" }, host.SyncedProgramIds);
        Assert.Equal("intro", host.PreviewSceneId);
        Assert.Equal(1, host.GoLiveRecords);
        Assert.Equal(0, host.RollbackCount);
        Assert.Equal("Program updated", host.OutputStatus);
    }

    [Fact]
    public async Task Take_DoubleInvocationWhileAwaitingSyncDoesNotSwapBack()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        host.HoldSync = true;
        var first = coordinator.TakeAsync();
        await coordinator.TakeAsync();
        Assert.Equal("interview", host.ActiveSceneId);
        Assert.Equal(1, host.SyncCallCount);
        host.ReleaseSync();
        await first;
    }

    [Fact]
    public async Task Take_FailureRestoresPriorProgramAndRetainsPreviewForRetry()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        host.SyncThrows = new InvalidOperationException("native rejected scene");
        await coordinator.TakeAsync();
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal(1, host.RollbackCount);
        Assert.Contains("previous local Program restored", host.CommandStatus);
        host.SyncThrows = null;
        await coordinator.TakeAsync();
        Assert.Equal("interview", host.ActiveSceneId);
    }

    [Fact]
    public async Task Take_ExhaustedBackpressureRestoresProgramInsteadOfClaimingSuccess()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 2, recordingSyncRetryDelayMs: 1);
        host.PreviewSceneId = "interview";
        host.SyncFailuresRemaining = 10;
        await coordinator.TakeAsync();
        Assert.Equal(2, host.SyncCallCount);
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal(1, host.RollbackCount);
        Assert.NotEqual("Program updated", host.OutputStatus);
    }

    [Fact]
    public async Task TakeApi_DisabledControlFailsWithoutInvokingTake()
    {
        var invoked = false;
        var result = await StudioControlSurface.RunTake(false, () =>
        {
            invoked = true;
            return Task.FromResult(TakeResult.Success);
        });
        Assert.False(result.Ok);
        Assert.False(invoked);
        Assert.Contains("unavailable", result.Error);
    }

    [Fact]
    public async Task TakeApi_ReportsEachInvocationOutcomeInsteadOfPriorSuccess()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        var success = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.True(success.Ok);
        host.SyncThrows = new InvalidOperationException("core rejected next scene");
        var failure = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(failure.Ok);
        Assert.Contains("core rejected next scene", failure.Error);
        Assert.Contains("not confirmed", failure.Error);
    }

    [Fact]
    public async Task TakeApi_ConcurrentInvocationCannotBorrowFirstCompletion()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        host.HoldSync = true;
        var first = StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        var duplicate = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(duplicate.Ok);
        Assert.Contains("already in progress", duplicate.Error);
        Assert.False(first.IsCompleted);
        host.ReleaseSync();
        Assert.True((await first).Ok);
        Assert.Equal(1, host.SyncCallCount);
    }

    [Fact]
    public async Task TakeApi_OfflineLocalSelectionIsNotReportedAsOnAirSuccess()
    {
        var (coordinator, bridge, host) = Build();
        bridge.Running = false;
        host.PreviewSceneId = "interview";
        var result = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(result.Ok);
        Assert.Contains("offline", result.Error);
        Assert.Equal(0, host.SyncCallCount);
    }

    [Fact]
    public async Task TakeApi_ExhaustedBusySyncFails()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 1);
        host.PreviewSceneId = "interview";
        host.SyncFailuresRemaining = 1;
        var result = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(result.Ok);
        Assert.Contains("busy", result.Error);
        Assert.Equal("intro", host.ActiveSceneId);
    }

    [Fact]
    public async Task TakeApi_UnexpectedCommandExceptionReturnsFailure()
    {
        var result = await StudioControlSurface.RunTake(true, () => throw new InvalidOperationException("route preparation failed"));
        Assert.False(result.Ok);
        Assert.Contains("route preparation failed", result.Error);
    }

    // ---------------------------------------------------------------- Take

    [Fact]
    public async Task Take_PromotesPreviewToProgram_AndSwapsProgramBackToPreview()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.HasPendingCue = false;

        await coordinator.TakeAsync();

        Assert.Equal("interview", host.ActiveSceneId);          // preview promoted to program
        Assert.Equal("intro", host.PreviewSceneId);             // old program swapped back to preview
        Assert.Equal(0, host.PromoteCallCount);                 // no media went live -> nothing promoted
        Assert.Equal(1, host.GoLiveRecords);
        Assert.Equal("Program updated", host.OutputStatus);
        Assert.Equal(1, host.SyncCallCount);
    }

    [Fact]
    public async Task Take_RecordsGoLiveAgainstTheProgramRoutesFromBeforeTheSwap()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        var introRoutes = new List<SourceRoute>
        {
            new() { Id = "route-clip", Mode = SourceRouteMode.Fixed, ParticipantId = ShowInputRosterService.ToMediaSourceId("clip") }
        };
        host.ProgramRoutesByScene["intro"] = introRoutes;
        host.ProgramRoutesByScene["interview"] = [];

        await coordinator.TakeAsync();

        Assert.Equal("interview", host.ActiveSceneId);
        Assert.Equal(1, host.GoLiveRecords);
        Assert.Same(introRoutes, host.LastPreviousProgramRoutes);
    }

    [Fact]
    public async Task Take_AClipThatStaysOnProgramIsNotPromoted()
    {
        // The operator paused clip X on Program, then Takes to a scene that also carries X.
        // X did not go live, so the Take must not un-pause it (spec section 2: go-live is the
        // only event a source reacts to).
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [ClipRoute("clip")];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("clip")];

        await coordinator.TakeAsync();

        Assert.Equal("interview", host.ActiveSceneId);
        Assert.Empty(host.LastWentLive!);
        Assert.Equal(0, host.PromoteCallCount);
    }

    [Fact]
    public async Task Take_PromotesOnlyTheClipThatWentLive()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [ClipRoute("bed")];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("bed"), ClipRoute("sting")];

        await coordinator.TakeAsync();

        Assert.Equal(1, host.PromoteCallCount);
        Assert.Equal(new[] { "sting" }, host.LastPromoted);
    }

    [Fact]
    public async Task Take_CommitsPendingDraft_WhenPreviewAndProgramShareTheScene()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "intro";                          // same scene on PGM + PVW
        host.HasPendingCue = true;                              // but a media cue is drafted

        await coordinator.TakeAsync();

        Assert.Equal(1, host.CopyPreviewRoutesCallCount);       // draft committed in place
        Assert.Equal("intro", host.ActiveSceneId);              // no scene swap
        Assert.Equal("intro", host.PreviewSceneId);
        Assert.Equal(0, host.PromoteCallCount);                 // the draft carried no new media
    }

    [Fact]
    public async Task Take_CommittingADraftThatCuesAClipPromotesThatClip()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "intro";
        host.HasPendingCue = true;
        host.ProgramRoutesByScene["intro"] = [];
        host.DraftRoutesByScene["intro"] = [ClipRoute("sting")];

        await coordinator.TakeAsync();

        Assert.Equal(1, host.CopyPreviewRoutesCallCount);
        Assert.Equal(1, host.PromoteCallCount);
        Assert.Equal(new[] { "sting" }, host.LastPromoted);
    }

    [Fact]
    public async Task Take_RefreshesTheMediaBinWhenAClipLeavesProgramWithNothingGoingLive()
    {
        // A clip that LEFT Program on this Take must stop showing "playing" in the bin, but it
        // never appears in wentLive (only entries are promoted) -- so this refresh cannot be
        // gated on PromoteCallCount alone; it fires because the Program media SET changed.
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [ClipRoute("clip")];
        host.ProgramRoutesByScene["interview"] = [];               // clip leaves Program on this Take

        await coordinator.TakeAsync();

        Assert.Equal(0, host.PromoteCallCount);
        Assert.Equal(1, host.RefreshMediaBinPlaybackIndicatorsCallCount);
        Assert.Same(host.ProgramRoutesByScene["intro"], host.LastRefreshPreviousProgramRoutes);
    }

    [Fact]
    public async Task Take_DoesNotDoubleRefreshWhenPromoteAlreadyRebuiltTheBin()
    {
        // Promote's own rebuild (ApplyMediaSelection over every asset) already gives every bin
        // row its current on-air state, so a Take that also promotes something must NOT pay for
        // a second bin rebuild.
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [ClipRoute("bed")];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("bed"), ClipRoute("sting")];

        await coordinator.TakeAsync();

        Assert.Equal(1, host.PromoteCallCount);
        Assert.Equal(0, host.RefreshMediaBinPlaybackIndicatorsCallCount);
    }

    [Fact]
    public async Task Take_DoesNotRefreshTheMediaBinWhenTheProgramMediaSetIsUnchanged()
    {
        // An automated Magic Scene Take between two scenes that share the exact same Program
        // media (or carry none at all) must not rebuild MediaBinGroups every cut.
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [ClipRoute("clip")];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("clip")];   // same clip, still on Program

        await coordinator.TakeAsync();

        Assert.Equal(0, host.PromoteCallCount);
        Assert.Equal(0, host.RefreshMediaBinPlaybackIndicatorsCallCount);
    }

    private static SourceRoute ClipRoute(string assetId) =>
        new() { Id = $"route-{assetId}", Mode = SourceRouteMode.Fixed, ParticipantId = ShowInputRosterService.ToMediaSourceId(assetId) };

    // ---------------------------------------------------------------- Take rollback: media selection (T1.3, #430)

    [Theory]
    [InlineData(false)]   // the sync threw (timeout, parse failure, core rejected it)
    [InlineData(true)]    // backpressure retries ran out
    public async Task Take_RollbackRestoresTheSelectionAClipThatWentLiveTookOver(bool exhaustBackpressure)
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 2, recordingSyncRetryDelayMs: 1);
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("y")];
        var prior = FakeTransportHost.Clip("bed", playing: false, "BED is ready to cue");
        host.Selection = prior;
        if (exhaustBackpressure) host.SyncFailuresRemaining = 10;
        else host.SyncThrows = new InvalidOperationException("core answered with an unparseable snapshot");

        await coordinator.TakeAsync();

        Assert.Equal(1, host.PromoteCallCount);                  // the Take really did promote Y
        Assert.Equal(1, host.RollbackCount);
        Assert.Equal(prior, host.Selection);                     // ...and the rollback undid it
        Assert.Equal(1, host.RestoreSelectionCallCount);         // one restore = one bin rebuild
        Assert.Equal("Audition", host.ToggleLabel);
    }

    [Fact]
    public async Task Take_RollbackKeepsAPausedProgramClipPausedWithAnHonestToggle()
    {
        // X is paused on Program and stays on Program in the Take's scene; Y goes live and the
        // Take moves the selection to it. After the rollback X must be selected again, still
        // paused, and the toggle must offer to RESUME it -- not pause it.
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [ClipRoute("x")];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("x"), ClipRoute("y")];
        host.PauseClipOnProgram("x");
        host.Selection = FakeTransportHost.Clip("x", playing: false, "X paused on Program");
        Assert.Equal("Resume Program", host.ToggleLabel);
        host.SyncThrows = new TimeoutException("media core did not answer within 4000 ms");

        await coordinator.TakeAsync();

        Assert.Equal("x", host.Selection.AssetId);
        Assert.False(host.Selection.Playing);
        Assert.Equal("X paused on Program", host.Selection.Status);
        Assert.Equal("Resume Program", host.ToggleLabel);
        Assert.Contains("x", host.OperatorPausedMediaAssetIds);  // the paused set is untouched
    }

    [Fact]
    public async Task Take_RollbackPutsARollingClipThatLeftProgramBackOnPauseProgram()
    {
        // Report case 3: X is rolling on Program and the failed Take's scene drops it, so the
        // Take cleared X's playing flag. The rollback puts X back on air, still rolling -- the
        // toggle must read "Pause Program", or the operator's press would pause X on air.
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [ClipRoute("x")];
        host.ProgramRoutesByScene["interview"] = [];
        host.Selection = FakeTransportHost.Clip("x", playing: true, "Playing X on Program");
        host.SyncThrows = new InvalidOperationException("native rejected scene");

        await coordinator.TakeAsync();

        Assert.Equal(1, host.RefreshMediaBinPlaybackIndicatorsCallCount);   // the Take cleared it
        Assert.True(host.Selection.Playing);
        Assert.Equal("Playing X on Program", host.Selection.Status);
        Assert.Equal("Pause Program", host.ToggleLabel);
    }

    [Fact]
    public async Task Take_RefusedRollbackLeavesTheSelectionAlone()
    {
        // The scene rollback refuses when newer edits landed during the pending sync; the
        // selection must then stay exactly as the Take (and those edits) left it.
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("y")];
        host.Selection = FakeTransportHost.Clip("bed", playing: false, "BED is ready to cue");
        host.RollbackRefuses = true;
        host.SyncThrows = new InvalidOperationException("native rejected scene");

        await coordinator.TakeAsync();

        Assert.Equal(0, host.RestoreSelectionCallCount);
        Assert.Equal("y", host.Selection.AssetId);
    }

    [Fact]
    public async Task Take_RollbackKeepsASelectionTheOperatorMadeWhileTheSyncWasPending()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("y")];
        host.Selection = FakeTransportHost.Clip("bed", playing: false, "BED is ready to cue");
        var operatorChoice = FakeTransportHost.Clip("z", playing: true, "Auditioning Z");
        host.DuringSync = () => host.Selection = operatorChoice;
        host.SyncThrows = new InvalidOperationException("native rejected scene");

        await coordinator.TakeAsync();

        Assert.Equal(1, host.RestoreSelectionCallCount);         // the bin is still rebuilt once
        Assert.Equal(operatorChoice, host.Selection);
    }

    [Fact]
    public async Task Take_SuccessDoesNotTouchTheSelectionAfterwards()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.ProgramRoutesByScene["intro"] = [];
        host.ProgramRoutesByScene["interview"] = [ClipRoute("y")];

        await coordinator.TakeAsync();

        Assert.Equal(0, host.RestoreSelectionCallCount);
        Assert.Equal("y", host.Selection.AssetId);
        Assert.True(host.Selection.Playing);
    }

    // ---------------------------------------------------------------- Engine

    [Fact]
    public async Task ToggleEngine_InstallsSpineCallback_AndSubscribesZoomCapture()
    {
        var (coordinator, bridge, host) = Build();
        host.ZoomCaptureSubscribed = false;
        bridge.Running = true;

        await coordinator.ToggleEngineAsync();

        Assert.True(bridge.SpineCallbackInstalled);             // ConfigureZoomSpineSync(BuildSpinePayload)
        Assert.True(host.ZoomCaptureSubscribed);
        Assert.True(host.SurfacesCaptureSubscribed);
    }

    [Fact]
    public async Task ToggleEngine_UnsubscribesZoomCapture_WhenAlreadySubscribed()
    {
        var (coordinator, _, host) = Build();
        host.ZoomCaptureSubscribed = true;

        await coordinator.ToggleEngineAsync();

        Assert.Equal(1, host.UnsubscribeCallCount);
    }

    [Fact]
    public async Task ToggleEngine_ReportsUnavailable_WhenCoreDoesNotComeUp()
    {
        var (coordinator, bridge, host) = Build();
        host.ZoomCaptureSubscribed = false;
        bridge.Running = false; // EnsureMediaCoreRunningAsync is a no-op fake; core stays down

        await coordinator.ToggleEngineAsync();

        Assert.False(host.ZoomCaptureSubscribed);
        Assert.False(bridge.SpineCallbackInstalled);
    }

    // ================================================================ fakes

    private sealed class FakeTransportHost : ITransportHost, ITransportDispatcher
    {
        private TaskCompletionSource<bool>? _syncGate;

        public bool Recording { get; set; }

        public bool Streaming { get; set; }

        public bool ZoomCaptureSubscribed { get; set; }

        public string EngineStatus { private get; set; } = string.Empty;

        public string CommandStatus { get; set; } = string.Empty;

        public string OutputStatus { get; set; } = "Outputs idle";

        public string OutputSessionStatus { private get; set; } = string.Empty;

        public string? RecordingDiskWarning { private get; set; }

        public string? SelectedParticipantId => null;

        public string ActiveSceneId { get; set; } = "intro";

        public string PreviewSceneId { get; set; } = "intro";

        public string ProgramSceneSummary => ActiveSceneId;

        public string TakeTransitionLabel => "Fade";

        public bool IsSceneAvailable(string? sceneId) => sceneId is "intro" or "interview";

        // --- test knobs ---
        public bool HoldSync { get; set; }

        public Exception? SyncThrows { get; set; }

        public int SyncFailuresRemaining { get; set; }

        public NativeMediaCoreStateSnapshot SyncResult { get; set; } = new();

        public bool HasPendingCue { get; set; }

        public IReadOnlyList<string> StreamDestinations { get; set; } = ["rtmp://live.example/app/key"];

        public string? StreamValidationError { get; set; }

        // --- observed counters ---
        public int SyncCallCount { get; private set; }
        public List<string> SyncedProgramIds { get; } = [];

        public int PromoteCallCount { get; private set; }

        public int RefreshMediaBinPlaybackIndicatorsCallCount { get; private set; }

        public int GoLiveRecords { get; private set; }

        public IReadOnlyList<SourceRoute>? LastPreviousProgramRoutes { get; private set; }

        // Program routes per scene id; GetResolvedProgramRoutes answers for ActiveSceneId.
        public Dictionary<string, IReadOnlyList<SourceRoute>> ProgramRoutesByScene { get; } = new(StringComparer.Ordinal);

        // Preview-draft routes per scene id; CopyPreviewRoutesToScene commits them when present.
        public Dictionary<string, IReadOnlyList<SourceRoute>> DraftRoutesByScene { get; } = new(StringComparer.Ordinal);

        // A REAL ledger, so the went-live list the coordinator acts on is the production rule.
        private readonly MediaGoLiveLedger _goLive = new();

        public IReadOnlyList<string>? LastWentLive { get; private set; }

        public IReadOnlyList<string>? LastPromoted { get; private set; }

        public int CopyPreviewRoutesCallCount { get; private set; }

        public int RollbackCount { get; private set; }

        public int UnsubscribeCallCount { get; private set; }

        public bool SurfacesCaptureSubscribed { get; private set; }

        public void ReleaseSync() => _syncGate?.TrySetResult(true);

        // --- ITransportDispatcher: run inline (tests assert on the marshalled writes) ---
        public void RunOnUiThread(Action action) => action();

        // --- ITransportHost ---
        public bool HasPendingPreviewChanges(string sceneId) => HasPendingCue;

        public Func<Func<bool>> CaptureTakeRollback()
        {
            var program = ActiveSceneId;
            var preview = PreviewSceneId;
            return () => () =>
            {
                if (RollbackRefuses) return false;
                ActiveSceneId = program;
                PreviewSceneId = preview;
                RollbackCount++;
                return true;
            };
        }

        // --- media selection, modelled on StudioViewModel's Promote / RefreshMediaBin / restore ---
        public bool RollbackRefuses { get; set; }

        public HashSet<string> StillAssetIds { get; } = new(StringComparer.Ordinal);

        public MediaSelectionState Selection { get; set; } =
            new(null, null, null, null, SupportsPlayback: false, Playing: false, "No media asset playing");

        public int RestoreSelectionCallCount { get; private set; }

        public Action? DuringSync { get; set; }

        public void PauseClipOnProgram(string assetId) => _goLive.RecordPause(assetId);

        public static MediaSelectionState Clip(string assetId, bool playing, string status) =>
            new(assetId, assetId.ToUpperInvariant(), $@"C:\media\{assetId}.mp4", "clip", SupportsPlayback: true, playing, status);

        // The transport toggle's label, computed exactly as StudioViewModel.MediaPlaybackButtonLabel does.
        public string ToggleLabel =>
            StudioViewModel.FormatMediaPlaybackActionLabel(
                Selection.AssetId is { Length: > 0 } id &&
                    MediaRoutePlaybackService.IsMediaAssetRoutedOnProgram(id, GetResolvedProgramRoutes()),
                Selection.Playing);

        public MediaSelectionState CaptureMediaSelection() => Selection;

        public IReadOnlyCollection<string> OperatorPausedMediaAssetIds => _goLive.OperatorPausedAssetIds;

        public void RestoreMediaSelectionAfterRollback(MediaSelectionState selection)
        {
            RestoreSelectionCallCount++;
            Selection = selection;
        }

        public void BeginTakeMutation() { }
        public void EndTakeMutation() { }
        public void RequestTakeReconciliation() { }

        public void CopyPreviewRoutesToScene(string sceneId)
        {
            CopyPreviewRoutesCallCount++;
            if (DraftRoutesByScene.TryGetValue(sceneId, out var draft)) ProgramRoutesByScene[sceneId] = draft;
        }

        public bool PromoteProgramMediaRouteToPlayback(IReadOnlyList<string> wentLiveMediaAssetIds)
        {
            PromoteCallCount++;
            LastPromoted = wentLiveMediaAssetIds;
            // Same choice StudioViewModel makes: the selection moves to the clip that went live.
            var promoted = MediaRoutePlaybackService.ChooseAssetToPromote(
                wentLiveMediaAssetIds, Selection.AssetId, id => !StillAssetIds.Contains(id));
            if (promoted is not null)
            {
                Selection = Clip(promoted, playing: true, $"Playing {promoted.ToUpperInvariant()} on Program");
            }
            return true;
        }

        public void RefreshPreviewRoutingState() { }

        public IReadOnlyList<SourceRoute>? LastRefreshPreviousProgramRoutes { get; private set; }

        public void RefreshMediaBinPlaybackIndicators(IReadOnlyList<SourceRoute> previousProgramRoutes)
        {
            RefreshMediaBinPlaybackIndicatorsCallCount++;
            LastRefreshPreviousProgramRoutes = previousProgramRoutes;
            if (MediaRoutePlaybackService.SelectedAssetLeftProgram(
                    Selection.AssetId, previousProgramRoutes, GetResolvedProgramRoutes()))
            {
                Selection = Selection with { Playing = false, Status = $"{Selection.Name} left Program" };
            }
        }

        public IReadOnlyList<SourceRoute> GetResolvedProgramRoutes() =>
            ProgramRoutesByScene.TryGetValue(ActiveSceneId ?? string.Empty, out var routes) ? routes : [];

        public IReadOnlyList<string> RecordProgramMediaGoLive(IReadOnlyList<SourceRoute> previousProgramRoutes)
        {
            GoLiveRecords++;
            LastPreviousProgramRoutes = previousProgramRoutes;
            LastWentLive = _goLive.RecordTake(previousProgramRoutes, GetResolvedProgramRoutes());
            return LastWentLive;
        }

        public Task EnsureMediaCoreRunningAsync(string startingStatus) => Task.CompletedTask;

        public async Task<NativeMediaCoreStateSnapshot> SyncActiveSceneAsync(string? reason = null)
        {
            SyncCallCount++;
            SyncedProgramIds.Add(ActiveSceneId);
            DuringSync?.Invoke();
            if (SyncFailuresRemaining > 0)
            {
                SyncFailuresRemaining--;
                throw new MediaCoreSyncInFlightException();
            }

            if (HoldSync)
            {
                _syncGate = new TaskCompletionSource<bool>();
                await _syncGate.Task;
            }

            if (SyncThrows is not null)
            {
                throw SyncThrows;
            }

            return SyncResult;
        }

        public Task<Dictionary<string, object?>> BuildSpinePayloadAsync(CancellationToken cancellationToken) => Task.FromResult(new Dictionary<string, object?> { ["spine"] = true });

        public void UnsubscribeZoomCapture(string status)
        {
            UnsubscribeCallCount++;
            ZoomCaptureSubscribed = false;
        }

        public void NotifySurfacesCaptureSubscribed(bool subscribed, string? compositorRenderer) =>
            SurfacesCaptureSubscribed = subscribed;

        public void NotifySurfacesPreviewParticipant(string? participantId) { }

        public void RefreshSdkReadiness() { }

        public void RefreshSurfaceBindings() { }

        public void RefreshTransportState() { }

        public void RefreshOutputStatus() { }

        public bool TryEvaluateRecordingDiskPreflight(out IsoDiskPreflightResult result)
        {
            result = null!;
            return false;
        }

        public IReadOnlyList<string> BuildSelectedStreamDestinations(bool validatedOnly) => StreamDestinations;

        public string? ValidateStreamDestinations() => StreamValidationError;

        public void NotifyRecordingCommandCanExecuteChanged() { }

        public void NotifyStreamingCommandCanExecuteChanged() { }

        public string RecordingLogFormat => "mp4";

        public double RecordingLogBitrateMbps => 24;

        public string RecordingVideoCodec => "h264";

        public bool StreamRtmpEnabled => true;

        public bool StreamNdiEnabled => false;

        public bool StreamSrtEnabled => false;

        public double StreamLogBitrateMbps => 8;

        public string StreamVideoCodec => "h264";

        public string StreamEncoderMode => "auto";

        public string FormatStreamDestinationTelemetry(IReadOnlyList<string> destinations) =>
            string.Join(",", destinations);
    }

    private sealed class FakeMediaCoreBridge : IMediaCoreBridge
    {
        public bool Running { get; set; } = true;

        public bool SpineCallbackInstalled { get; private set; }

        public NativeMediaCoreProfile? Profile => null;

        public string ProfileSummary => "GPU 1080p60";

        public NativeMediaCoreStateSnapshot? LastSnapshot => null;

        public NativeMediaCoreStateSnapshot PollResult { get; set; } = new();

#pragma warning disable CS0067 // events are part of the seam surface; the coordinator does not raise them
        public event Action<MediaCoreHealth>? HealthChanged;
        public event Action<string>? StatusChanged;
        public event Action<NativeMediaCoreProfile>? ProfileChanged;
        public event Action<NativeMediaCoreStateSnapshot>? SnapshotChanged;
        public event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
        public event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
        public event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;
        public event Action<ProgramSharedTexture>? PreviewSharedTextureReceived;
        public event Action<ParticipantSharedTexture>? ParticipantSharedTextureReceived;
        public event Action<MultiviewSharedTexture>? MultiviewSharedTextureReceived;
#pragma warning restore CS0067

        public void ConfigureZoomSpineSync(Func<CancellationToken, Task<Dictionary<string, object?>>>? payloadFactory) =>
            SpineCallbackInstalled = payloadFactory is not null;

        public Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult<NativeMediaCoreProfile?>(null);

        public void Stop() { }

        public Task<NativeMediaCoreStateSnapshot> PollSnapshotAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(PollResult);

        // --- unused by the coordinator (the seam surface StudioViewModel wires elsewhere) ---
        public Task<NativeMediaCoreStateSnapshot> SyncAsync(
            IReadOnlyList<NativeMediaCoreCommand> commands, double? elapsedMs = null,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<RawCaptureSnapshot> StopZoomCaptureAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task OpenVstEditorAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetVstParamAsync(string selection, long paramId, double normalized,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task SetVstStateAsync(string selection, string stateBase64,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<string?> GetVstStateAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetCaptureAudioSyncOffsetAsync(string deviceId, int offsetMs,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RegisterCaptureShmAsync(string deviceId, string shmName, int width, int height,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ConnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default, string? outputSourceId = null) =>
            throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ListNativeCaptureDevicesAsync(
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> DisconnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task AddBrowserSourceAsync(string url, int width, int height, int fps,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RemoveBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task ReloadBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
