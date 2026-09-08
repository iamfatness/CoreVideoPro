namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Typed builders for the 28 <c>ohg.*</c> action ids (Plan 7b Task 2), one method per action,
/// matching the positional/typed param contract in <c>show-engine/src/actions.ts</c>'s
/// <c>OHG_ACTIONS</c> exactly (ints boxed <see cref="int"/>, bools <see cref="bool"/>, everything
/// else <see cref="string"/> — including participant ids and PINs, per that file's owner
/// decision that those are opaque strings, never coerced to <c>int</c>). Callers pass the
/// returned tuple straight to <see cref="IOhgActionInvoker.InvokeAsync"/>.
/// </summary>
public static class OhgActionArgs
{
    // ---- panelist ---------------------------------------------------------------

    /// <summary>
    /// <c>slot: null</c> yields exactly ONE arg (<c>[participantId]</c>), not a trailing
    /// <c>null</c> — the bridge trims trailing nulls before sending anyway, but this builder does
    /// not rely on that: omitting the slot arg entirely is the honest positional-args-with-an-
    /// omitted-optional-tail shape.
    /// </summary>
    public static (string ActionId, object?[] Args) PanelistAdd(string participantId, int? slot = null)
        => slot is int s
            ? ("ohg.panelist.add", new object?[] { participantId, s })
            : ("ohg.panelist.add", new object?[] { participantId });

    public static (string ActionId, object?[] Args) PanelistRemove(int slot)
        => ("ohg.panelist.remove", new object?[] { slot });

    public static (string ActionId, object?[] Args) PanelistReplace(int slot, string participantId)
        => ("ohg.panelist.replace", new object?[] { slot, participantId });

    public static (string ActionId, object?[] Args) PanelistRoleSet(string pin, string role)
        => ("ohg.panelist.role.set", new object?[] { pin, role });

    public static (string ActionId, object?[] Args) PanelistSyncAll()
        => ("ohg.panelist.syncAll", System.Array.Empty<object?>());

    // ---- program ------------------------------------------------------------------

    /// <summary>Wire string per <c>parseProgramSource</c>: <c>black | gallery | activeSpeaker |
    /// look:&lt;id&gt; | slot:&lt;n&gt;</c>. See <see cref="SourceForSlot"/>/<see cref="SourceForLook"/>.</summary>
    public static (string ActionId, object?[] Args) ProgramPreview(string source)
        => ("ohg.program.preview", new object?[] { source });

    public static (string ActionId, object?[] Args) ProgramCut()
        => ("ohg.program.cut", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) ProgramAuto()
        => ("ohg.program.auto", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) ProgramDirectCut(string source)
        => ("ohg.program.directCut", new object?[] { source });

    public static (string ActionId, object?[] Args) ProgramAsFollowSet(bool on)
        => ("ohg.program.asFollow.set", new object?[] { on });

    // ---- look -----------------------------------------------------------------------

    public static (string ActionId, object?[] Args) LookSet(string lookId)
        => ("ohg.look.set", new object?[] { lookId });

    public static (string ActionId, object?[] Args) LookNextGuest()
        => ("ohg.look.nextGuest", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) LookPrevGuest()
        => ("ohg.look.prevGuest", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) LookBoxAssign(int box, int slot)
        => ("ohg.look.box.assign", new object?[] { box, slot });

    public static (string ActionId, object?[] Args) LookBoxClear(int box)
        => ("ohg.look.box.clear", new object?[] { box });

    // ---- gallery -------------------------------------------------------------------

    public static (string ActionId, object?[] Args) GalleryResetFromSlots()
        => ("ohg.gallery.resetFromSlots", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) GalleryReplace(int cell, int slot)
        => ("ohg.gallery.replace", new object?[] { cell, slot });

    public static (string ActionId, object?[] Args) GalleryRemove(int cell)
        => ("ohg.gallery.remove", new object?[] { cell });

    public static (string ActionId, object?[] Args) GalleryEmpty()
        => ("ohg.gallery.empty", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) GallerySmartSet(bool on)
        => ("ohg.gallery.smart.set", new object?[] { on });

    // ---- graphics --------------------------------------------------------------------

    public static (string ActionId, object?[] Args) HeadlineIn()
        => ("ohg.gfx.headline.in", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) HeadlineOut()
        => ("ohg.gfx.headline.out", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) HeadlineChange(string name, string location)
        => ("ohg.gfx.headline.change", new object?[] { name, location });

    public static (string ActionId, object?[] Args) QuestionIn()
        => ("ohg.gfx.question.in", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) QuestionOut()
        => ("ohg.gfx.question.out", System.Array.Empty<object?>());

    // ---- mukana ---------------------------------------------------------------------

    public static (string ActionId, object?[] Args) MukanaSync()
        => ("ohg.mukana.sync", System.Array.Empty<object?>());

    public static (string ActionId, object?[] Args) OverrideSet(string pin, string name, string location, string role)
        => ("ohg.mukana.override.set", new object?[] { pin, name, location, role });

    public static (string ActionId, object?[] Args) OverrideDelete(string pin)
        => ("ohg.mukana.override.delete", new object?[] { pin });

    // ---- ProgramSource wire-string helpers -------------------------------------------

    /// <summary>The <c>slot:&lt;n&gt;</c> wire form of a <c>ProgramSource</c>, per
    /// <c>formatProgramSource</c> in <c>show-engine/src/actions.ts</c>.</summary>
    public static string SourceForSlot(int slot) => $"slot:{slot}";

    /// <summary>The <c>look:&lt;id&gt;</c> wire form of a <c>ProgramSource</c>, per
    /// <c>formatProgramSource</c> in <c>show-engine/src/actions.ts</c>.</summary>
    public static string SourceForLook(string id) => $"look:{id}";
}
