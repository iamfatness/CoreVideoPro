using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// #725: a one-apply roster dip on a Take must not reflow the multiview wall.
public sealed class MultiviewParticipantGraceTests
{
    private static Participant P(string id, string name) => new() { Id = id, Name = name };

    private static ShowInputSlot ZoomSlot(int number, string pid, bool inShow = true)
    {
        var slot = new ShowInputSlot { SlotNumber = number };
        slot.Kind = ShowInputKind.ZoomParticipant;
        slot.ParticipantId = pid;
        slot.InShow = inShow;
        return slot;
    }

    private static (List<ShowInputSlot> Slots, List<Participant> All) Show()
    {
        var all = Enumerable.Range(1, 9).Select(i => P($"pid-{i}", $"Guest {i}")).ToList();
        var slots = all.Select((p, i) => ZoomSlot(i + 1, p.Id)).ToList();
        return (slots, all);
    }

    [Fact]
    public void APresentRosterIsReturnedUnchangedAndLogsNothing()
    {
        var (slots, all) = Show();
        var log = new List<string>();
        var grace = new MultiviewParticipantGrace(() => 1000, log.Add);
        Assert.Same(all, grace.Resolve(slots, all, "spine"));
        Assert.Empty(log);
    }

    // The measured case: slot 9's participant (the producer) is missing from one apply.
    [Fact]
    public void AOneApplyDipKeepsTheWallAtNineTiles()
    {
        var (slots, all) = Show();
        var now = 10_000L;
        var log = new List<string>();
        var grace = new MultiviewParticipantGrace(() => now, log.Add);
        grace.Resolve(slots, all, "spine");

        now += 200;
        var dipped = all.Take(8).ToList();
        var resolved = grace.Resolve(slots, dipped, "production-sync", pid => pid == "pid-9");
        var sources = ShowInputRosterService.BuildMultiviewLayoutSources(slots, resolved, []);

        Assert.Equal(9, sources.Count);
        Assert.Contains(sources, s => s.SourceId == "zoom:pid-9");
        Assert.Contains(log, line => line.Contains("slot9 pid=pid-9 missing from production-sync list (n=8)") &&
                                     line.Contains("coreRosterHasIt=yes") && line.Contains("holding"));
        // Without the grace, the same list is the 8-tile reflow the owner saw.
        Assert.Equal(8, ShowInputRosterService.BuildMultiviewLayoutSources(slots, dipped, []).Count);
    }

    [Fact]
    public void AParticipantGoneLongerThanTheGraceIsDroppedAndSaysSo()
    {
        var (slots, all) = Show();
        var now = 10_000L;
        var log = new List<string>();
        var grace = new MultiviewParticipantGrace(() => now, log.Add);
        grace.Resolve(slots, all, "spine");

        now += (long)MultiviewParticipantGrace.Grace.TotalMilliseconds + 1;
        var resolved = grace.Resolve(slots, all.Take(8).ToList(), "spine");
        Assert.Equal(8, resolved.Count);
        Assert.Contains(log, line => line.Contains("slot9 pid=pid-9") && line.Contains("dropped from the multiview"));
    }

    [Fact]
    public void ADipIsReportedOnceAndReArmsAfterTheParticipantReturns()
    {
        var (slots, all) = Show();
        var now = 10_000L;
        var log = new List<string>();
        var grace = new MultiviewParticipantGrace(() => now, log.Add);
        grace.Resolve(slots, all, "spine");
        var dipped = all.Take(8).ToList();

        now += 50; grace.Resolve(slots, dipped, "production-sync");
        now += 50; grace.Resolve(slots, dipped, "production-sync");
        Assert.Single(log);

        now += 50; grace.Resolve(slots, all, "spine");
        now += 50; grace.Resolve(slots, dipped, "production-sync");
        Assert.Equal(2, log.Count);
    }

    [Fact]
    public void ASlotOutOfTheShowIsNeverHeld()
    {
        var (slots, all) = Show();
        slots[8].InShow = false;
        var now = 10_000L;
        var log = new List<string>();
        var grace = new MultiviewParticipantGrace(() => now, log.Add);
        grace.Resolve(slots, all, "spine");
        now += 100;
        var resolved = grace.Resolve(slots, all.Take(8).ToList(), "spine");
        Assert.Equal(8, resolved.Count);
        Assert.Empty(log);
    }
}
