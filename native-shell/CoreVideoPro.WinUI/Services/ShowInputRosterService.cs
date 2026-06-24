using System.Collections;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class ShowInputRosterService
{
    public const int MaxShowInputs = 10;
    public const int MaxMultiviewBoxes = 10;

    public static IReadOnlyList<ShowInputKindOption> KindOptions { get; } =
    [
        new() { Value = ShowInputKind.Unassigned, Label = "Unassigned" },
        new() { Value = ShowInputKind.ZoomParticipant, Label = "Zoom participant" },
        new() { Value = ShowInputKind.Blackmagic, Label = "Blackmagic SDI/HDMI" },
        new() { Value = ShowInputKind.Aja, Label = "AJA SDI/HDMI" },
        new() { Value = ShowInputKind.UvcWebcam, Label = "UVC webcam" },
        new() { Value = ShowInputKind.SrtIngest, Label = "SRT ingest" },
        new() { Value = ShowInputKind.Media, Label = "Media asset" }
    ];

    /// <summary>Stable identity prefix used to bind a media asset to a route/tile.</summary>
    public const string MediaSourcePrefix = "media:";

    public static string ToMediaSourceId(string assetId) => $"{MediaSourcePrefix}{assetId}";

    public static bool TryGetMediaAssetId(string? sourceId, out string assetId)
    {
        if (sourceId is { Length: > 0 } && sourceId.StartsWith(MediaSourcePrefix, StringComparison.Ordinal))
        {
            assetId = sourceId[MediaSourcePrefix.Length..];
            return assetId.Length > 0;
        }

        assetId = string.Empty;
        return false;
    }

    public static IReadOnlyList<ShowInputSlot> CreateDefaultSlots()
    {
        var slots = new List<ShowInputSlot>(MaxShowInputs);
        for (var index = 1; index <= MaxShowInputs; index++)
        {
            slots.Add(new ShowInputSlot { SlotNumber = index });
        }

        return slots;
    }

    public static IReadOnlyList<ShowInputSourceOption> BuildSourceOptions(
        ShowInputKind kind,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<MediaAsset>? mediaAssets = null)
    {
        return kind switch
        {
            ShowInputKind.Media => (mediaAssets ?? [])
                .Select(asset => new ShowInputSourceOption
                {
                    Value = ToMediaSourceId(asset.Id),
                    Label = string.IsNullOrWhiteSpace(asset.Kind) ? asset.Name : $"{asset.Name} - {asset.Kind}"
                })
                .ToList(),
            ShowInputKind.ZoomParticipant => participants
                .Select(participant => new ShowInputSourceOption
                {
                    Value = participant.Id,
                    Label = $"{participant.Name} - {participant.RoleLabel}"
                })
                .ToList(),
            ShowInputKind.Blackmagic => captureDevices
                .Where(device => device.Vendor.Equals("blackmagic", StringComparison.OrdinalIgnoreCase))
                .Select(device => new ShowInputSourceOption
                {
                    Value = device.Id,
                    Label = device.Name
                })
                .ToList(),
            ShowInputKind.Aja => captureDevices
                .Where(device => device.Vendor.Equals("aja", StringComparison.OrdinalIgnoreCase))
                .Select(device => new ShowInputSourceOption
                {
                    Value = device.Id,
                    Label = device.Name
                })
                .ToList(),
            ShowInputKind.UvcWebcam => captureDevices
                .Where(device =>
                    device.Vendor.Equals("uvc", StringComparison.OrdinalIgnoreCase) ||
                    device.Vendor.Equals("windows", StringComparison.OrdinalIgnoreCase))
                .Select(device => new ShowInputSourceOption
                {
                    Value = device.Id,
                    Label = device.Name
                })
                .ToList(),
            ShowInputKind.SrtIngest => captureDevices
                .Where(device => device.Vendor.Equals("srt", StringComparison.OrdinalIgnoreCase))
                .Select(device => new ShowInputSourceOption
                {
                    Value = device.Id,
                    Label = $"{device.Name} - {device.FormatLabel} - {device.ConnectionLabel}"
                })
                .ToList(),
            _ => []
        };
    }

    /// <summary>
    /// Pure mapping from an assigned Input 1-10 slot onto a <see cref="SourceRoute"/>.
    /// Zoom and Media resolve to a Fixed participant route (Media uses a "media:{assetId}"
    /// identity); capture-class kinds resolve to a CaptureDevice route. Unassigned/unknown
    /// kinds leave the route unchanged. Extracted from the view-model so route resolution can
    /// be unit-tested without the WinUI runtime.
    /// </summary>
    public static void ApplySlotRoute(SourceRoute route, ShowInputSlot slot)
    {
        if (!slot.IsAssigned)
        {
            return;
        }

        if (slot.Kind is ShowInputKind.ZoomParticipant or ShowInputKind.Media)
        {
            route.Mode = SourceRouteMode.Fixed;
            route.ParticipantId = slot.ParticipantId;
            route.CaptureDeviceId = null;
            return;
        }

        if (slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.SrtIngest)
        {
            route.Mode = SourceRouteMode.CaptureDevice;
            route.ParticipantId = null;
            route.CaptureDeviceId = slot.CaptureDeviceId;
        }
    }

    public static IReadOnlyList<ShowInputSourceOption> BuildCaptureSourceOptions(
        IReadOnlyList<CaptureDevice> captureDevices) =>
        [
            new ShowInputSourceOption { Value = string.Empty, Label = "Choose capture source" },
            .. captureDevices.Select(device => new ShowInputSourceOption
            {
                Value = device.Id,
                Label = $"{device.Name} - {device.FormatLabel} - {device.ConnectionLabel}"
            })
        ];

    public static IReadOnlyList<ShowInputSourceOption> BuildAudioSourceOptions(
        IReadOnlyList<AudioCaptureDevice> audioDevices) =>
        [
            new ShowInputSourceOption { Value = string.Empty, Label = "No paired microphone" },
            .. audioDevices.Select(device => new ShowInputSourceOption
            {
                Value = device.Id,
                Label = device.DisplayLabel
            })
        ];

    public static IReadOnlyList<ParticipantSurfaceTile> BuildMultiviewTiles(
        IReadOnlyList<ShowInputSlot> slots,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ParticipantSurfaceTile> participantTiles,
        IReadOnlyDictionary<string, VideoSurfaceState>? captureSurfaces = null,
        IReadOnlyList<MediaAsset>? mediaAssets = null)
    {
        var tilesByParticipant = participantTiles.ToDictionary(tile => tile.Participant.Id, tile => tile);
        var devicesById = captureDevices.ToDictionary(device => device.Id, device => device);
        var participantsById = participants.ToDictionary(participant => participant.Id, participant => participant);
        var mediaAssetsById = (mediaAssets ?? [])
            .ToDictionary(asset => asset.Id, asset => asset, StringComparer.Ordinal);
        captureSurfaces ??= new Dictionary<string, VideoSurfaceState>(StringComparer.Ordinal);

        var assignedDeviceIds = new HashSet<string>(
            slots
                .Where(slot => slot.InShow &&
                    slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.SrtIngest &&
                    !string.IsNullOrWhiteSpace(slot.CaptureDeviceId))
                .Select(slot => slot.CaptureDeviceId!),
            StringComparer.Ordinal);

        var routedTiles = slots
            .Where(slot => slot.InShow && slot.IsAssigned && HasResolvedSource(slot, participantsById, devicesById, mediaAssetsById))
            .Take(MaxMultiviewBoxes)
            .Select(slot => ToSurfaceTile(slot, participantsById, devicesById, tilesByParticipant, captureSurfaces, mediaAssetsById))
            .ToList();

        if (routedTiles.Count >= MaxMultiviewBoxes)
        {
            return routedTiles;
        }

        var nextSourceIndex = routedTiles.Count == 0
            ? 1
            : routedTiles.Max(tile => tile.SourceIndex) + 1;
        foreach (var device in captureDevices.Where(device =>
                     device.IsConnected &&
                     !assignedDeviceIds.Contains(device.Id) &&
                     captureSurfaces.TryGetValue(device.Id, out var surface) &&
                     surface.HasPreviewBitmap))
        {
            routedTiles.Add(ToFallbackCaptureTile(device, captureSurfaces[device.Id], nextSourceIndex++));
            if (routedTiles.Count >= MaxMultiviewBoxes)
            {
                break;
            }
        }

        return routedTiles;
    }

    public static int CountActiveShowInputs(IReadOnlyList<ShowInputSlot> slots) =>
        slots.Count(slot => slot.InShow && slot.IsAssigned && slot.Kind != ShowInputKind.Unassigned);

    public static IReadOnlyList<ParticipantSurfaceTile> SelectVisibleMultiviewTiles(IEnumerable? tiles) =>
        (tiles ?? Array.Empty<ParticipantSurfaceTile>())
            .OfType<ParticipantSurfaceTile>()
            .Where(tile => !tile.IsEmpty)
            .Take(MaxMultiviewBoxes)
            .ToList();

    public static bool SameMultiviewTileStructure(
        IReadOnlyList<ParticipantSurfaceTile> current,
        IReadOnlyList<ParticipantSurfaceTile> next)
    {
        if (current.Count != next.Count)
        {
            return false;
        }

        for (var index = 0; index < current.Count; index++)
        {
            if (current[index].Participant.Id != next[index].Participant.Id ||
                current[index].SourceIndex != next[index].SourceIndex)
            {
                return false;
            }
        }

        return true;
    }

    private static bool HasResolvedSource(
        ShowInputSlot slot,
        IReadOnlyDictionary<string, Participant> participantsById,
        IReadOnlyDictionary<string, CaptureDevice> devicesById,
        IReadOnlyDictionary<string, MediaAsset> mediaAssetsById) =>
        slot.Kind switch
        {
            ShowInputKind.ZoomParticipant =>
                !string.IsNullOrWhiteSpace(slot.ParticipantId) && participantsById.ContainsKey(slot.ParticipantId),
            ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.SrtIngest =>
                !string.IsNullOrWhiteSpace(slot.CaptureDeviceId) && devicesById.ContainsKey(slot.CaptureDeviceId),
            ShowInputKind.Media =>
                TryGetMediaAssetId(slot.ParticipantId, out var mediaAssetId) && mediaAssetsById.ContainsKey(mediaAssetId),
            _ => false
        };

    private static ParticipantSurfaceTile ToSurfaceTile(
        ShowInputSlot slot,
        IReadOnlyDictionary<string, Participant> participantsById,
        IReadOnlyDictionary<string, CaptureDevice> devicesById,
        IReadOnlyDictionary<string, ParticipantSurfaceTile> tilesByParticipant,
        IReadOnlyDictionary<string, VideoSurfaceState> captureSurfaces,
        IReadOnlyDictionary<string, MediaAsset> mediaAssetsById)
    {
        if (slot.Kind == ShowInputKind.ZoomParticipant &&
            slot.ParticipantId is not null &&
            tilesByParticipant.TryGetValue(slot.ParticipantId, out var zoomTile))
        {
            return new ParticipantSurfaceTile
            {
                Participant = zoomTile.Participant,
                Surface = zoomTile.Surface,
                SourceIndex = slot.SlotNumber
            };
        }

        if (slot.Kind == ShowInputKind.ZoomParticipant &&
            slot.ParticipantId is not null &&
            participantsById.TryGetValue(slot.ParticipantId, out var participant))
        {
            return new ParticipantSurfaceTile
            {
                Participant = participant,
                Surface = VideoSurfaceState.Waiting(
                    VideoSurfaceKind.Multiview,
                    $"show-input:{slot.SlotNumber}",
                    participant.Name),
                SourceIndex = slot.SlotNumber
            };
        }

        if (slot.CaptureDeviceId is not null && devicesById.TryGetValue(slot.CaptureDeviceId, out var device))
        {
            var label = $"{device.Name} - {device.ResolutionLabel}";
            var hasLiveSurface = captureSurfaces.TryGetValue(device.Id, out var liveSurface) &&
                liveSurface.HasPreviewBitmap;
            var surface = hasLiveSurface
                ? liveSurface! with
                {
                    SurfaceKey = $"capture:{device.Id}",
                    Kind = VideoSurfaceKind.Multiview,
                    Title = label
                }
                : (device.IsConnected
                    ? VideoSurfaceState.CaptureSourceOnline(VideoSurfaceKind.Multiview, $"capture:{device.Id}", label)
                    : VideoSurfaceState.Waiting(VideoSurfaceKind.Multiview, $"capture:{device.Id}", label)) with
                    {
                        DetailLine = device.IsConnected
                            ? $"{device.ConnectionLabel} - {device.SignalLabel} - waiting for capture frames"
                            : "Connect device in Sources to bring online."
                    };

            return new ParticipantSurfaceTile
            {
                Participant = new Participant
                {
                    Id = $"capture:{device.Id}",
                    Name = label,
                    Title = slot.KindLabel,
                    Role = ParticipantRole.Guest,
                    Health = hasLiveSurface ? FeedHealth.Live : device.IsConnected ? FeedHealth.Live : FeedHealth.VideoOff
                },
                Surface = surface,
                SourceIndex = slot.SlotNumber
            };
        }

        if (slot.Kind == ShowInputKind.Media &&
            TryGetMediaAssetId(slot.ParticipantId, out var mediaAssetId) &&
            mediaAssetsById.TryGetValue(mediaAssetId, out var asset))
        {
            var mediaSourceId = ToMediaSourceId(asset.Id);
            return new ParticipantSurfaceTile
            {
                Participant = new Participant
                {
                    Id = mediaSourceId,
                    Name = asset.Name,
                    Title = asset.Kind,
                    Role = ParticipantRole.Guest,
                    Health = asset.IsPlaying ? FeedHealth.Live : FeedHealth.VideoOff
                },
                Surface = VideoSurfaceState.MediaAssetPreview(
                    mediaSourceId,
                    asset.Name,
                    asset.FilePath,
                    asset.Kind,
                    asset.IsPlaying,
                    naturalSourceWidth: asset.NaturalWidth,
                    naturalSourceHeight: asset.NaturalHeight),
                SourceIndex = slot.SlotNumber
            };
        }

        return ParticipantSurfaceTile.EmptySlot(slot.SlotNumber);
    }

    private static ParticipantSurfaceTile ToFallbackCaptureTile(
        CaptureDevice device,
        VideoSurfaceState liveSurface,
        int sourceIndex)
    {
        var label = $"{device.Name} - {device.ResolutionLabel}";
        return new ParticipantSurfaceTile
        {
            Participant = new Participant
            {
                Id = $"capture:{device.Id}",
                Name = label,
                Title = ResolveCaptureTitle(device),
                Role = ParticipantRole.Guest,
                Health = FeedHealth.Live
            },
            Surface = liveSurface with
            {
                SurfaceKey = $"capture:{device.Id}",
                Kind = VideoSurfaceKind.Multiview,
                Title = label,
                DetailLine = "Live capture fallback - assign to a Show Input to lock its slot."
            },
            SourceIndex = sourceIndex
        };
    }

    private static string ResolveCaptureTitle(CaptureDevice device) =>
        device.Vendor.ToLowerInvariant() switch
        {
            "blackmagic" => "Blackmagic SDI/HDMI",
            "aja" => "AJA SDI/HDMI",
            "srt" => "SRT ingest",
            _ => "UVC webcam"
        };
}
