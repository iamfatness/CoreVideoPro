using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreSyncInFlightExceptionTests
{
    [Fact]
    public void DerivesFromInvalidOperationException_SoExistingCatchSitesStillWork()
    {
        // Background sync callers catch InvalidOperationException and swallow it;
        // the backpressure signal must stay assignable to that type.
        Assert.IsAssignableFrom<InvalidOperationException>(new MediaCoreSyncInFlightException());
    }

    [Fact]
    public void CarriesTheBackpressureMessage()
    {
        Assert.Equal(
            "media-core sync in flight; skipped for backpressure",
            new MediaCoreSyncInFlightException().Message);
    }
}
