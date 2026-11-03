using CoreVideoPro.MediaCore.Models;
using Windows.System;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Pure F-key routing for graphics playout (issue #1076). F1..F12 unmodified play in
/// the playlist row carrying that hotkey (a Guest row resolves to the current Program subject
/// downstream, so sourceId is passed as null). Shift+F12 plays everything out; Shift+F1..F11
/// cue the shot layout carrying that hotkey (slice 2). Nothing routes while a text input has
/// focus, so typing a name never fires a graphic.</summary>
public static class GraphicsHotkeyRouter
{
    public sealed record Route(string ActionId, IReadOnlyList<object?> Args);

    public const string PlayInAction = "graphics.playIn", PlayOutAllAction = "graphics.playOutAll", LayoutCueAction = "graphics.layout.cue";

    public static Route? TryRoute(VirtualKey key, VirtualKeyModifiers modifiers, bool focusedIsTextInput, IReadOnlyList<GraphicsPlaylistRow> rows) =>
        TryRoute(key, modifiers, focusedIsTextInput, rows, []);

    public static Route? TryRoute(VirtualKey key, VirtualKeyModifiers modifiers, bool focusedIsTextInput, IReadOnlyList<GraphicsPlaylistRow> rows, IReadOnlyList<ShotLayout> layouts)
    {
        if (focusedIsTextInput || key < VirtualKey.F1 || key > VirtualKey.F12) return null;
        var number = (int)key - (int)VirtualKey.F1 + 1;
        if (modifiers == VirtualKeyModifiers.Shift) {
            if (number == 12) return new(PlayOutAllAction, []);
            var layout = layouts?.FirstOrDefault(l => l.Hotkey == ShotLayout.HotkeyPrefix + number);
            return layout is null ? null : new(LayoutCueAction, [layout.Id]);
        }
        if (modifiers != VirtualKeyModifiers.None) return null;
        var hotkey = "F" + number;
        var row = rows?.FirstOrDefault(r => r.Hotkey == hotkey);
        if (row is null) return null;
        return row.Kind == GraphicsPlaylistRow.Guest ? new(PlayInAction, [row.Id, null]) : new(PlayInAction, [row.Id]);
    }
}
