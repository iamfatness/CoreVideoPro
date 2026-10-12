using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class CoreSnapshotRevisionPolicyTests
{
    [Theory]
    [InlineData("a", 8, false)]
    [InlineData("a", 9, true)]
    [InlineData("a", 10, true)]
    [InlineData("b", 0, true)]
    [InlineData("", 0, true)]
    public void OlderAssemblyCannotRestoreStateAndNewCoreCanReset(string epoch, long revision, bool accepted)
    {
        var prior = new NativeMediaCoreStateSnapshot { AppliedControlRevision = 9, AudioObservation = new() { Epoch = "a" } };
        var incoming = new NativeMediaCoreStateSnapshot { AppliedControlRevision = revision, AudioObservation = new() { Epoch = epoch } };
        Assert.Equal(accepted, CoreSnapshotRevisionPolicy.Accept(prior, incoming));
    }
}
