using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The route-rewrite decision behind <c>StudioViewModel.SetPreviewRouteSlots</c> (the OHG
/// <c>applyLook</c> / <c>setPreview</c> path, spec §8 + D11), extracted as a pure static so it
/// can be unit-tested — <c>StudioViewModel</c> itself is not constructible in tests.
///
/// **Why this exists at all (fix round 1).** The first cut wrote only
/// <c>ShowInputSlotNumber</c> + <c>Mode = Fixed</c> and trusted publish-time resolution to fill
/// in the rest. It does not: <c>StudioViewModel.ResolveRouteFromShowInput</c> and
/// <c>ShowInputRosterService.ApplySlotRoute</c> BOTH return early when the named slot is not
/// assigned, so the route kept the <c>ParticipantId</c>/<c>CaptureDeviceId</c> it happened to
/// carry before — and a look naming an empty box composited THE PREVIOUS GUEST, on preview and
/// then on PROGRAM after the take. A route pointed at an empty slot must render an empty box.
///
/// The operator-equivalent path is <c>SceneCanvasLayerViewModel.ApplyRoute</c>, which always
/// writes <c>ParticipantId</c>, <c>CaptureDeviceId</c> and the kind-derived <c>Mode</c> whenever
/// it writes a slot number. This mirrors it, deferring the assigned case to
/// <see cref="ShowInputRosterService.ApplySlotRoute"/> so there is exactly one kind→route
/// mapping in the codebase.
/// </summary>
internal static class OhgRouteSlotWriter
{
    /// <summary>
    /// Point one stored PREVIEW route at one Show Input slot.
    /// <list type="bullet">
    /// <item><paramref name="slot"/> null ⇒ the route carries nothing:
    /// <see cref="SourceRouteMode.None"/>, every source id cleared.</item>
    /// <item>a slot that is missing or UNASSIGNED ⇒ the slot number is recorded, both source ids
    /// are CLEARED, and the mode is <see cref="SourceRouteMode.Fixed"/> — a fixed route with no
    /// participant renders the placeholder slate (an empty box), which is what the show wants,
    /// and it stays a real layer so the box reappears the moment the slot is filled. It is
    /// deliberately not <c>None</c>: <c>None</c> would drop the layer from the plan entirely.</item>
    /// <item>an ASSIGNED slot ⇒ <see cref="ShowInputRosterService.ApplySlotRoute"/> writes the
    /// kind-derived mode and id (Zoom/Media ⇒ Fixed + ParticipantId; capture-class ⇒
    /// CaptureDevice + CaptureDeviceId).</item>
    /// </list>
    /// <paramref name="resolvedSlot"/> is the <see cref="ShowInputSlot"/> with that number from
    /// the shell's roster, or null when there is none.
    /// </summary>
    internal static void ApplyOhgSlotToRoute(SourceRoute route, int? slot, ShowInputSlot? resolvedSlot)
    {
        // A role-targeted route short-circuits ResolveRouteFromShowInput entirely (R1), so a slot
        // written under a surviving role id would be silently ignored. Clearing it is the same
        // thing the canvas picker does when you choose an input over a role.
        route.ProductionRoleId = null;
        route.SpotlightIndex = null;
        route.ParticipantId = null;
        route.CaptureDeviceId = null;

        if (slot is not { } slotNumber)
        {
            route.Mode = SourceRouteMode.None;
            route.ShowInputSlotNumber = null;
            return;
        }

        route.ShowInputSlotNumber = slotNumber;
        route.Mode = SourceRouteMode.Fixed;

        if (resolvedSlot is not null && resolvedSlot.SlotNumber == slotNumber)
        {
            // No-op when the slot is unassigned — the cleared, Fixed route above is the answer.
            ShowInputRosterService.ApplySlotRoute(route, resolvedSlot);
        }
    }
}
