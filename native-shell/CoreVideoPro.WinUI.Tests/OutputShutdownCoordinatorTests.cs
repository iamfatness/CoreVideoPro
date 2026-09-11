using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;
using static CoreVideoPro.WinUI.Tests.CloseGuardPolicyTests;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// T1.8 (#461) "Stop and close": the stops go through the transport, and the close waits for the
/// core's lifecycle evidence FOR THIS STOP, bounded at 15 s. Time is fake: the injected delay
/// advances the clock, and the rig plays the bridge's 250 ms poll by re-stamping the snapshot's
/// <c>RawReceivedUtc</c> on every tick (or not, to model a stale snapshot).
/// </summary>
public sealed class OutputShutdownCoordinatorTests
{
    private static readonly DateTimeOffset Epoch = new(2026, 9, 10, 12, 0, 0, TimeSpan.Zero);

    private sealed class Rig
    {
        public TimeSpan Now = TimeSpan.Zero;
        public bool CoreRunning = true;
        public long Generation;
        public bool RecordingRequested;
        public bool StreamingRequested;
        public bool RecordingInFlight;
        public int IgnoreRecordingStops;
        public bool BridgePolling = true;
        public NativeMediaCoreStateSnapshot? Current;
        public readonly Queue<NativeMediaCoreStateSnapshot?> Next = new();
        public Action<int>? OnTick;
        public int Ticks;
        public int RecordingStops;
        public int StreamingStops;
        public Action? OnRecordingStop;
        public Action? OnStreamingStop;
        public readonly List<string> StopOrder = [];
        public readonly List<string> Log = [];
        public readonly List<string> Progress = [];

        public DateTimeOffset UtcNow => Epoch + Now;

        /// <summary>The snapshot the bridge held BEFORE the stop: received a second before t=0.</summary>
        public Rig WithPreStop(NativeMediaCoreStateSnapshot snapshot)
        {
            Current = snapshot with { RawReceivedUtc = Epoch - TimeSpan.FromSeconds(1) };
            return this;
        }

        public CloseGuardInput State() =>
            new(CoreRunning, RecordingRequested, false, StreamingRequested, false, Current, RecordingInFlight, false, Generation);

        public OutputShutdownCoordinator Build() => new(
            readState: State,
            stopRecording: () =>
            {
                RecordingStops++;
                StopOrder.Add("recording");
                if (RecordingInFlight || IgnoreRecordingStops-- > 0)
                {
                    return Task.CompletedTask; // the transport ignores a stop while a toggle is in flight
                }

                RecordingRequested = false;
                OnRecordingStop?.Invoke();
                return Task.CompletedTask;
            },
            stopStreaming: () =>
            {
                StreamingStops++;
                StopOrder.Add("stream");
                StreamingRequested = false;
                OnStreamingStop?.Invoke();
                return Task.CompletedTask;
            },
            reportProgress: Progress.Add,
            log: Log.Add,
            elapsed: () => Now,
            utcNow: () => UtcNow,
            delay: interval =>
            {
                Now += interval;
                Ticks++;
                if (Next.Count > 0)
                {
                    Current = Next.Dequeue();
                }

                if (BridgePolling && Current is not null)
                {
                    Current = Current with { RawReceivedUtc = UtcNow };
                }

                OnTick?.Invoke(Ticks);
                return Task.CompletedTask;
            });
    }

    private static CloseGuardDecision CloseRequest(Rig rig) => CloseGuardPolicy.Evaluate(rig.State());

    [Fact]
    public async Task DoesNotStopOrWaitWhenNothingWasLive()
    {
        var rig = new Rig().WithPreStop(Snapshot(Recording("completed")));

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.NothingLive, outcome);
        Assert.Equal(0, rig.RecordingStops);
        Assert.Equal(0, rig.StreamingStops);
        Assert.Equal(TimeSpan.Zero, rig.Now);
    }

    [Fact]
    public async Task StopsRecordingAndWaitsForCompleted()
    {
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing")));
        rig.Next.Enqueue(Snapshot(Recording("stopping")));
        rig.Next.Enqueue(Snapshot(Recording("finalizing")));
        rig.Next.Enqueue(Snapshot(Recording("finalizing")));
        rig.Next.Enqueue(Snapshot(Recording("completed")));

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(1, rig.RecordingStops);
        Assert.Equal(0, rig.StreamingStops);
        // It waited for the completed snapshot rather than returning on the stop acknowledgement.
        Assert.Equal(4 * OutputShutdownCoordinator.PollInterval, rig.Now);
        Assert.Equal([OutputShutdownCoordinator.ProgressFinishingRecording], rig.Progress);
    }

    [Fact]
    public async Task AStalePreStopCompletedIsNeverFinished()
    {
        // The bridge's last snapshot left the core BEFORE the stop and already reads completed
        // (the core never answers again). Received before the stop, it is not this stop's evidence.
        var rig = new Rig { RecordingRequested = true, BridgePolling = false }.WithPreStop(Snapshot(Recording("producing")));
        var closeRequest = CloseRequest(rig);
        rig.Current = Snapshot(Recording("completed")) with { RawReceivedUtc = Epoch - TimeSpan.FromMilliseconds(1) };

        var outcome = await rig.Build().StopAndWaitAsync(closeRequest);

        Assert.Equal(OutputShutdownOutcome.TimedOut, outcome);
        Assert.Equal(OutputShutdownCoordinator.FinishTimeout, rig.Now);
    }

    [Fact]
    public async Task ReturnsWhenTheBaselineSessionFails()
    {
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing")));
        rig.Next.Enqueue(Snapshot(Recording("finalizing")));
        rig.Next.Enqueue(Snapshot(Recording("failed")));

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.FinishedWithFailure, outcome);
        Assert.Equal(2 * OutputShutdownCoordinator.PollInterval, rig.Now);
    }

    [Fact]
    public async Task TimesOutAtTheBoundAndSaysSoLoudly()
    {
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing")));
        rig.Next.Enqueue(Snapshot(Recording("finalizing")));

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.TimedOut, outcome);
        // The last evaluation happens AT the deadline and nothing runs after it (finding 5).
        Assert.Equal(OutputShutdownCoordinator.FinishTimeout, rig.Now);
        Assert.Contains(rig.Log, line => line.StartsWith("shutdown: outputs did not finish within 15s — closing anyway", StringComparison.Ordinal));
    }

    [Fact]
    public async Task AnAbsentRecordingNodeRunsToTheBound()
    {
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing")));
        rig.Next.Enqueue(Snapshot(recording: null));

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.TimedOut, outcome);
        Assert.Contains(rig.Log, line => line.Contains("no recording evidence", StringComparison.Ordinal));
    }

    [Fact]
    public async Task StopsStreamsAndWaitsForEverySender()
    {
        var rig = new Rig { StreamingRequested = true }
            .WithPreStop(Snapshot(null, Sender("rtmp://a", "producing"), Sender("srt://b", "producing")));
        rig.Next.Enqueue(Snapshot(null, Sender("rtmp://a", "completed"), Sender("srt://b", "stopping")));
        rig.Next.Enqueue(Snapshot(null, Sender("rtmp://a", "completed"), Sender("srt://b", "completed")));

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(0, rig.RecordingStops);
        Assert.Equal(1, rig.StreamingStops);
        Assert.Equal([OutputShutdownCoordinator.ProgressStoppingStreams], rig.Progress);
    }

    [Fact]
    public async Task ReSendsTheStopWhileIntentIsStillSet()
    {
        // The transport ignored the first two stops (a toggle was in flight), so intent stays set
        // and the stop is re-sent, at most once a second, until one lands.
        var rig = new Rig { RecordingRequested = true, IgnoreRecordingStops = 2 }.WithPreStop(Snapshot(Recording("producing")));
        rig.OnTick = tick =>
        {
            if (!rig.RecordingRequested && rig.Current?.Recording?.Lifecycle?.State == "producing")
            {
                rig.Current = Snapshot(Recording("completed")) with { RawReceivedUtc = rig.UtcNow };
            }
        };

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(3, rig.RecordingStops);
        Assert.True(rig.Now >= TimeSpan.FromSeconds(2), "the re-sends must be rate limited to once a second");
    }

    [Fact]
    public async Task AStartInFlightWithAPreviousCompletedWaitsForTheNewSession()
    {
        // Record pressed just before the close: the start is in flight and the snapshot still shows
        // the PREVIOUS session completed. The close must not read that as finished.
        var rig = new Rig { RecordingRequested = true, RecordingInFlight = true }
            .WithPreStop(Snapshot(Recording("completed", session: "old")));
        var closeRequest = CloseRequest(rig);
        TimeSpan? newSessionCompletedAt = null;
        rig.OnTick = tick =>
        {
            if (tick == 3)
            {
                rig.RecordingInFlight = false; // the start landed
                rig.Current = Snapshot(Recording("producing", session: "new")) with { RawReceivedUtc = rig.UtcNow };
            }
            else if (tick > 3 && !rig.RecordingRequested && rig.Current?.Recording?.Lifecycle?.State == "producing")
            {
                rig.Current = Snapshot(Recording("finalizing", session: "new")) with { RawReceivedUtc = rig.UtcNow };
            }
            else if (rig.Current?.Recording?.Lifecycle?.State == "finalizing")
            {
                rig.Current = Snapshot(Recording("completed", session: "new")) with { RawReceivedUtc = rig.UtcNow };
                newSessionCompletedAt = rig.Now;
            }
        };

        var outcome = await rig.Build().StopAndWaitAsync(closeRequest);

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.NotNull(newSessionCompletedAt);
        Assert.Equal(newSessionCompletedAt, rig.Now);
    }

    [Fact]
    public async Task ACoreRestartDuringTheWaitIsInterruptedNeverFinished()
    {
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing")));
        rig.OnTick = tick =>
        {
            if (tick == 2)
            {
                // The supervisor respawned the core: a fresh core with an idle recording.
                rig.Generation = 1;
                rig.Current = Snapshot(Recording("idle", session: "fresh")) with { RawReceivedUtc = rig.UtcNow };
            }
        };

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.CoreRestarted, outcome);
        Assert.Contains(rig.Log, line => line.Contains("MEDIA CORE RESTARTED", StringComparison.Ordinal) &&
                                         line.Contains("nothing more can be saved", StringComparison.Ordinal));
    }

    [Fact]
    public async Task ACoreThatDiesEndsTheWaitAtOnce()
    {
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing")));
        rig.OnRecordingStop = () => rig.CoreRunning = false;

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.CoreUnavailable, outcome);
        Assert.Equal(TimeSpan.Zero, rig.Now);
        Assert.Contains(rig.Log, line => line.Contains("nothing more can be saved", StringComparison.Ordinal));
    }

    [Fact]
    public async Task StreamsAreStoppedBeforeRecordingOnTheFirstSendAndOnEveryReSend()
    {
        // Fix round 2 (N1): the order is load-bearing (see OutputShutdownCoordinator.SendStops).
        var rig = new Rig { RecordingRequested = true, StreamingRequested = true, IgnoreRecordingStops = 1 }
            .WithPreStop(Snapshot(Recording("producing"), Sender("rtmp://a", "producing")));
        // The ignored recording stop keeps intent set, so there is a re-send; re-arm the stream
        // intent before it so that re-send carries both stops.
        rig.OnTick = tick => { if (tick == 3) rig.StreamingRequested = true; };

        await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.True(rig.StopOrder.Count >= 4, string.Join(",", rig.StopOrder));
        Assert.Equal(["stream", "recording", "stream", "recording"], rig.StopOrder.Take(4));
    }

    [Fact]
    public async Task RecordAndStreamTogetherFinishesInsteadOfTimingOut()
    {
        // Fix round 2 (N1), modelled end to end. The rig plays the core's real behaviour: if the
        // recording stop's batch goes out while Streaming is still desired, it carries a
        // start-program-output that restarts the encoder and ERASES the recording's lifecycle
        // (MediaCore.cpp:2363-2370, AsyncEncoderSink.cpp:117-132) — the recording node then has
        // no lifecycle and never reports completed. In the right order it finalizes normally.
        var rig = new Rig { RecordingRequested = true, StreamingRequested = true }
            .WithPreStop(Snapshot(Recording("producing"), Sender("rtmp://a", "producing")));
        var recordingStopped = false;
        var recordingLifecycleErased = false;
        var streamStopped = false;
        var ticksSinceRecordingStop = 0;
        rig.OnRecordingStop = () =>
        {
            recordingStopped = true;
            recordingLifecycleErased = rig.StreamingRequested; // stop batch built with Streaming=true
        };
        rig.OnStreamingStop = () => streamStopped = true;
        rig.OnTick = _ =>
        {
            if (recordingStopped) ticksSinceRecordingStop++;
            var recording = !recordingStopped
                ? Recording("producing")
                : recordingLifecycleErased
                    ? Recording(null, active: false, status: "stopping")
                    : ticksSinceRecordingStop < 3 ? Recording("finalizing") : Recording("completed");
            var sender = streamStopped ? Sender("rtmp://a", "completed") : Sender("rtmp://a", "producing");
            rig.Current = Snapshot(recording, sender) with { RawReceivedUtc = rig.UtcNow };
        };

        var outcome = await rig.Build().StopAndWaitAsync(CloseRequest(rig));

        Assert.False(recordingLifecycleErased, "the recording stop went out while streaming was still desired");
        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.True(rig.Now < OutputShutdownCoordinator.FinishTimeout, $"ran to {rig.Now}");
    }

    [Fact]
    public async Task ACoreRespawnedWhileTheDialogWasOpenStillHasItsOutputsStopped()
    {
        // Fix round 2 (N2): the generation baseline is armed when the stop is SENT. The old core's
        // file is gone (logged), but the new core is recording again and must be finished too.
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing", session: "old-core")));
        var closeRequest = CloseRequest(rig);
        rig.Generation = 1;
        rig.Current = Snapshot(Recording("producing", session: "new-core")) with { RawReceivedUtc = Epoch - TimeSpan.FromMilliseconds(1) };
        rig.Next.Enqueue(Snapshot(Recording("finalizing", session: "new-core")));
        rig.Next.Enqueue(Snapshot(Recording("completed", session: "new-core")));

        var outcome = await rig.Build().StopAndWaitAsync(closeRequest);

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(1, rig.RecordingStops);
        Assert.Contains(rig.Log, line => line.Contains("MEDIA CORE RESTARTED while the close prompt was open", StringComparison.Ordinal));
    }

    [Fact]
    public async Task ACoreRespawnedWhileTheDialogWasOpenWithNothingLiveNowIsInterrupted()
    {
        var rig = new Rig { RecordingRequested = true }.WithPreStop(Snapshot(Recording("producing", session: "old-core")));
        var closeRequest = CloseRequest(rig);
        rig.Generation = 1;
        rig.RecordingRequested = false;
        rig.Current = Snapshot(Recording("idle", session: "fresh")) with { RawReceivedUtc = Epoch };

        var outcome = await rig.Build().StopAndWaitAsync(closeRequest);

        Assert.Equal(OutputShutdownOutcome.CoreRestarted, outcome);
        Assert.Equal(0, rig.RecordingStops);
    }

    [Fact]
    public async Task AThrowingStopDoesNotSkipTheOtherStopOrTheWait()
    {
        var rig = new Rig { RecordingRequested = true, StreamingRequested = true }
            .WithPreStop(Snapshot(Recording("producing"), Sender("rtmp://a", "producing")));
        rig.Next.Enqueue(Snapshot(Recording("completed"), Sender("rtmp://a", "completed")));
        var streamingStops = 0;
        var coordinator = new OutputShutdownCoordinator(
            readState: () => rig.State() with { RecordingRequested = false, StreamingRequested = false },
            stopRecording: () => throw new InvalidOperationException("boom"),
            stopStreaming: () => { streamingStops++; return Task.CompletedTask; },
            reportProgress: _ => { },
            log: rig.Log.Add,
            elapsed: () => rig.Now,
            utcNow: () => rig.UtcNow,
            delay: interval =>
            {
                // The rig's advancing clock: a regression that stays pending fails at the bound
                // instead of spinning forever.
                rig.Now += interval;
                if (rig.Next.Count > 0) rig.Current = rig.Next.Dequeue();
                rig.Current = rig.Current! with { RawReceivedUtc = rig.UtcNow };
                return Task.CompletedTask;
            });

        var outcome = await coordinator.StopAndWaitAsync(CloseRequest(rig));

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(1, streamingStops);
        Assert.Contains(rig.Log, line => line.Contains("recording stop failed", StringComparison.Ordinal));
    }
}
