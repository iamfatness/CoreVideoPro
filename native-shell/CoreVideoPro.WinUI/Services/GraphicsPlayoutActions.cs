using CoreVideoPro.Control;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;

namespace CoreVideoPro.WinUI.Services;

/// <summary>How playout buttons and hotkeys reach the control surface (issue #1076). Every UI
/// gesture is an action id + args, the same contract Companion/OSC/HTTP use, so the operator's
/// click and a remote invoke follow one path. MainWindow registers the in-process
/// <see cref="StudioControlSurface"/> in <see cref="Surface"/>; while an id is not yet
/// implemented there (Stage D in flight) or no surface is registered (layout probes), the
/// same action is applied directly to <see cref="StudioViewModel.GraphicsPlayout"/>.</summary>
public static class GraphicsPlayoutActions
{
    public const string PlayIn = "graphics.playIn", ToPreview = "graphics.toPreview", PlayOut = "graphics.playOut",
        PreviewRemove = "graphics.preview.remove", PlayOutAll = "graphics.playOutAll", PreviewClear = "graphics.preview.clear",
        PlaylistDefault = "graphics.playlist.default", HotkeySet = "graphics.playlist.hotkey.set";

    /// <summary>The in-process control surface, registered by MainWindow once it exists.</summary>
    public static Func<string, IReadOnlyList<object?>, CancellationToken, Task<ControlInvokeResult>>? Surface { get; set; }

    public static async Task<ControlInvokeResult> InvokeAsync(StudioViewModel studio, string actionId, params object?[] args)
    {
        if (Surface is { } surface) {
            ControlInvokeResult result;
            try { result = await surface(actionId, args, CancellationToken.None); }
            catch (Exception ex) when (ex is not OutOfMemoryException) { result = ControlInvokeResult.Fail(ex.Message); }
            if (result.Ok || !IsUnimplemented(result)) return result;
        }
        // TODO(#1076 Stage D): once StudioControlSurface implements graphics.playIn / graphics.toPreview /
        // graphics.playOut / graphics.preview.remove / graphics.playOutAll / graphics.preview.clear /
        // graphics.playlist.default / graphics.playlist.hotkey.set this direct path only serves
        // hosts without a registered surface (layout probes).
        try { return await DirectAsync(studio, actionId, args); }
        catch (Exception ex) when (ex is not OutOfMemoryException) { return ControlInvokeResult.Fail(ex.Message); }
    }

    private static bool IsUnimplemented(ControlInvokeResult result) =>
        result.Error?.Contains("not implemented by the WinUI surface", StringComparison.Ordinal) == true;

    /// <summary>The playable entry for a row: a plain row is itself; a Guest row is the entry of
    /// <paramref name="sourceId"/>, or of the current Program subject when none is given, or the
    /// first guest in the roster.</summary>
    public static GraphicsPlaylistEntry? ResolveEntry(StudioViewModel studio, string rowId, string? sourceId)
    {
        if (studio.GraphicsPlayout.Row(rowId) is not { } row) return null;
        if (row.Kind != GraphicsPlaylistRow.Guest) return GraphicsPlayoutService.EntryFor(row);
        var entries = studio.GuestEntries.Where(e => e.RowId == rowId).ToArray();
        if (sourceId is { Length: > 0 }) return entries.FirstOrDefault(e => e.SourceId == sourceId);
        var subject = studio.GraphicsProgramSubjectSourceId;
        return entries.FirstOrDefault(e => subject is not null && e.SourceId == subject) ?? entries.FirstOrDefault();
    }

    private static async Task<ControlInvokeResult> DirectAsync(StudioViewModel studio, string actionId, IReadOnlyList<object?> args)
    {
        var playout = studio.GraphicsPlayout;
        switch (actionId) {
            case PlayIn:
            case ToPreview: {
                if (ResolveEntry(studio, Str(args, 0), Str(args, 1)) is not { } entry) return ControlInvokeResult.Fail(NoEntry(Str(args, 0)));
                return Map(await (actionId == PlayIn ? playout.PlayInAsync(entry) : playout.ToPreviewAsync(entry)));
            }
            case PlayOut: {
                var first = Str(args, 0);
                if (first.StartsWith(GraphicsPlayoutService.InstancePrefix, StringComparison.Ordinal) || args.Count < 2)
                    return Map(await playout.PlayOutAsync(first));
                if (ResolveEntry(studio, first, Str(args, 1)) is not { } entry) return ControlInvokeResult.Fail(NoEntry(first));
                return Map(await playout.PlayOutAsync(entry));
            }
            case PreviewRemove: return Map(await playout.RemoveFromPreviewAsync(Str(args, 0)));
            case PlayOutAll: return Map(await playout.PlayOutAllAsync());
            case PreviewClear: return Map(await playout.ClearPreviewAsync());
            case PlaylistDefault: {
                var starterId = Str(args, 0);
                var starter = GraphicsShowStarters.All.FirstOrDefault(s => s.Id == starterId) ?? GraphicsShowStarters.All[0];
                if (studio.GraphicsWorkspace.State is not { CoreEpoch.Length: > 0 } state)
                    return ControlInvokeResult.Fail("Native Graphics is unavailable. Start the engine, then add your first graphic.");
                IReadOnlyList<GraphicsPlaylistRow> rows;
                try { rows = GraphicsWorkspaceLibrary.DefaultPlaylistFor(starter, state); }
                catch (InvalidOperationException ex) { return ControlInvokeResult.Fail(ex.Message); }
                return await studio.GraphicsLibrary.SavePlaylistAsync(rows) ? ControlInvokeResult.Success : ControlInvokeResult.Fail(studio.GraphicsLibrary.Status);
            }
            case HotkeySet: {
                var rowId = Str(args, 0); var key = Str(args, 1);
                if (!GraphicsPlaylistRow.ValidHotkey(key)) return ControlInvokeResult.Fail("Hotkeys are F1 to F12.");
                if (playout.Row(rowId) is null) return ControlInvokeResult.Fail(NoEntry(rowId));
                var rows = playout.Playlist.Select(r => r.Id == rowId ? r with { Hotkey = key } : key.Length > 0 && r.Hotkey == key ? r with { Hotkey = "" } : r).ToArray();
                return await studio.GraphicsLibrary.SavePlaylistAsync(rows) ? ControlInvokeResult.Success : ControlInvokeResult.Fail(studio.GraphicsLibrary.Status);
            }
            default: return ControlInvokeResult.Fail($"Action '{actionId}' is not a graphics playout action.");
        }
    }

    private static string NoEntry(string rowId) => rowId.Length == 0 ? "Choose a playlist row first." : $"Playlist row '{rowId}' has nothing to play. Add a source in Sources or choose another row.";
    private static ControlInvokeResult Map(NativeGraphicsOperationResult result) => result.Accepted ? ControlInvokeResult.Success : ControlInvokeResult.Fail(result.Reason);
    private static string Str(IReadOnlyList<object?> args, int index) => index < args.Count && args[index] is { } value ? value.ToString() ?? "" : "";
}
