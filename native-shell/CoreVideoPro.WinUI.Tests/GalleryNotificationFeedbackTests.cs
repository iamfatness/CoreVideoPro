using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class GalleryNotificationFeedbackTests
{
    [Fact]
    public void SelectionNotificationEchoesCannotEnterModelEditPipeline()
    {
        var guard = new GalleryEditGuard();
        var settings = "16:9";
        var saves = 0;
        var syncs = 0;
        Action echoSetter = () => guard.TryEdit(() =>
        {
            settings = "4:3";
            saves++;
            syncs++;
        });
        guard.Notify(() => { echoSetter(); echoSetter(); echoSetter(); });
        Assert.Equal("16:9", settings);
        Assert.Equal(0, saves);
        Assert.Equal(0, syncs);
        // Genuine user edits remain enabled once notification delivery finishes.
        echoSetter();
        Assert.Equal("4:3", settings);
        Assert.Equal(1, saves);
        Assert.Equal(1, syncs);
    }

    [Fact]
    public void NestedNotificationKeepsOuterEditProtected()
    {
        var guard = new GalleryEditGuard();
        var nestedEdits = 0;
        Assert.True(guard.TryEdit(() =>
        {
            guard.Notify(() => Assert.False(guard.TryEdit(() => nestedEdits++)));
            Assert.False(guard.TryEdit(() => nestedEdits++));
        }));
        Assert.Equal(0, nestedEdits);
        Assert.True(guard.TryEdit(() => nestedEdits++));
        Assert.Equal(1, nestedEdits);
    }

    [Fact]
    public void SubscriberExceptionRestoresGuardWithoutSwallowingError()
    {
        var guard = new GalleryEditGuard();
        Assert.Throws<InvalidOperationException>(() => guard.Notify(() =>
            throw new InvalidOperationException("binding failure")));
        Assert.True(guard.TryEdit(() => { }));
        Assert.Throws<InvalidOperationException>(() => guard.TryEdit(() =>
            guard.Notify(() => throw new InvalidOperationException("nested binding failure"))));
        Assert.True(guard.TryEdit(() => { }));
    }
}
