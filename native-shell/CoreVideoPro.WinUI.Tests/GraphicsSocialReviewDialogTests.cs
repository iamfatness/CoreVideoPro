using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Controls;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class GraphicsSocialReviewDialogTests
{
    [Fact]
    public void IncomingEditClearsSelectionAndRequiresFreshReview()
    {
        var queue = new GraphicsSocialReview();
        var source = queue.Connect();
        var original = queue.Receive(source, "item", "Author", "Original text")!;
        Assert.Equal(original, GraphicsSocialReviewDialog.PreserveSelection(queue.Items, original.Key));
        Assert.True(queue.Approve(original.Key));
        Assert.Equal(GraphicsSocialDecision.Approved, GraphicsSocialReviewDialog.PreserveSelection(queue.Items, original.Key)?.Decision);

        var edited = queue.Receive(source, "item", "Author", "Changed text")!;
        Assert.Null(GraphicsSocialReviewDialog.PreserveSelection(queue.Items, original.Key));
        Assert.Equal(edited, GraphicsSocialReviewDialog.PreserveSelection(queue.Items, edited.Key));
        Assert.Null(GraphicsSocialReviewDialog.PreserveSelection(queue.Items, null));
    }
}
