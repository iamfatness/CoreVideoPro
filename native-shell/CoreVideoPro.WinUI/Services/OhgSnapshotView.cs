using System.Collections.Generic;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Pure view records projected from the show-engine's wire <c>ShowSnapshot</c>
/// (see <c>show-engine/src/showSnapshot.ts</c>) by <see cref="OhgSnapshotProjection"/>. No WinUI
/// types here — these are plain .NET records so the projection is unit-testable without a
/// <c>DispatcherQueue</c>.</summary>
public sealed record OhgPanelistRow(
    string ParticipantId,
    string DisplayName,
    string Location,
    string? Pin,
    bool HasMukana,
    string Role,
    bool Online,
    bool VideoOn,
    bool AudioOn,
    bool HandRaised,
    int? Slot /* seated slot or null */);

public sealed record OhgSlotRow(int Slot, OhgPanelistRow? Panelist, bool OnAir);

public sealed record OhgGalleryCellRow(int Cell, int Slot /* 0 = blank */, string? DisplayName);

/// <summary>Program/Preview are the WIRE strings ("black", "gallery", "activeSpeaker",
/// "look:&lt;id&gt;", "slot:&lt;n&gt;").</summary>
public sealed record OhgProgramView(string Program, string Preview, bool ActiveSpeakerFollow, string? ActiveSpeakerId);

public sealed record OhgQueueView(IReadOnlyList<string> Previous, string? Current, IReadOnlyList<string> Upcoming);

public sealed record OhgBoxView(int Box, int? Slot, string? DisplayName);

public sealed record OhgLookView(
    string LookId,
    string ScenePreset,
    int? HostSlot,
    int? ReaderSlot,
    IReadOnlyList<OhgBoxView> Boxes,
    int Page,
    int PageCount,
    string BoxFill);

public sealed record OhgOverlayView(
    string? QuestionText,
    string? QuestionAsker,
    string? HeadlineName,
    string? HeadlineLocation,
    bool HeadlineVisible);

public sealed record OhgCapabilityView(string State, string? Detail);

public sealed record OhgHealthView(string Panelists, string Hands, string Question, string Worst);

public sealed record OhgSnapshotView(
    long Revision,
    IReadOnlyList<OhgPanelistRow> Panelists,
    IReadOnlyList<OhgSlotRow> Slots,
    IReadOnlyList<OhgGalleryCellRow> Gallery,
    OhgQueueView Queue,
    OhgProgramView Program,
    OhgLookView? Look,
    IReadOnlyDictionary<int, int> ManualBoxes,
    IReadOnlyList<int> OnAirSlots,
    OhgOverlayView Overlays,
    OhgCapabilityView Registry,
    OhgCapabilityView HandsQueue,
    OhgCapabilityView QuestionFeed,
    OhgHealthView Health,
    bool SmartGallery,
    IReadOnlyList<OhgPanelistRow> Unseated,
    string? PagingRefused,
    IReadOnlyList<string> RestoreWarnings);

/// <summary>A configured look for the picker (Task 5) — built from config, not from the snapshot.</summary>
public sealed record OhgLookOption(string Id, string Label);
