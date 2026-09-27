using System.Collections.Concurrent;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class LatestRosterFactMailboxTests
{
    [Fact]
    public async Task BurstKeepsOnePendingLatestFactAndDoesNotBlockProducer()
    {
        using var firstEntered = new ManualResetEventSlim();
        using var releaseFirst = new ManualResetEventSlim();
        var delivered = new ConcurrentQueue<long>();
        var mailbox = new LatestRosterFactMailbox((fact, _) =>
        {
            delivered.Enqueue(fact.RosterRevision);
            if (fact.RosterRevision == 1)
            {
                firstEntered.Set();
                releaseFirst.Wait(TimeSpan.FromSeconds(5));
            }
        });
        static RawCaptureSnapshot Fact(long revision) => new()
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:engine", RosterRevision = revision
        };

        mailbox.Post(Fact(1), 0);
        Assert.True(firstEntered.Wait(TimeSpan.FromSeconds(5)));
        for (var revision = 2; revision <= 100; revision++) mailbox.Post(Fact(revision), 0);
        mailbox.Post(Fact(50), 0);
        Assert.Equal(1, mailbox.PendingCount);
        Assert.Equal(98, mailbox.Coalesced);
        Assert.Equal(1, mailbox.DiscardedOlder);
        releaseFirst.Set();
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (delivered.Count < 2 && DateTime.UtcNow < deadline) await Task.Delay(20);
        Assert.Equal([1L, 100L], delivered.ToArray());
    }
}
