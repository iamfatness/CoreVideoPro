using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>A local (non-Zoom) show input the Tiles wall may seat: its canonical
/// source id (<c>capture:&lt;id&gt;</c>), the operator-facing name, and the source-picker
/// group it lists under (Camera, Screen, Browser, SRT, NDI, RTMP).</summary>
public sealed record TilesLocalSource(string Id, string Name, string Group);

/// <summary>
/// #1071: the Tiles wall admits local sources (webcam, capture card, screen) as members,
/// not only Zoom participants. The eligible set is "video-on Zoom participants + every
/// show-input slot that is IN SHOW and bound to a local source with video". This policy
/// produces the local half from the show-input roster, so <see cref="TilesLayerPayloadBuilder"/>
/// and the Tiles member picker read the same list.
///
/// Why IN-SHOW slots, not every bound slot (review of PR #1081, finding 3): a slot bound
/// for a Camera scene but not in the show is not brought online
/// (<c>StudioViewModel.QueueSelectedCaptureDevicesOnline</c> connects InShow devices only),
/// so seating it would either hold a never-drawn member or surprise a Zoom-only wall on
/// upgrade with a webcam tile nobody asked for. In-show is the operator's explicit "this
/// is part of the show" — the same rule routed mode and the multiviewer use.
///
/// Why no <c>media:</c> members yet (finding 1): the core opens a media asset only for a
/// scene ROUTE (it needs the asset path, which a Tiles member id does not carry;
/// <c>MediaCore::syncStillMediaDesired</c> / <c>syncMediaTransportsDesired</c>), so a
/// Media tile would never receive a frame. Media as a Tiles member is a follow-up that
/// adds the asset path to the tiles wire node.
///
/// The core matches a <c>capture:</c> member against frames by its full scheme-qualified
/// id (MediaCore.cpp's tiles frame-age gather; the tile layer keeps the full id as
/// <c>participantId</c>), so no id rewriting happens here.
/// </summary>
public static class TilesLocalSourcePolicy
{
    /// <summary>A canonical local source id: <c>capture:</c> or <c>media:</c>. These carry
    /// no per-meeting identity, so <see cref="TilesIdentityPolicy"/> persists and resolves them
    /// unchanged — only <c>zoom:</c> ids go through the roster. (<c>media:</c> is recognised
    /// so a saved id round-trips honestly even though it is not yet a member candidate.)</summary>
    public static bool IsLocalSourceId(string? sourceId) =>
        sourceId is { Length: > 0 } &&
        (sourceId.StartsWith(ShowInputRosterService.CaptureSourcePrefix, StringComparison.Ordinal) ||
         sourceId.StartsWith(ShowInputRosterService.MediaSourcePrefix, StringComparison.Ordinal));

    /// <summary>The source-picker group for a local input kind — the same words the
    /// show-input picker uses (<see cref="ShowInputRosterService.BuildUnifiedSourceOptions"/>).</summary>
    public static string GroupFor(ShowInputKind kind) => kind switch
    {
        ShowInputKind.Screen => "Screen",
        ShowInputKind.SrtIngest => "SRT",
        ShowInputKind.RtmpIngest => "RTMP",
        ShowInputKind.Ndi => "NDI",
        ShowInputKind.Browser => "Browser",
        ShowInputKind.Media => "Media",
        _ => "Camera"
    };

    /// <summary>The ids routed mode filters to: every in-show slot's canonical id, Zoom and
    /// local alike. One definition for <c>BuildTilesLayerWire</c> and the workspace member
    /// states, so "routed" means the same thing on the wire and in the editor.</summary>
    public static HashSet<string> RoutedSourceIds(IEnumerable<ShowInputSlot> slots) =>
        slots.Where(slot => slot.InShow)
            .Select(ShowInputRosterService.SlotSourceId)
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Select(id => id!)
            .ToHashSet(StringComparer.Ordinal);

    /// <summary>
    /// Every IN-SHOW slot bound to a local capture source (webcam, SDI/HDMI, screen, browser,
    /// SRT, NDI, RTMP), in slot order, de-duplicated by canonical id. Media slots are not
    /// candidates yet (see the class remarks). The name is the device name when the device is
    /// enumerated, else the slot label, so a saved-but-unplugged device still reads as
    /// something. <paramref name="displayName"/> is the operator override hook
    /// (<c>StudioViewModel.ResolveSourceDisplayName</c>): (sourceId, derivedName) -> shown name.
    /// </summary>
    public static IReadOnlyList<TilesLocalSource> LocalVideoSources(
        IEnumerable<ShowInputSlot> slots,
        IReadOnlyList<CaptureDevice> captureDevices,
        Func<string, string, string>? displayName = null)
    {
        var result = new List<TilesLocalSource>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var slot in slots.OrderBy(slot => slot.SlotNumber))
        {
            if (!slot.InShow || slot.Kind is ShowInputKind.Unassigned or ShowInputKind.ZoomParticipant or ShowInputKind.Media)
            {
                continue;
            }

            var id = ShowInputRosterService.SlotSourceId(slot);
            if (!IsLocalSourceId(id) || !seen.Add(id!))
            {
                continue;
            }

            var derived = captureDevices.FirstOrDefault(device =>
                    string.Equals(device.Id, slot.CaptureDeviceId, StringComparison.Ordinal))?.Name
                ?? slot.SlotLabel;
            result.Add(new TilesLocalSource(id!, displayName?.Invoke(id!, derived) ?? derived, GroupFor(slot.Kind)));
        }

        return result;
    }
}
