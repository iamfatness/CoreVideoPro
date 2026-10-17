using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class PresentationCreditTests
{
    [Theory]
    [InlineData(PresentationAttempt.StillDrawing)]
    [InlineData(0x087A0001)]
    public void DeferredPresentRetriesWithoutWaitingForAnUnsubmittedFrame(int result)
    {
        var credit = new PresentationCredit();
        var signals = 1;
        bool Poll() => signals-- > 0;
        Assert.True(credit.TryAcquire(Poll));
        Assert.False(PresentationAttempt.Commit(result, credit.Commit));
        Assert.True(credit.TryAcquire(Poll));
        Assert.Equal(0, signals); // No second wait: no new frame was submitted.
        Assert.True(PresentationAttempt.Commit(0, credit.Commit));
        Assert.False(credit.TryAcquire(Poll));
    }

    [Fact]
    public void RecreatedChainCannotUseRetiredChainsCredit()
    {
        var credit = new PresentationCredit();
        Assert.True(credit.TryAcquire(() => true));
        credit.Reset();
        Assert.False(credit.TryAcquire(() => false));
    }
}
