using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// Regression (2026-09-10, beta-2026-09-10-5a24225, live): the meters froze for a whole session.
/// Several startup edits call <c>EnsureMediaCoreRunningAsync</c> → <c>StartAsync</c> → <c>StartPolling</c>
/// at once, and start/stop were not atomic. A heap dump showed the one live poll timer armed with
/// generation 17 while the runner was at 20, so every 250 ms tick was a silent no-op: the shell's copy
/// of core state only refreshed when an operator command's reply happened to carry it.
/// </summary>
public sealed class MediaCoreBridgePollTimerRaceTests
{
    [Fact]
    public async Task ConcurrentStartsAndStopsAlwaysLeaveTheLatestStartArmedWithTheCurrentGeneration()
    {
        await using var bridge = new MediaCoreBridgeService(new MediaCoreSupervisor(new MediaCoreSupervisorOptions()));

        for (var round = 0; round < 1000; round++)
        {
            using var go = new ManualResetEventSlim(false);
            var workers = Enumerable.Range(0, 8).Select(i => Task.Run(() =>
            {
                go.Wait();
                // Mostly starts (the launch storm), with stops mixed in (a restart, a leave/rejoin).
                if (i % 4 == 3) bridge.StopPolling(); else bridge.StartPolling();
            })).ToArray();
            go.Set();
            await Task.WhenAll(workers);

            // Whatever the interleaving, a final start must leave a live, current poll.
            bridge.StartPolling();
            Assert.True(bridge.PollTimerIsCurrent, $"round {round}: the installed poll timer carries a stale generation, so the poll is dead");
        }
    }

    [Fact]
    public async Task ConcurrentStartsNeverLeaveAStaleTimerInstalled()
    {
        await using var bridge = new MediaCoreBridgeService(new MediaCoreSupervisor(new MediaCoreSupervisorOptions()));

        for (var round = 0; round < 1000; round++)
        {
            using var go = new ManualResetEventSlim(false);
            var workers = Enumerable.Range(0, 8).Select(_ => Task.Run(() => { go.Wait(); bridge.StartPolling(); })).ToArray();
            go.Set();
            await Task.WhenAll(workers);

            // Only starts ran, so SOME start was last — its timer must be the installed, current one.
            Assert.True(bridge.PollTimerIsCurrent, $"round {round}: concurrent starts left a stale poll timer installed");
        }
    }
}
