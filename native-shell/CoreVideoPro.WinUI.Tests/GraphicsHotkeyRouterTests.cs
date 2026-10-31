using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using Windows.System;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class GraphicsHotkeyRouterTests
{
    private static readonly GraphicsPlaylistRow[] Rows = [
        new() { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = "Guest name", Hotkey = "F1", FollowSources = true },
        new() { Id = "title", Kind = GraphicsPlaylistRow.Title, Name = "Show title", Hotkey = "F2" },
        new() { Id = "logo", Kind = GraphicsPlaylistRow.Logo, Name = "Logo", Hotkey = "" },
    ];

    [Fact] public void IgnoresKeysWhileTextInputHasFocus()
    {
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F1, VirtualKeyModifiers.None, focusedIsTextInput: true, Rows));
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F12, VirtualKeyModifiers.Shift, focusedIsTextInput: true, Rows));
    }

    [Fact] public void F1MapsToFirstRowPlayIn()
    {
        var route = GraphicsHotkeyRouter.TryRoute(VirtualKey.F1, VirtualKeyModifiers.None, focusedIsTextInput: false, Rows);
        Assert.NotNull(route);
        Assert.Equal("graphics.playIn", route!.ActionId);
        Assert.Equal("guest", route.Args[0]);
        // Guest row: sourceId null so the surface resolves the current Program subject.
        Assert.Equal(2, route.Args.Count); Assert.Null(route.Args[1]);
        var title = GraphicsHotkeyRouter.TryRoute(VirtualKey.F2, VirtualKeyModifiers.None, false, Rows);
        Assert.Equal(["title"], title!.Args);
    }

    [Fact] public void UnassignedKeyIsNotHandled()
    {
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F3, VirtualKeyModifiers.None, false, Rows));
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F7, VirtualKeyModifiers.None, false, Rows));
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.A, VirtualKeyModifiers.None, false, Rows));
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F1, VirtualKeyModifiers.Control, false, Rows));
        // Shift+F1..F11 are reserved for shot layouts: not handled yet, so the key falls through.
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F1, VirtualKeyModifiers.Shift, false, Rows));
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F1, VirtualKeyModifiers.None, false, []));
    }

    [Fact] public void ShiftF12PlaysOutAll()
    {
        var route = GraphicsHotkeyRouter.TryRoute(VirtualKey.F12, VirtualKeyModifiers.Shift, false, Rows);
        Assert.NotNull(route);
        Assert.Equal("graphics.playOutAll", route!.ActionId);
        Assert.Empty(route.Args);
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F12, VirtualKeyModifiers.None, false, Rows));
    }
}
