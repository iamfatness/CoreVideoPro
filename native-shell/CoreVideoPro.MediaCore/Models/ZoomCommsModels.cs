namespace CoreVideoPro.MediaCore.Models;

// Comms (Zoom talkback, docs/reference/comms-talkback.md): the core's
// "zoom-comms-state" fact, i.e. the Zoom engine's last comms_state re-keyed to
// camelCase. Same shape as CoreVideo Edge's `comms` state object (EdgeComms),
// so the two apps read one contract. Every value is what the engine reported;
// nothing here is inferred by the shell.
public sealed record ZoomCommsChannel(int Slot, string? Label, int Listeners, bool Ready, bool Keyed, bool Latched,
    string? Room, int ReachOk, int ReachDark);

public sealed record ZoomCommsPerson(uint ParticipantId, string Name, bool Talkback, string? Room, IReadOnlyList<int> Channels);

public sealed record ZoomCommsMic(string? Device, IReadOnlyList<string> Devices, double? PeakDbfs, int GainDb, bool Sidetone,
    string? SidetoneDevice, IReadOnlyList<string> SidetoneDevices, bool Tone);

public sealed record ZoomCommsState(
    bool Available,
    string? Reason,
    bool LampLink,
    bool LampMeetingMic,
    bool LampChannel,
    bool LampTx,
    string? Room,
    IReadOnlyList<ZoomCommsChannel> Channels,
    IReadOnlyList<ZoomCommsPerson> Roster,
    ZoomCommsMic Mic,
    long FramesSent,
    long ChannelSends,
    long Underruns,
    long SendErrors,
    IReadOnlyList<string> Log,
    // Private comms applied by the engine: the meeting mic is the silent
    // virtual mic (Pro's toggle, or a capture-only station, always private).
    bool Private = false);

/// <summary>One "zoom-comms-state" fact. <see cref="State"/> null = the core dropped
/// the report (Leave, engine gone): the shell must stop showing any key as live.</summary>
public sealed record ZoomCommsUpdate(ZoomCommsState? State);

/// <summary>The core's answer to one comms-* request. <see cref="Error"/> is operator text.</summary>
public sealed record ZoomCommsCommandResult(bool Ok, string? Error)
{
    public static readonly ZoomCommsCommandResult Success = new(true, null);
}
