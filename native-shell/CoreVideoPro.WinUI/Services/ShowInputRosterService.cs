using System.Collections;
using CoreVideoPro.MediaCore.Models;
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
        new() { Value = ShowInputKind.Screen, Label = "Screen" },
        new() { Value = ShowInputKind.SrtIngest, Label = "SRT ingest" },
        new() { Value = ShowInputKind.Media, Label = "Media asset" },
        new() { Value = ShowInputKind.Browser, Label = "Browser source" }
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

    /// <summary>Canonical source-id prefixes — the same scheme fed to the core as
    /// multiview source ids and used as the key for per-source display-name overrides.</summary>
    public const string ZoomSourcePrefix = "zoom:";

    public const string CaptureSourcePrefix = "capture:";

    public static string ZoomSourceId(string participantId) => $"{ZoomSourcePrefix}{participantId}";

    public static string CaptureSourceId(string deviceId) => $"{CaptureSourcePrefix}{deviceId}";

    /// <summary>Swaps the full source assignment (kind, source ids, audio device, in-show flag)
    /// between two slots — the multiviewer drag-drop reorder. Slot NUMBERS stay fixed; the
    /// CONTENT moves, so the wall order (slot order) changes. Kind is written first because its
    /// setter clears the id fields that don't apply to the new kind; the ids follow.</summary>
    public static void SwapSlotAssignments(ShowInputSlot a, ShowInputSlot b)
    {
        if (ReferenceEquals(a, b))
        {
            return;
        }

        using var _ = ShowInputWriteScope.Enter("operator-swap");

        var (aKind, aParticipant, aCapture, aAudio, aInShow) =
            (a.Kind, a.ParticipantId, a.CaptureDeviceId, a.AudioDeviceId, a.InShow);
        var (bKind, bParticipant, bCapture, bAudio, bInShow) =
            (b.Kind, b.ParticipantId, b.CaptureDeviceId, b.AudioDeviceId, b.InShow);

        a.Kind = bKind;
        a.ParticipantId = bParticipant;
        a.CaptureDeviceId = bCapture;
        a.AudioDeviceId = bAudio;
        a.InShow = bInShow;

        b.Kind = aKind;
        b.ParticipantId = aParticipant;
        b.CaptureDeviceId = aCapture;
        b.AudioDeviceId = aAudio;
        b.InShow = aInShow;
    }

    /// <summary>Non-assignable informational rows in the unified source picker (empty-group
    /// hints). The slot editor ignores any selection whose value carries this prefix.</summary>
    public const string HintSourcePrefix = "hint:";

    public static bool IsHintSourceId(string? sourceId) =>
        sourceId is { Length: > 0 } && sourceId.StartsWith(HintSourcePrefix, StringComparison.Ordinal);

    /// <summary>Infers the slot kind for a capture device — the operator picks a SOURCE and
    /// the kind follows (SRC-1); nobody should have to know "UVC webcam" vs "Screen" up front.
    /// Mirrors the per-kind filters in <see cref="BuildSourceOptions"/>.</summary>
    public static ShowInputKind InferCaptureDeviceKind(CaptureDevice device)
    {
        if (device.Id.StartsWith("screen:", StringComparison.Ordinal))
        {
            return ShowInputKind.Screen;
        }

        if (device.Id.StartsWith("browser:", StringComparison.Ordinal))
        {
            return ShowInputKind.Browser;
        }

        return device.Vendor.ToLowerInvariant() switch
        {
            "blackmagic" => ShowInputKind.Blackmagic,
            "aja" => ShowInputKind.Aja,
            "srt" => ShowInputKind.SrtIngest,
            _ => ShowInputKind.UvcWebcam
        };
    }

    /// <summary>
    /// SRC-1 (sources-redesign-spec §A2): ONE flat, grouped source list for a slot — the
    /// operator picks a source, never a type. Values are the canonical ids
    /// (zoom:/capture:/media:), labels carry the group ("Camera · Elgato…"). Empty Zoom/Media
    /// groups get a non-selectable hint row instead of silently vanishing. When
    /// <paramref name="currentSourceId"/> is assigned but no longer present, a saved-source
    /// entry is prepended so the slot KEEPS its binding instead of silently re-pointing at
    /// another source.
    /// </summary>
    public static IReadOnlyList<ShowInputSourceOption> BuildUnifiedSourceOptions(
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<MediaAsset> mediaAssets,
        string? currentSourceId = null,
        string? currentSourceLabel = null)
    {
        var options = new List<ShowInputSourceOption>();

        if (participants.Count == 0)
        {
            options.Add(new ShowInputSourceOption
            {
                Value = HintSourcePrefix + "zoom",
                Label = "No Zoom guests — join a meeting",
                Group = "Zoom"
            });
        }
        else
        {
            options.AddRange(participants.Select(participant => new ShowInputSourceOption
            {
                Value = ZoomSourceId(participant.Id),
                Label = $"{participant.Name} — {participant.RoleLabel}",
                Group = "Zoom"
            }));
        }

        foreach (var device in captureDevices)
        {
            var group = InferCaptureDeviceKind(device) switch
            {
                ShowInputKind.Screen => "Screen",
                ShowInputKind.SrtIngest => "SRT",
                ShowInputKind.Browser => "Browser",
                _ => "Camera"
            };
            options.Add(new ShowInputSourceOption
            {
                Value = CaptureSourceId(device.Id),
                Label = device.Name,
                Group = group
            });
        }

        if (mediaAssets.Count == 0)
        {
            options.Add(new ShowInputSourceOption
            {
                Value = HintSourcePrefix + "media",
                Label = "No media assets — add them on the Media tab",
                Group = "Media"
            });
        }
        else
        {
            options.AddRange(mediaAssets.Select(asset => new ShowInputSourceOption
            {
                Value = ToMediaSourceId(asset.Id),
                Label = string.IsNullOrWhiteSpace(asset.Kind) ? asset.Name : $"{asset.Name} ({asset.Kind})",
                Group = "Media"
            }));
        }

        // Never silently substitute: an assigned-but-gone source stays available as a saved binding.
        if (currentSourceId is { Length: > 0 } &&
            !options.Any(option => string.Equals(option.Value, currentSourceId, StringComparison.Ordinal)))
        {
            options.Insert(0, new ShowInputSourceOption
            {
                Value = currentSourceId,
                Label = $"Saved source unavailable — {UnavailableSourceDescription(currentSourceId, currentSourceLabel)}"
            });
        }

        return options;
    }

    private static string UnavailableSourceDescription(string sourceId, string? sourceLabel)
    {
        if (!string.IsNullOrWhiteSpace(sourceLabel))
        {
            return sourceLabel;
        }

        if (sourceId.StartsWith("capture:window:", StringComparison.OrdinalIgnoreCase))
        {
            return "previous window capture";
        }

        if (sourceId.StartsWith("capture:screen:", StringComparison.OrdinalIgnoreCase))
        {
            return "previous screen capture";
        }

        if (sourceId.StartsWith("capture:", StringComparison.OrdinalIgnoreCase))
        {
            return "previous capture source";
        }

        if (sourceId.StartsWith("zoom:", StringComparison.OrdinalIgnoreCase))
        {
            return "previous Zoom guest";
        }

        if (sourceId.StartsWith("media:", StringComparison.OrdinalIgnoreCase))
        {
            return "previous media asset";
        }

        return "previous source";
    }

    /// <summary>Fixed submenu order for the source-picker menu.</summary>
    public static readonly IReadOnlyList<string> UnifiedSourceGroups = ["Zoom", "Camera", "Screen", "Browser", "Media", "SRT"];

    /// <summary>The label shown on a slot's picker button: "Camera · Elgato HD60" for a bound
    /// source (group carries the type, so no separate type chip is needed), the bare label for
    /// group-less saved-source entries, or null when unbound (the button shows its placeholder).</summary>
    public static string? UnifiedSourceDisplayLabel(
        IReadOnlyList<ShowInputSourceOption> options, string? sourceId)
    {
        if (sourceId is not { Length: > 0 })
        {
            return null;
        }

        var option = options.FirstOrDefault(item => string.Equals(item.Value, sourceId, StringComparison.Ordinal));
        if (option is null)
        {
            return sourceId;
        }

        return string.IsNullOrEmpty(option.Group) ? option.Label : $"{option.Group} · {option.Label}";
    }

    /// <summary>The canonical source id an assigned slot resolves to (zoom:/capture:/media:),
    /// or null when the slot is unassigned/unresolvable. This is the key for display-name overrides.</summary>
    public static string? SlotSourceId(ShowInputSlot slot) => slot.Kind switch
    {
        ShowInputKind.ZoomParticipant when slot.ParticipantId is { Length: > 0 } pid => ZoomSourceId(pid),
        // A Media slot stores its "media:<assetId>" id directly in ParticipantId.
        ShowInputKind.Media when slot.ParticipantId is { Length: > 0 } media => media,
        ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser
            when slot.CaptureDeviceId is { Length: > 0 } cap => CaptureSourceId(cap),
        _ => null
    };

    /// <summary>Returns the operator override for <paramref name="sourceId"/> when present and
    /// non-blank, otherwise the derived Zoom/UVC/asset name. Single source of truth so the auto
    /// lower-thirds, multiview labels, and the Inputs editor all agree.</summary>
    public static string ResolveDisplayName(
        IReadOnlyDictionary<string, string>? overrides,
        string? sourceId,
        string derivedName)
    {
        if (overrides is not null && sourceId is { Length: > 0 } &&
            overrides.TryGetValue(sourceId, out var name) && !string.IsNullOrWhiteSpace(name))
        {
            return name.Trim();
        }

        return derivedName;
    }

    /// <summary>Reconciles the Zoom-participant Show Input slots against the live roster:
    /// frees slots whose participant left, and — when <paramref name="autoAssign"/> is on — fills
    /// FREE (Unassigned) slots with roster participants not yet shown, in roster order, up to the
    /// slot cap. Never disturbs operator- or capture/media-assigned slots, and keeps each already
    /// shown participant in its current slot (no reshuffle when others leave).</summary>
    public static void SyncZoomParticipantSlots(
        IList<ShowInputSlot> slots,
        IReadOnlyList<string> participantIdsInRosterOrder,
        bool autoAssign,
        IReadOnlyList<string>? autoAssignCandidates = null)
    {
        using var _ = ShowInputWriteScope.Enter("roster-sync");

        var validIds = new HashSet<string>(participantIdsInRosterOrder, StringComparer.Ordinal);

        // Free slots whose Zoom participant is no longer in the meeting.
        foreach (var slot in slots)
        {
            if (slot.Kind == ShowInputKind.ZoomParticipant &&
                !string.IsNullOrWhiteSpace(slot.ParticipantId) &&
                !validIds.Contains(slot.ParticipantId))
            {
                slot.Kind = ShowInputKind.Unassigned;
                slot.ParticipantId = null;
                slot.InShow = false;
            }
        }

        if (!autoAssign)
        {
            return;
        }

        var alreadyShown = new HashSet<string>(
            slots.Where(s => s.Kind == ShowInputKind.ZoomParticipant && !string.IsNullOrWhiteSpace(s.ParticipantId))
                 .Select(s => s.ParticipantId!),
            StringComparer.Ordinal);

        // Fill from CANDIDATES (newly-joined participants), not the whole roster.
        // Filling every not-currently-assigned id re-added participants the
        // operator had deliberately unassigned or replaced, on the NEXT sync tick
        // (~1/s) — the Sources screen "keeps reverting after I change it
        // manually" bug. The coordinator passes only ids it has never seen in
        // this meeting; null preserves the old fill-everyone behavior for
        // callers that mean it (the operator flipping the auto-assign toggle).
        foreach (var participantId in autoAssignCandidates ?? participantIdsInRosterOrder)
        {
            if (string.IsNullOrWhiteSpace(participantId) || alreadyShown.Contains(participantId))
            {
                continue;  // already in a slot — keep it stable
            }

            var freeSlot = slots.FirstOrDefault(s => s.Kind == ShowInputKind.Unassigned);
            if (freeSlot is null)
            {
                break;  // every slot is taken
            }

            freeSlot.Kind = ShowInputKind.ZoomParticipant;
            freeSlot.ParticipantId = participantId;
            freeSlot.InShow = true;
            alreadyShown.Add(participantId);
        }
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
            ShowInputKind.Screen => captureDevices
                .Where(device => device.Id.StartsWith("screen:", StringComparison.Ordinal))
                .Select(device => new ShowInputSourceOption { Value = device.Id, Label = device.Name })
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

        if (slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser)
        {
            route.Mode = SourceRouteMode.CaptureDevice;
            route.ParticipantId = null;
            route.CaptureDeviceId = slot.CaptureDeviceId;
        }
    }

    /// <summary>
    /// Keeps the default Speaker + Slides scene useful before a Zoom meeting is joined.
    /// Role routes remain untouched whenever Zoom video participants exist, so normal
    /// active-speaker and screen-share switching resumes as soon as a meeting is present.
    /// The supplied route is expected to be a transient clone; saved scene semantics are
    /// never rewritten by this fallback.
    /// </summary>
    public static bool TryApplyStandaloneRoleFallback(
        SourceRoute route,
        IReadOnlyList<ShowInputSlot> slots,
        IReadOnlyList<Participant> roomVideoParticipants)
    {
        if (roomVideoParticipants.Count > 0 ||
            route.Mode is not (SourceRouteMode.ActiveSpeaker or SourceRouteMode.ScreenShare))
        {
            return false;
        }

        var candidates = slots
            .Where(slot => slot.InShow && slot.IsAssigned)
            .OrderBy(slot => slot.SlotNumber);

        var fallback = route.Mode == SourceRouteMode.ScreenShare
            ? candidates.FirstOrDefault(slot => slot.Kind == ShowInputKind.Screen)
            : candidates
                .OrderBy(slot => slot.Kind switch
                {
                    ShowInputKind.UvcWebcam => 0,
                    ShowInputKind.Blackmagic => 1,
                    ShowInputKind.Aja => 2,
                    ShowInputKind.SrtIngest => 3,
                    _ => 4
                })
                .FirstOrDefault(slot => slot.Kind is
                    ShowInputKind.UvcWebcam or
                    ShowInputKind.Blackmagic or
                    ShowInputKind.Aja or
                    ShowInputKind.SrtIngest);

        if (fallback is null)
        {
            return false;
        }

        route.ShowInputSlotNumber = fallback.SlotNumber;
        ApplySlotRoute(route, fallback);
        return true;
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
                    slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser &&
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

    /// <summary>
    /// Builds the ordered <c>set-multiview-layout</c> source list from the SAME Show Input slots
    /// that feed <see cref="BuildMultiviewTiles"/> (InShow + assigned + resolved, capped at
    /// <see cref="MaxMultiviewBoxes"/>). The core composites these into one GPU shared texture.
    /// </summary>
    private static string _lastMvResolveDiagSig = "";

    public static IReadOnlyList<MediaCoreMultiviewSourceWire> BuildMultiviewLayoutSources(
        IReadOnlyList<ShowInputSlot> slots,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<MediaAsset>? mediaAssets = null,
        IReadOnlyDictionary<string, string>? displayNameOverrides = null)
    {
        var devicesById = captureDevices.ToDictionary(device => device.Id, device => device);
        var participantsById = participants.ToDictionary(participant => participant.Id, participant => participant);
        var mediaAssetsById = (mediaAssets ?? [])
            .ToDictionary(asset => asset.Id, asset => asset, StringComparer.Ordinal);

        // DIAGNOSTIC (change-gated): why do in-show slots resolve (or not) into multiview sources?
        var inShowAssigned = slots.Where(s => s.InShow && s.IsAssigned).ToList();
        var diagSig = string.Join("|", inShowAssigned.Select(s => $"{s.SlotNumber}:{s.Kind}:{s.CaptureDeviceId}:{s.ParticipantId}")) +
            "#dev:" + string.Join(",", devicesById.Keys) + "#par:" + string.Join(",", participantsById.Keys);
        if (diagSig != _lastMvResolveDiagSig)
        {
            _lastMvResolveDiagSig = diagSig;
            foreach (var s in inShowAssigned)
            {
                LaunchLog.Write($"mv-resolve: slot{s.SlotNumber} kind={s.Kind} capId='{s.CaptureDeviceId}' pid='{s.ParticipantId}' resolved={HasResolvedSource(s, participantsById, devicesById, mediaAssetsById)}");
            }
            LaunchLog.Write($"mv-resolve: inShowAssigned={inShowAssigned.Count} captureDevices=[{string.Join(",", devicesById.Keys)}] participants=[{string.Join(",", participantsById.Keys.Take(8))}]");
        }

        return slots
            .Where(slot => slot.InShow && slot.IsAssigned &&
                HasResolvedSource(slot, participantsById, devicesById, mediaAssetsById))
            .Take(MaxMultiviewBoxes)
            .Select(slot => ToMultiviewSourceWire(slot, participantsById, devicesById, mediaAssetsById, displayNameOverrides))
            .Where(source => source is not null)
            .Select(source => source!)
            .ToList();
    }

    private static MediaCoreMultiviewSourceWire? ToMultiviewSourceWire(
        ShowInputSlot slot,
        IReadOnlyDictionary<string, Participant> participantsById,
        IReadOnlyDictionary<string, CaptureDevice> devicesById,
        IReadOnlyDictionary<string, MediaAsset> mediaAssetsById,
        IReadOnlyDictionary<string, string>? displayNameOverrides = null)
    {
        if (slot.Kind == ShowInputKind.ZoomParticipant &&
            slot.ParticipantId is { Length: > 0 } pid &&
            participantsById.TryGetValue(pid, out var participant))
        {
            var sourceId = ZoomSourceId(pid);
            return new MediaCoreMultiviewSourceWire(
                SourceId: sourceId,
                Kind: "zoom",
                Slot: slot.SlotNumber - 1,
                Label: ResolveDisplayName(displayNameOverrides, sourceId, participant.Name),
                ParticipantId: pid);
        }

        if (slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser &&
            slot.CaptureDeviceId is { Length: > 0 } deviceId &&
            devicesById.TryGetValue(deviceId, out var device))
        {
            var sourceId = CaptureSourceId(deviceId);
            return new MediaCoreMultiviewSourceWire(
                SourceId: sourceId,
                Kind: "capture",
                Slot: slot.SlotNumber - 1,
                Label: ResolveDisplayName(displayNameOverrides, sourceId, device.Name),
                CaptureDeviceId: deviceId);
        }

        if (slot.Kind == ShowInputKind.Media &&
            TryGetMediaAssetId(slot.ParticipantId, out var mediaAssetId) &&
            mediaAssetsById.TryGetValue(mediaAssetId, out var asset))
        {
            var sourceId = ToMediaSourceId(mediaAssetId);
            return new MediaCoreMultiviewSourceWire(
                SourceId: sourceId,
                Kind: "media",
                Slot: slot.SlotNumber - 1,
                Label: ResolveDisplayName(displayNameOverrides, sourceId, asset.Name),
                MediaAssetId: mediaAssetId);
        }

        return null;
    }

    /// <summary>
    /// Aspect-aware-ish grid shape for the advisory cols/rows in set-multiview-layout. The core
    /// computes the authoritative cells itself, so this only needs to be a sane count→shape map.
    /// </summary>
    public static (int Columns, int Rows) ResolveGridShape(int tileCount) =>
        tileCount switch
        {
            <= 1 => (1, 1),
            2 => (2, 1),
            3 => (3, 1),
            <= 4 => (2, 2),
            <= 6 => (3, 2),
            <= 8 => (4, 2),
            <= 10 => (5, 2),
            <= 12 => (4, 3),
            _ => (4, 4)
        };

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

    /// <summary>
    /// In-show slots whose assigned source no longer exists, described for the operator.
    /// </summary>
    /// <remarks>
    /// An unresolvable assignment is silently dropped from the multiview source list, so
    /// the operator gets a placeholder tile and no explanation — on the owner rig, slot 3
    /// pointed at "screen:3" after the display count changed, and it rendered a placeholder
    /// every tick from 2026-07-31 onward with nothing said. The core already warns about the
    /// matching compositor layer, but nobody reads media-core.log mid-show.
    ///
    /// Zoom participants are deliberately EXCLUDED: a guest leaving is normal show traffic,
    /// not a misconfiguration. This mirrors the compositor's own guardrail, which warns for
    /// capture:/media: layers and stays quiet for Zoom.
    /// </remarks>
    public static IReadOnlyList<string> DescribeUnresolvedAssignments(
        IReadOnlyList<ShowInputSlot> slots,
        IReadOnlyList<Participant> participants,
        IReadOnlyList<CaptureDevice> captureDevices,
        IReadOnlyList<MediaAsset>? mediaAssets = null)
    {
        var devicesById = captureDevices.ToDictionary(device => device.Id, device => device);
        var participantsById = participants.ToDictionary(participant => participant.Id, participant => participant);
        var mediaAssetsById = (mediaAssets ?? [])
            .ToDictionary(asset => asset.Id, asset => asset, StringComparer.Ordinal);

        var unresolved = new List<string>();
        foreach (var slot in slots)
        {
            if (!slot.InShow || !slot.IsAssigned || slot.Kind == ShowInputKind.ZoomParticipant)
            {
                continue;
            }
            if (HasResolvedSource(slot, participantsById, devicesById, mediaAssetsById))
            {
                continue;
            }
            var missing = slot.Kind == ShowInputKind.Media
                ? slot.ParticipantId
                : slot.CaptureDeviceId;
            unresolved.Add(
                $"Input {slot.SlotNumber} ({slot.Kind}) is assigned to '{missing}', which is no longer available. " +
                $"Reassign it or take the input out of the show.");
        }
        return unresolved;
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
            ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest or ShowInputKind.Browser =>
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
                            ? "Source connected · waiting for video."
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
                DetailLine = "Assign this source to a Show Input to keep it in the multiview."
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
