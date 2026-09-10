using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;
using static CoreVideoPro.WinUI.Tests.CloseGuardPolicyTests;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// T1.8 (#461) "Stop and close": the stops go through the transport, and the close waits for the
/// core's lifecycle evidence, bounded at 15 s. Time is fake: the injected delay advances the clock.
/// </summary>
public sealed class OutputShutdownCoordinatorTests
{
    private sealed class Rig
    {
        public TimeSpan Now = TimeSpan.Zero;
        public bool CoreRunning = true;
        public bool RecordingRequested;
        public bool StreamingRequested;
        public NativeMediaCoreStateSnapshot? LastSnapshot;
        public readonly Queue<NativeMediaCoreStateSnapshot?> Polls = new();
        public int RecordingStops;
        public int StreamingStops;
        public int PollCount;
        public readonly List<string> Log = [];
        public readonly List<string> Progress = [];
        public Exception? PollThrows;
        public Action? OnRecordingStop;

        public OutputShutdownCoordinator Build() => new(
            readState: () => new CloseGuardInput(CoreRunning, RecordingRequested, false, StreamingRequested, false, LastSnapshot),
            stopRecording: () => { RecordingStops++; RecordingRequested = false; OnRecordingStop?.Invoke(); return Task.CompletedTask; },
            stopStreaming: () => { StreamingStops++; StreamingRequested = false; return Task.CompletedTask; },
            pollSnapshot: _ =>
            {
                PollCount++;
                if (PollThrows is { } error) throw error;
                // The last queued snapshot repeats: a core that stays in one state.
                var next = Polls.Count > 1 ? Polls.Dequeue() : Polls.Count == 1 ? Polls.Peek() : null;
                LastSnapshot = next ?? LastSnapshot;
                return Task.FromResult(next);
            },
            reportProgress: Progress.Add,
            log: Log.Add,
            elapsed: () => Now,
            delay: interval => { Now += interval; return Task.CompletedTask; });
    }

    [Fact]
    public async Task DoesNotStopOrWaitWhenNothingWasLive()
    {
        var rig = new Rig { LastSnapshot = Snapshot(Recording("completed")) };

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.NothingLive, outcome);
        Assert.Equal(0, rig.RecordingStops);
        Assert.Equal(0, rig.StreamingStops);
        Assert.Equal(0, rig.PollCount);
        Assert.Equal(TimeSpan.Zero, rig.Now);
    }

    [Fact]
    public async Task StopsRecordingAndWaitsForCompleted()
    {
        var rig = new Rig { RecordingRequested = true, LastSnapshot = Snapshot(Recording("producing")) };
        rig.Polls.Enqueue(Snapshot(Recording("stopping")));
        rig.Polls.Enqueue(Snapshot(Recording("finalizing")));
        rig.Polls.Enqueue(Snapshot(Recording("finalizing")));
        rig.Polls.Enqueue(Snapshot(Recording("completed")));

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(1, rig.RecordingStops);
        Assert.Equal(0, rig.StreamingStops);
        Assert.Equal(4, rig.PollCount);
        // It waited between polls rather than returning on the stop acknowledgement.
        Assert.Equal(3 * OutputShutdownCoordinator.PollInterval, rig.Now);
        Assert.Equal([OutputShutdownCoordinator.ProgressFinishingRecording], rig.Progress);
    }

    [Fact]
    public async Task ReturnsWhenADestinationFails()
    {
        var rig = new Rig { RecordingRequested = true, LastSnapshot = Snapshot(Recording("producing")) };
        rig.Polls.Enqueue(Snapshot(Recording("finalizing")));
        rig.Polls.Enqueue(Snapshot(Recording("failed")));

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.FinishedWithFailure, outcome);
        Assert.Equal(2, rig.PollCount);
        Assert.True(rig.Now < OutputShutdownCoordinator.FinishTimeout);
    }

    [Fact]
    public async Task TimesOutAtTheBoundAndSaysSoLoudly()
    {
        var rig = new Rig { RecordingRequested = true, LastSnapshot = Snapshot(Recording("producing")) };
        rig.Polls.Enqueue(Snapshot(Recording("finalizing")));

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.TimedOut, outcome);
        Assert.Equal(OutputShutdownCoordinator.FinishTimeout, rig.Now);
        Assert.Contains(rig.Log, line => line.StartsWith("shutdown: outputs did not finish within 15s — closing anyway", StringComparison.Ordinal));
    }

    [Fact]
    public async Task StopsStreamsAndWaitsForEverySender()
    {
        var rig = new Rig
        {
            StreamingRequested = true,
            LastSnapshot = Snapshot(null, Sender("rtmp://a", "producing"), Sender("srt://b", "producing"))
        };
        rig.Polls.Enqueue(Snapshot(null, Sender("rtmp://a", "completed"), Sender("srt://b", "stopping")));
        rig.Polls.Enqueue(Snapshot(null, Sender("rtmp://a", "completed"), Sender("srt://b", "completed")));

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(0, rig.RecordingStops);
        Assert.Equal(1, rig.StreamingStops);
        Assert.Equal([OutputShutdownCoordinator.ProgressStoppingStreams], rig.Progress);
    }

    [Fact]
    public async Task ReSendsAStopThatDidNotLand()
    {
        // A toggle was in flight when the close came, so the transport ignored the first stop and
        // the core keeps producing. The stop is re-sent (at most once a second) until it lands.
        var rig = new Rig { RecordingRequested = true, LastSnapshot = Snapshot(Recording("producing")) };
        for (var i = 0; i < 8; i++) rig.Polls.Enqueue(Snapshot(Recording("producing")));
        rig.Polls.Enqueue(Snapshot(Recording("completed")));

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.True(rig.RecordingStops >= 2, $"expected a re-sent stop, saw {rig.RecordingStops}");
        Assert.True(rig.RecordingStops <= 3, $"re-sends must be rate limited, saw {rig.RecordingStops}");
    }

    [Fact]
    public async Task ReturnsImmediatelyWhenTheCoreIsGone()
    {
        var rig = new Rig { RecordingRequested = true, LastSnapshot = Snapshot(Recording("producing")) };
        rig.PollThrows = new InvalidOperationException("Media core is not running.");
        // The core dies right after the stop is sent.
        rig.OnRecordingStop = () => rig.CoreRunning = false;

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.CoreUnavailable, outcome);
        Assert.Equal(TimeSpan.Zero, rig.Now);
    }

    [Fact]
    public async Task ATransientPollFailureKeepsWaitingInsideTheBound()
    {
        var rig = new Rig { RecordingRequested = true, LastSnapshot = Snapshot(Recording("producing")) };
        rig.PollThrows = new TimeoutException("slow response");

        var outcome = await rig.Build().StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.TimedOut, outcome);
        Assert.Equal(OutputShutdownCoordinator.FinishTimeout, rig.Now);
    }

    [Fact]
    public async Task AThrowingStopDoesNotSkipTheOtherStopOrTheWait()
    {
        var streamingStops = 0;
        var polls = 0;
        var coordinator = new OutputShutdownCoordinator(
            readState: () => new CloseGuardInput(true, true, false, true, false, null),
            stopRecording: () => throw new InvalidOperationException("boom"),
            stopStreaming: () => { streamingStops++; return Task.CompletedTask; },
            pollSnapshot: _ => { polls++; return Task.FromResult<NativeMediaCoreStateSnapshot?>(Snapshot(Recording("completed"))); },
            reportProgress: _ => { },
            log: _ => { },
            elapsed: () => TimeSpan.Zero,
            delay: _ => Task.CompletedTask);

        var outcome = await coordinator.StopAndWaitAsync();

        Assert.Equal(OutputShutdownOutcome.Finished, outcome);
        Assert.Equal(1, streamingStops);
        Assert.Equal(1, polls);
    }
}
