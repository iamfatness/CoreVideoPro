using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class RecordingIsoSelectionTests
{
    [Fact]
    public void PassiveLossAndRecoveryDoNotChangeTakeOrAdmitPreviouslyUnavailableSource()
    {
        var selection = new RecordingIsoSelection();
        string[] selected = ["zoom:a", "zoom:b", "zoom:c"];
        Assert.Equal(new[] { "zoom:a", "zoom:b" }, selection.Resolve(true, true, selected, ["zoom:a", "zoom:b"], selected));
        Assert.Equal(new[] { "zoom:a", "zoom:b" }, selection.Resolve(true, true, selected, [], selected));
        Assert.Equal(new[] { "zoom:a", "zoom:b" }, selection.Resolve(true, true, selected, ["zoom:c", "zoom:b"], selected));
        selection.RefreshIntent();
        Assert.Equal(selected, selection.Resolve(true, true, selected, ["zoom:c", "zoom:b"], selected));
    }

    [Fact]
    public void ExplicitDeselectUnassignAndMasterOffRemainAuthoritative()
    {
        var selection = new RecordingIsoSelection();
        string[] both = ["zoom:a", "zoom:b"];
        selection.Resolve(true, true, both, both, both);
        Assert.Equal(new[] { "zoom:b" }, selection.Resolve(true, true, ["zoom:b"], [], both));
        Assert.Empty(selection.Resolve(true, true, both, [], []));
        selection.Resolve(true, true, both, both, both);
        Assert.Empty(selection.Resolve(true, false, both, both, both));
        Assert.Empty(selection.Resolve(true, true, both, [], both));
    }

    [Fact]
    public void IdleAndNextTakeResolveAvailabilityAfresh()
    {
        var selection = new RecordingIsoSelection();
        string[] both = ["zoom:a", "zoom:b"];
        Assert.Equal(new[] { "zoom:a" }, selection.Resolve(false, true, both, ["zoom:a"], both));
        Assert.Empty(selection.Resolve(false, true, both, [], both));
        selection.Resolve(true, true, both, both, both);
        selection.Reset();
        Assert.Equal(new[] { "zoom:b" }, selection.Resolve(true, true, both, ["zoom:b"], both));
    }

    [Fact]
    public void RetainedSourcesKeepTheirSlotsWithinExistingEightWriterLimit()
    {
        var selection = new RecordingIsoSelection();
        var ids = Enumerable.Range(1, 9).Select(i => "zoom:" + i).ToArray();
        Assert.Equal(ids.Take(8), selection.Resolve(true, true, ids, ids, ids));
        selection.RefreshIntent();
        Assert.Equal(ids.Take(8), selection.Resolve(true, true, ids, [ids[8]], ids));
    }
}
