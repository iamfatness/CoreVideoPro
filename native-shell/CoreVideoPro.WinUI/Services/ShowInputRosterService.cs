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
        new() { Value = ShowInputKind.UvcWebcam, Label = "UVC webcam" }
    ];

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
        IReadOnlyList<CaptureDevice> captureDevices)
    {
        return kind switch
        {
            ShowInputKind.ZoomParticipant => participants
                .Select(participant => new ShowInputSourceOption
                {
                    Value = participant.Id,
                    Label = $"{participant.Name} · {participant.RoleLabel}"
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
            _ => []
        };
    }

    public static IReadOnlyList<ParticipantSurfaceTile> BuildMultiviewTiles(
        IReadOnlyList<ShowInputSlot> slots,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<ParticipantSurfaceTile> participantTiles)
    {
        var tilesByParticipant = participantTiles.ToDictionary(tile => tile.Participant.Id, tile => tile);
        var devicesById = captureDevices.ToDictionary(device => device.Id, device => device);
        var participantsById = participants.ToDictionary(participant => participant.Id, participant => participant);

        return slots
            .Where(slot => slot.InShow && slot.IsAssigned && HasResolvedSource(slot, participantsById, devicesById))
            .Take(MaxMultiviewBoxes)
            .Select(slot => ToSurfaceTile(slot, participantsById, devicesById, tilesByParticipant))
            .ToList();
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
        IReadOnlyDictionary<string, CaptureDevice> devicesById) =>
        slot.Kind switch
        {
            ShowInputKind.ZoomParticipant =>
                !string.IsNullOrWhiteSpace(slot.ParticipantId) && participantsById.ContainsKey(slot.ParticipantId),
            ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam =>
                !string.IsNullOrWhiteSpace(slot.CaptureDeviceId) && devicesById.ContainsKey(slot.CaptureDeviceId),
            _ => false
        };

    private static ParticipantSurfaceTile ToSurfaceTile(
        ShowInputSlot slot,
        IReadOnlyDictionary<string, Participant> participantsById,
        IReadOnlyDictionary<string, CaptureDevice> devicesById,
        IReadOnlyDictionary<string, ParticipantSurfaceTile> tilesByParticipant)
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
            var label = $"{device.Name} · {device.ResolutionLabel}";
            return new ParticipantSurfaceTile
            {
                Participant = new Participant
                {
                    Id = $"capture:{device.Id}",
                    Name = label,
                    Title = slot.KindLabel,
                    Role = ParticipantRole.Guest,
                    Health = device.IsConnected ? FeedHealth.Live : FeedHealth.VideoOff
                },
                Surface = VideoSurfaceState.Waiting(
                    VideoSurfaceKind.Multiview,
                    $"capture:{device.Id}",
                    label) with
                {
                    DetailLine = device.IsConnected
                        ? $"{device.ConnectionLabel} · {device.SignalLabel}"
                        : "Connect device in Sources to bring online."
                },
                SourceIndex = slot.SlotNumber
            };
        }

        return ParticipantSurfaceTile.EmptySlot(slot.SlotNumber);
    }
}