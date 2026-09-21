using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The latest-wins rule for the one-shot set-media-transport gesture (#535 slice 3b, review
/// round 2). The decision was extracted here because the bug it exists to stop lived in the
/// SEND, not in the retry scheduling, and StudioViewModel cannot be constructed in tests.
/// </summary>
public sealed class MediaTransportGestureLedgerTests
{
    [Fact]
    public void ASupersededRetryIsRefusedBeforeItSends()
    {
        // The exact reported sequence:
        //   tap 1 (pause) is skipped for backpressure and schedules a 120 ms retry
        //   tap 2 (resume) claims the slot, sends, completes and RELEASES it
        //   tap 1's already-scheduled retry wakes up against an empty slot
        // Gating only the retry SCHEDULING lets that last step deliver the stale pause on air.
        var ledger = new MediaTransportGestureLedger();

        var pause = ledger.Claim("clip");
        Assert.True(ledger.ShouldSend("clip", pause));
        Assert.Equal(MediaTransportGestureStep.Retry, ledger.OnSkipped("clip", pause, attempt: 0));

        var resume = ledger.Claim("clip");
        Assert.True(ledger.ShouldSend("clip", resume));
        Assert.True(ledger.Release("clip", resume));          // the resume landed and completed

        // The pause's delayed retry now asks to send. It must be refused.
        Assert.False(ledger.ShouldSend("clip", pause));
    }

    [Fact]
    public void ANewerTapSupersedesAnOlderOneWhileItIsStillInFlight()
    {
        var ledger = new MediaTransportGestureLedger();
        var pause = ledger.Claim("clip");
        var resume = ledger.Claim("clip");

        Assert.False(ledger.ShouldSend("clip", pause));
        Assert.True(ledger.ShouldSend("clip", resume));
        Assert.Equal(MediaTransportGestureStep.Superseded, ledger.OnSkipped("clip", pause, attempt: 0));
    }

    [Fact]
    public void ASupersededGestureNeverReleasesTheNewerOnesSlot()
    {
        // Release doubles as "am I still the gesture the operator is waiting on" — a superseded
        // attempt's completion or failure must not clear the slot or write an operator status.
        var ledger = new MediaTransportGestureLedger();
        var pause = ledger.Claim("clip");
        var resume = ledger.Claim("clip");

        Assert.False(ledger.Release("clip", pause));
        Assert.True(ledger.ShouldSend("clip", resume));
        Assert.True(ledger.Release("clip", resume));
    }

    [Fact]
    public void TheSlotIsPerAssetSoTwoClipsDoNotSupersedeEachOther()
    {
        var ledger = new MediaTransportGestureLedger();
        var intro = ledger.Claim("intro");
        var outro = ledger.Claim("outro");

        Assert.True(ledger.ShouldSend("intro", intro));
        Assert.True(ledger.ShouldSend("outro", outro));
    }

    [Fact]
    public void ASkippedGestureReArmsUntilTheAttemptBudgetRunsOut()
    {
        var ledger = new MediaTransportGestureLedger(retryAttempts: 2);
        var token = ledger.Claim("clip");

        Assert.Equal(MediaTransportGestureStep.Retry, ledger.OnSkipped("clip", token, attempt: 0));
        Assert.Equal(MediaTransportGestureStep.Retry, ledger.OnSkipped("clip", token, attempt: 1));
        Assert.Equal(MediaTransportGestureStep.GiveUp, ledger.OnSkipped("clip", token, attempt: 2));

        // Giving up releases the slot, so nothing may send under that token afterwards.
        Assert.False(ledger.ShouldSend("clip", token));
    }

    [Fact]
    public void ACompletedGestureCannotSendAgain()
    {
        var ledger = new MediaTransportGestureLedger();
        var token = ledger.Claim("clip");
        Assert.True(ledger.Release("clip", token));
        Assert.False(ledger.ShouldSend("clip", token));
        Assert.False(ledger.Release("clip", token));
    }

    [Fact]
    public void AnUnknownAssetNeverSends()
    {
        var ledger = new MediaTransportGestureLedger();
        Assert.False(ledger.ShouldSend("clip", 1));
        Assert.Equal(MediaTransportGestureStep.Superseded, ledger.OnSkipped("clip", 1, attempt: 0));
    }

    // ---- the WIRING, not just the leaf ---------------------------------------------------
    //
    // The ledger above is correct in isolation and was NOT what broke: the first cut consulted
    // it only from the retry-scheduling callback, so an already-scheduled retry still reached
    // _bridge.SyncAsync. StudioViewModel is not constructible in tests (CLAUDE.md strangler
    // note), so this reads the send path's own source and fails if the gate is not in front of
    // the send — the same shape as OhgShowPageContentTests.Page_GuardsEveryUiCallback.

    [Fact]
    public void TheSendPathAsksTheLedgerBeforeItReachesTheBridge()
    {
        var body = ReadMethodBody("private async Task SendMediaTransportGestureAsync(");

        var gate = body.IndexOf("_mediaTransportGestures.ShouldSend(", StringComparison.Ordinal);
        var send = body.IndexOf("_bridge.SyncAsync(", StringComparison.Ordinal);

        Assert.True(gate >= 0, "the send path must consult MediaTransportGestureLedger.ShouldSend");
        Assert.True(send >= 0, "the send path must still reach _bridge.SyncAsync");
        Assert.True(
            gate < send,
            "ShouldSend must be asked BEFORE the command reaches the bridge; gating only the " +
            "retry scheduling lets a superseded retry deliver a stale transport action on air.");
    }

    [Fact]
    public void TheRetryMarshalsToTheUiThreadBeforeItAsksToSendAgain()
    {
        // The ledger is not thread-safe and the post-delay continuation lands on the thread
        // pool, so the retry must be back on the UI thread before it consults it.
        var body = ReadMethodBody("private async Task RetryMediaTransportGestureAsync(");

        var delay = body.IndexOf("Task.Delay(", StringComparison.Ordinal);
        var marshal = body.IndexOf("RunOnUiThread(", StringComparison.Ordinal);
        var resend = body.IndexOf("SendMediaTransportGestureAsync(", StringComparison.Ordinal);

        Assert.True(delay >= 0 && marshal >= 0 && resend >= 0);
        Assert.True(delay < marshal, "the delay comes first");
        Assert.True(marshal < resend, "the re-send must be started from inside RunOnUiThread");
    }

    private static string ReadMethodBody(string signature)
    {
        var source = ReadStudioViewModelSource();
        var start = source.IndexOf(signature, StringComparison.Ordinal);
        Assert.True(start >= 0, $"could not find `{signature}` in StudioViewModel.cs");

        var open = source.IndexOf('{', start);
        Assert.True(open >= 0);

        var depth = 0;
        for (var i = open; i < source.Length; i++)
        {
            if (source[i] == '{') depth++;
            else if (source[i] == '}')
            {
                depth--;
                if (depth == 0)
                {
                    return source[open..(i + 1)];
                }
            }
        }

        throw new InvalidOperationException($"unbalanced braces after `{signature}`");
    }

    private static string ReadStudioViewModelSource()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(
                directory.FullName,
                "native-shell",
                "CoreVideoPro.WinUI",
                "ViewModels",
                "StudioViewModel.cs");
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException("Could not locate StudioViewModel.cs from the test output directory.");
    }
}
