using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>A local (non-Zoom) show input the Tiles wall may seat: its canonical
/// source id (<c>capture:&lt;id&gt;</c> / <c>media:&lt;assetId&gt;</c>), the operator-facing
/// name, and the source-picker group it lists under (Camera, Screen, Media, ...).</summary>
public sealed record TilesLocalSource(string Id, string Name, string Group);

/// <summary>
/// #1071: the Tiles wall admits local sources (webcam, capture card, screen, media) as
/// members, not only Zoom participants. The eligible set is "video-on Zoom participants +
/// every show-input slot bound to a local source that has video". This policy produces the
/// local half from the show-input roster, so <see cref="TilesLayerPayloadBuilder"/> and the
/// Tiles member picker read the same list.
///
/// Why SLOTS and not every enumerated device: a Tiles member is something the operator has
/// put in the show (the same rule the multiviewer and routed mode use via
/// <see cref="ShowInputRosterService.SlotSourceId"/>). An unbound webcam the OS merely lists
/// is not a member candidate; binding it to an input makes it one.
///
/// The core already matches a <c>capture:</c>/<c>media:</c> member against frames by its
/// full scheme-qualified id (MediaCore.cpp's tiles frame-age gather; the tile layer keeps
/// the full id as <c>participantId</c>), so no id rewriting happens here.
/// </summary>
public static class TilesLocalSourcePolicy
{
    /// <summary>A canonical local source id: <c>capture:</c> or <c>media:</c>. These carry
    /// no per-meeting identity, so <see cref="TilesIdentityPolicy"/> persists and resolves them
    /// unchanged — only <c>zoom:</c> ids go through the roster.</summary>
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

    /// <summary>
    /// Every show-input slot bound to a local source with video, in slot order, de-duplicated
    /// by canonical id. A Media slot counts only when its asset is in
    /// <paramref name="visualMediaAssets"/> (video or still) — an audio-only asset has no
    /// picture to tile, and an asset that has left the bin is not invented. A capture slot
    /// always has video; its name is the device name when the device is enumerated, else the
    /// slot label, so a saved-but-unplugged device still reads as something.
    /// <paramref name="displayName"/> is the operator override hook
    /// (<c>StudioViewModel.ResolveSourceDisplayName</c>): (sourceId, derivedName) -> shown name.
    /// </summary>
    public static IReadOnlyList<TilesLocalSource> LocalVideoSources(
        IEnumerable<ShowInputSlot> slots,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<MediaAsset> visualMediaAssets,
        Func<string, string, string>? displayName = null)
    {
        var result = new List<TilesLocalSource>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var slot in slots.OrderBy(slot => slot.SlotNumber))
        {
            if (slot.Kind is ShowInputKind.Unassigned or ShowInputKind.ZoomParticipant)
            {
                continue;
            }

            var id = ShowInputRosterService.SlotSourceId(slot);
            if (!IsLocalSourceId(id) || seen.Contains(id!))
            {
                continue;
            }

            string derived;
            if (slot.Kind == ShowInputKind.Media)
            {
                var asset = visualMediaAssets.FirstOrDefault(asset =>
                    string.Equals(ShowInputRosterService.ToMediaSourceId(asset.Id), id, StringComparison.Ordinal));
                if (asset is null)
                {
                    continue;  // no video (audio-only) or no longer in the bin
                }

                derived = asset.Name;
            }
            else
            {
                derived = captureDevices.FirstOrDefault(device =>
                        string.Equals(device.Id, slot.CaptureDeviceId, StringComparison.Ordinal))?.Name
                    ?? slot.SlotLabel;
            }

            seen.Add(id!);
            result.Add(new TilesLocalSource(id!, displayName?.Invoke(id!, derived) ?? derived, GroupFor(slot.Kind)));
        }

        return result;
    }
}
