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
        // Shift+F1..F11 belong to shot layouts: with none, the key falls through.
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F1, VirtualKeyModifiers.Shift, false, Rows));
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F1, VirtualKeyModifiers.None, false, []));
    }

    private static readonly ShotLayout[] Layouts = [
        new() { Id = "open", Name = "Open", SceneId = "intro", RowIds = ["title", "logo"], Hotkey = "Shift+F1" },
        new() { Id = "two-up", Name = "Two-up", SceneId = "interview", RowIds = ["guest"], Hotkey = "Shift+F2" },
        new() { Id = "clean", Name = "Clean", Hotkey = "Shift+F11", PlayOutOthers = true },
        new() { Id = "unkeyed", Name = "No key", SceneId = "panel" },
    ];

    [Fact] public void ShiftF2CuesTheLayoutWithThatHotkey()
    {
        var route = GraphicsHotkeyRouter.TryRoute(VirtualKey.F2, VirtualKeyModifiers.Shift, focusedIsTextInput: false, Rows, Layouts);
        Assert.NotNull(route);
        Assert.Equal("graphics.layout.cue", route!.ActionId);
        Assert.Equal(["two-up"], route.Args);
        Assert.Equal(["clean"], GraphicsHotkeyRouter.TryRoute(VirtualKey.F11, VirtualKeyModifiers.Shift, false, Rows, Layouts)!.Args);
        // Shift+F12 stays Play Out all even with layouts; unassigned Shift keys fall through;
        // plain F2 is still the playlist row; nothing while typing.
        Assert.Equal("graphics.playOutAll", GraphicsHotkeyRouter.TryRoute(VirtualKey.F12, VirtualKeyModifiers.Shift, false, Rows, Layouts)!.ActionId);
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F3, VirtualKeyModifiers.Shift, false, Rows, Layouts));
        Assert.Equal("graphics.playIn", GraphicsHotkeyRouter.TryRoute(VirtualKey.F2, VirtualKeyModifiers.None, false, Rows, Layouts)!.ActionId);
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F2, VirtualKeyModifiers.Shift, true, Rows, Layouts));
        Assert.Null(GraphicsHotkeyRouter.TryRoute(VirtualKey.F2, VirtualKeyModifiers.Shift, false, Rows, []));
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
