using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Mirrors React App.tsx scene slot / route helpers for preview editing.
/// </summary>
public static class SceneRoutingService
{
    public static IReadOnlyList<RouteSelectOption> RouteModeOptions { get; } =
    [
        new() { Value = "fixed", Label = "Fixed participant" },
        new() { Value = "capture-input", Label = "Capture input" },
        new() { Value = "active-speaker", Label = "Active speaker" },
        new() { Value = "spotlight", Label = "Spotlight" },
        new() { Value = "screen-share", Label = "Screen share" },
        new() { Value = "none", Label = "None" }
    ];

    public static IReadOnlyList<RouteSelectOption> AudioRoleOptions { get; } =
    [
        new() { Value = "mix", Label = "Mix" },
        new() { Value = "isolated", Label = "Isolated" },
        new() { Value = "audience", Label = "Audience" }
    ];

    /// <summary>
    /// "Add source" picker for the scene canvas (§4 IA): Inputs 1-10 plus the auto roles
    /// (Active Speaker / Screen Share / Media). The <c>input-NN</c> values mirror the per-layer
    /// source picker so an added layer references an Input directly.
    /// </summary>
    public static IReadOnlyList<RouteSelectOption> AddSourceOptions { get; } = BuildAddSourceOptions();

    private static IReadOnlyList<RouteSelectOption> BuildAddSourceOptions()
    {
        var options = new List<RouteSelectOption>();
        for (var slot = 1; slot <= ShowInputRosterService.MaxShowInputs; slot++)
        {
            options.Add(new RouteSelectOption
            {
                Value = $"input-{slot:00}",
                Label = $"Input {slot}"
            });
        }

        options.Add(new RouteSelectOption { Value = "active-speaker", Label = "Active Speaker" });
        options.Add(new RouteSelectOption { Value = "screen-share", Label = "Screen Share" });
        options.Add(new RouteSelectOption { Value = "media", Label = "Media" });
        // R1: role-targeted layers — resolve to whoever holds the role at sync
        // time, so "Host + Reader" scenes survive any roster.
        foreach (var role in ProductionRoleService.Roles)
        {
            options.Add(new RouteSelectOption
            {
                Value = ProductionRoleService.ToOptionValue(role.Value),
                Label = $"Role: {role.Label}"
            });
        }

        return options;
    }

    public static string ModeToWire(SourceRouteMode mode) => mode switch
    {
        SourceRouteMode.Fixed => "fixed",
        SourceRouteMode.CaptureDevice => "capture-input",
        SourceRouteMode.ActiveSpeaker => "active-speaker",
        SourceRouteMode.Spotlight => "spotlight",
        SourceRouteMode.ScreenShare => "screen-share",
        SourceRouteMode.None => "none",
        _ => "active-speaker"
    };

    public static SourceRouteMode ModeFromWire(string wire) => wire switch
    {
        "fixed" => SourceRouteMode.Fixed,
        "capture-input" => SourceRouteMode.CaptureDevice,
        "active-speaker" => SourceRouteMode.ActiveSpeaker,
        "spotlight" => SourceRouteMode.Spotlight,
        "screen-share" => SourceRouteMode.ScreenShare,
        "none" => SourceRouteMode.None,
        _ => SourceRouteMode.ActiveSpeaker
    };

    public static string AudioRoleToWire(SourceAudioRole role) => role switch
    {
        SourceAudioRole.Mix => "mix",
        SourceAudioRole.Isolated => "isolated",
        SourceAudioRole.Audience => "audience",
        _ => "mix"
    };

    public static SourceAudioRole AudioRoleFromWire(string wire) => wire switch
    {
        "mix" => SourceAudioRole.Mix,
        "isolated" => SourceAudioRole.Isolated,
        "audience" => SourceAudioRole.Audience,
        _ => SourceAudioRole.Mix
    };

    public static SourceRoute BuildAddedSourceRoute(
        string sceneId,
        int index,
        string optionValue,
        string? selectedMediaAssetId = null)
    {
        var route = new SourceRoute
        {
            Id = $"{sceneId}-add-{Guid.NewGuid():N}"[..(sceneId.Length + 12)],
            Mode = SourceRouteMode.None,
            AudioRole = SourceAudioRole.Mix,
            CanvasRect = new NormalizedCanvasRect { X = 0, Y = 0, Width = 1, Height = 1 },
            FitMode = SourceRouteVisualDefaults.FitMode,
            SourceScale = SourceRouteVisualDefaults.SourceScale,
            SourceOffsetX = SourceRouteVisualDefaults.SourceOffsetX,
            SourceOffsetY = SourceRouteVisualDefaults.SourceOffsetY,
            SourceFramingModified = false,
            ZIndex = index
        };

        if (optionValue.StartsWith("input-", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(optionValue.AsSpan(6), out var slotNumber))
        {
            route.Mode = SourceRouteMode.Fixed;
            route.ShowInputSlotNumber = slotNumber;
        }
        else if (optionValue.StartsWith("capture:", StringComparison.OrdinalIgnoreCase) &&
                 optionValue.Length > "capture:".Length)
        {
            route.Mode = SourceRouteMode.CaptureDevice;
            route.CaptureDeviceId = optionValue["capture:".Length..];
        }
        else if (ProductionRoleService.RoleIdFromOption(optionValue) is { Length: > 0 } roleId)
        {
            // R1: fixed-mode route whose participant is resolved from the role
            // holder at sync time (unassigned role -> placeholder slate).
            route.Mode = SourceRouteMode.Fixed;
            route.ProductionRoleId = roleId;
        }
        else if (string.Equals(optionValue, "media", StringComparison.OrdinalIgnoreCase) &&
                 !string.IsNullOrWhiteSpace(selectedMediaAssetId))
        {
            route.Mode = SourceRouteMode.Fixed;
            route.ParticipantId = ShowInputRosterService.ToMediaSourceId(selectedMediaAssetId.Trim());
        }
        else
        {
            route.Mode = optionValue switch
            {
                "active-speaker" => SourceRouteMode.ActiveSpeaker,
                "screen-share" => SourceRouteMode.ScreenShare,
                _ => SourceRouteMode.None
            };
        }

        return route;
    }

    public static void ApplyInputSlotTemplate(IList<SourceRoute> routes, string sceneId, int slotCount)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slotCount);

        while (routes.Count > slotCount)
        {
            routes.RemoveAt(routes.Count - 1);
        }

        while (routes.Count < slotCount)
        {
            routes.Add(BuildAddedSourceRoute(sceneId, routes.Count, $"input-{routes.Count + 1}"));
        }

        for (var index = 0; index < routes.Count; index++)
        {
            routes[index].Mode = SourceRouteMode.Fixed;
            routes[index].ParticipantId = null;
            routes[index].CaptureDeviceId = null;
            routes[index].ShowInputSlotNumber = index + 1;
            routes[index].SpotlightIndex = null;
            routes[index].AudioRole = SourceAudioRole.Mix;
            routes[index].FitMode = SourceRouteVisualDefaults.FitMode;
            routes[index].SourceScale = SourceRouteVisualDefaults.SourceScale;
            routes[index].SourceOffsetX = SourceRouteVisualDefaults.SourceOffsetX;
            routes[index].SourceOffsetY = SourceRouteVisualDefaults.SourceOffsetY;
            routes[index].SourceFramingModified = false;
            routes[index].ZIndex = index;
        }
    }

    public static IReadOnlyList<SourceRoute> GetRouteDefaults(
        Scene scene,
        IReadOnlyList<SourceRoute>? existingRoutes,
        IReadOnlyList<Participant> participants)
    {
        // Honour any operator-added sources beyond the layout's default slot count: the
        // "Add source" affordance appends routes to the working set, and reconciliation must
        // not truncate them back to the layout capacity.
        var slotCount = Math.Max(GetRouteSlotCount(scene, participants), existingRoutes?.Count ?? 0);
        var slotDefaults = scene.Layout == "speaker-slides"
            ? GetSlotDefaults(scene, existingRoutes, participants).Concat(["screen-share"]).ToList()
            : GetSlotDefaults(scene, existingRoutes, participants);

        return Enumerable.Range(0, slotCount)
            .Select(index =>
            {
                var existing = existingRoutes?.ElementAtOrDefault(index);
                var fallback = RouteFromSlot(scene.Id, index, slotDefaults.ElementAtOrDefault(index));
                return NormalizeRouteUpdate(existing ?? fallback, participants);
            })
            .ToList();
    }

    public static IReadOnlyList<Participant> GetSceneParticipants(
        Scene scene,
        IReadOnlyList<SourceRoute> routes,
        IReadOnlyList<Participant> participants)
    {
        var sorted = SortParticipantsForProduction(participants);
        var routeParticipants = routes
            .Select(route => ResolveRouteParticipantInternal(route, sorted))
            .Where(participant => participant is not null)
            .Cast<Participant>()
            .ToList();

        var assigned = DedupeParticipants(routeParticipants);
        var fallback = sorted
            .Where(participant => assigned.All(assignedParticipant => assignedParticipant.Id != participant.Id))
            .ToList();

        return assigned
            .Concat(fallback)
            .Take(GetSceneSlotCount(scene, participants))
            .ToList();
    }

    public static string DescribeRouteAssignments(
        Scene scene,
        IReadOnlyList<SourceRoute> routes,
        IReadOnlyList<Participant> participants)
    {
        var labels = routes
            .Select(route => DescribeRoute(route, participants))
            .Where(label => !string.IsNullOrWhiteSpace(label))
            .ToList();

        return labels.Count == 0 ? "No participants assigned" : string.Join(" + ", labels);
    }

    public static IReadOnlyList<string> GetRouteWarnings(
        Scene scene,
        IReadOnlyList<SourceRoute> routes,
        IReadOnlyList<Participant> participants)
    {
        var warnings = new List<string>();
        var fixedParticipantIds = routes
            .Where(route =>
                (route.Mode is SourceRouteMode.Fixed or SourceRouteMode.Spotlight) &&
                route.ParticipantId is not null)
            .Select(route => route.ParticipantId!)
            .ToList();

        var isolatedParticipantIds = routes
            .Where(route => route.AudioRole == SourceAudioRole.Isolated && route.ParticipantId is not null)
            .Select(route => route.ParticipantId!)
            .ToList();

        foreach (var participantId in GetDuplicateIds(fixedParticipantIds))
        {
            var participant = participants.FirstOrDefault(item => item.Id == participantId);
            warnings.Add($"{participant?.Name ?? participantId} is assigned to multiple fixed routes.");
        }

        foreach (var participantId in GetDuplicateIds(isolatedParticipantIds))
        {
            var participant = participants.FirstOrDefault(item => item.Id == participantId);
            warnings.Add($"{participant?.Name ?? participantId} has duplicated isolated audio.");
        }

        for (var index = 0; index < routes.Count; index++)
        {
            var route = routes[index];
            var participant = route.ParticipantId is not null
                ? participants.FirstOrDefault(item => item.Id == route.ParticipantId)
                : null;

            if (route.Mode == SourceRouteMode.Fixed && participant is null)
            {
                warnings.Add($"Slot {index + 1} fixed participant is unavailable.");
            }

            if (route.Mode == SourceRouteMode.ActiveSpeaker &&
                !participants.Any(item => item.IsActiveSpeaker && item.Health != FeedHealth.VideoOff))
            {
                warnings.Add($"Slot {index + 1} active speaker is unavailable.");
            }

            if (route.Mode == SourceRouteMode.ScreenShare &&
                !participants.Any(item => item.IsScreenSharing))
            {
                warnings.Add($"Slot {index + 1} screen share is unavailable.");
            }

            if (route.Mode == SourceRouteMode.Spotlight && participant is null)
            {
                warnings.Add($"Slot {index + 1} spotlight source is unavailable.");
            }

            if (route.Mode == SourceRouteMode.None)
            {
                warnings.Add($"Slot {index + 1} is parked.");
            }

            if (participant?.Health == FeedHealth.LowResolution)
            {
                warnings.Add($"{participant.Name} is below target resolution.");
            }

            if (participant?.Health == FeedHealth.Recovering)
            {
                warnings.Add($"{participant.Name} feed is recovering.");
            }

            if (participant?.Health == FeedHealth.VideoOff)
            {
                warnings.Add($"{participant.Name} video is off.");
            }
        }

        return warnings.Distinct().ToList();
    }

    public static SourceRoute NormalizeRouteUpdate(SourceRoute route, IReadOnlyList<Participant> participants)
    {
        SourceRoute normalized;
        if (route.Mode == SourceRouteMode.Fixed)
        {
            normalized = new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = route.ParticipantId ?? participants.FirstOrDefault()?.Id,
                CaptureDeviceId = null,
                ShowInputSlotNumber = route.ShowInputSlotNumber,
                SpotlightIndex = route.SpotlightIndex,
                AudioRole = route.AudioRole == SourceAudioRole.Audience
                    ? SourceAudioRole.Isolated
                    : route.AudioRole
            };
        }
        else if (route.Mode == SourceRouteMode.CaptureDevice)
        {
            normalized = new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = null,
                CaptureDeviceId = route.CaptureDeviceId,
                ShowInputSlotNumber = route.ShowInputSlotNumber,
                SpotlightIndex = route.SpotlightIndex,
                AudioRole = route.AudioRole == SourceAudioRole.Audience
                    ? SourceAudioRole.Isolated
                    : route.AudioRole
            };
        }
        else if (route.Mode == SourceRouteMode.Spotlight)
        {
            var spotlightIndex = route.SpotlightIndex ?? 0;
            normalized = new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = route.ParticipantId ?? participants.ElementAtOrDefault(spotlightIndex)?.Id,
                CaptureDeviceId = null,
                ShowInputSlotNumber = route.ShowInputSlotNumber,
                SpotlightIndex = spotlightIndex,
                AudioRole = route.AudioRole == SourceAudioRole.Audience
                    ? SourceAudioRole.Mix
                    : route.AudioRole
            };
        }
        else if (route.Mode is SourceRouteMode.ScreenShare or SourceRouteMode.None)
        {
            normalized = new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = null,
                CaptureDeviceId = null,
                ShowInputSlotNumber = route.ShowInputSlotNumber,
                SpotlightIndex = route.SpotlightIndex,
                AudioRole = SourceAudioRole.Audience
            };
        }
        else
        {
            normalized = new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = null,
                CaptureDeviceId = null,
                ShowInputSlotNumber = route.ShowInputSlotNumber,
                SpotlightIndex = route.SpotlightIndex,
                AudioRole = route.AudioRole == SourceAudioRole.Isolated
                    ? SourceAudioRole.Mix
                    : route.AudioRole
            };
        }

        normalized.CanvasRect = route.CanvasRect?.Clone();
        if (route.SourceFramingModified)
        {
            normalized.FitMode = NormalizeFitMode(route.FitMode);
            normalized.SourceScale = NormalizeSourceScale(route.SourceScale);
            normalized.SourceOffsetX = NormalizeSourceOffset(route.SourceOffsetX);
            normalized.SourceOffsetY = NormalizeSourceOffset(route.SourceOffsetY);
            normalized.SourceFramingModified = HasModifiedSourceFraming(
                normalized.FitMode,
                normalized.SourceScale,
                normalized.SourceOffsetX,
                normalized.SourceOffsetY);
        }
        else
        {
            normalized.FitMode = SourceRouteVisualDefaults.FitMode;
            normalized.SourceScale = SourceRouteVisualDefaults.SourceScale;
            normalized.SourceOffsetX = SourceRouteVisualDefaults.SourceOffsetX;
            normalized.SourceOffsetY = SourceRouteVisualDefaults.SourceOffsetY;
            normalized.SourceFramingModified = false;
        }

        normalized.BorderStyle = NormalizeBorderStyle(route.BorderStyle);
        normalized.BorderColor = NormalizeBorderColor(route.BorderColor);
        normalized.BorderThickness = Math.Clamp(route.BorderThickness, 0, 12);
        normalized.ColorGrade = route.ColorGrade;
        normalized.ZIndex = route.ZIndex;
        return normalized;
    }

    public static void ApplyNormalizeRouteUpdate(SourceRoute route, IReadOnlyList<Participant> participants)
    {
        var normalized = NormalizeRouteUpdate(route, participants);
        route.Mode = normalized.Mode;
        route.ParticipantId = normalized.ParticipantId;
        route.CaptureDeviceId = normalized.CaptureDeviceId;
        route.ShowInputSlotNumber = normalized.ShowInputSlotNumber;
        route.SpotlightIndex = normalized.SpotlightIndex;
        route.AudioRole = normalized.AudioRole;
        route.CanvasRect = normalized.CanvasRect?.Clone();
        route.FitMode = normalized.FitMode;
        route.BorderStyle = normalized.BorderStyle;
        route.BorderColor = normalized.BorderColor;
        route.BorderThickness = normalized.BorderThickness;
        route.SourceScale = normalized.SourceScale;
        route.SourceOffsetX = normalized.SourceOffsetX;
        route.SourceOffsetY = normalized.SourceOffsetY;
        route.SourceFramingModified = normalized.SourceFramingModified;
        route.ColorGrade = normalized.ColorGrade;
        route.ZIndex = normalized.ZIndex;
    }

    public static void ApplyRouteValues(SourceRoute target, SourceRoute source)
    {
        target.Mode = source.Mode;
        target.ParticipantId = source.ParticipantId;
        target.CaptureDeviceId = source.CaptureDeviceId;
        target.ShowInputSlotNumber = source.ShowInputSlotNumber;
        target.SpotlightIndex = source.SpotlightIndex;
        target.AudioRole = source.AudioRole;
        target.CanvasRect = source.CanvasRect?.Clone();
        if (source.SourceFramingModified)
        {
            target.FitMode = NormalizeFitMode(source.FitMode);
            target.SourceScale = NormalizeSourceScale(source.SourceScale);
            target.SourceOffsetX = NormalizeSourceOffset(source.SourceOffsetX);
            target.SourceOffsetY = NormalizeSourceOffset(source.SourceOffsetY);
            target.SourceFramingModified = HasModifiedSourceFraming(
                target.FitMode,
                target.SourceScale,
                target.SourceOffsetX,
                target.SourceOffsetY);
        }
        else
        {
            target.FitMode = SourceRouteVisualDefaults.FitMode;
            target.SourceScale = SourceRouteVisualDefaults.SourceScale;
            target.SourceOffsetX = SourceRouteVisualDefaults.SourceOffsetX;
            target.SourceOffsetY = SourceRouteVisualDefaults.SourceOffsetY;
            target.SourceFramingModified = false;
        }

        target.BorderStyle = NormalizeBorderStyle(source.BorderStyle);
        target.BorderColor = NormalizeBorderColor(source.BorderColor);
        target.BorderThickness = Math.Clamp(source.BorderThickness, 0, 12);
        target.ColorGrade = source.ColorGrade;
        target.ZIndex = source.ZIndex;
    }

    public static string NormalizeFitMode(string? value) =>
        value switch
        {
            "fit" or "fill" or "stretch" => value,
            _ => SourceRouteVisualDefaults.FitMode
        };

    public static string NormalizeBorderStyle(string? value) =>
        value switch
        {
            "none" or "solid" or "accent" or "program" or "warning" => value,
            _ => SourceRouteVisualDefaults.BorderStyle
        };

    public static string NormalizeBorderColor(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return SourceRouteVisualDefaults.BorderColor;
        }

        var color = value.Trim();
        if (!color.StartsWith('#'))
        {
            color = "#" + color;
        }

        return color.Length is 7 or 9 ? color.ToUpperInvariant() : SourceRouteVisualDefaults.BorderColor;
    }

    public static double NormalizeSourceScale(double value) =>
        double.IsFinite(value) ? Math.Clamp(Math.Round(value, 2), 0.25, 4) : SourceRouteVisualDefaults.SourceScale;

    public static double NormalizeSourceOffset(double value) =>
        double.IsFinite(value) ? Math.Clamp(Math.Round(value, 2), -1, 1) : 0;

    public static bool HasModifiedSourceFraming(
        string? fitMode,
        double sourceScale,
        double sourceOffsetX,
        double sourceOffsetY) =>
        NormalizeFitMode(fitMode) != SourceRouteVisualDefaults.FitMode ||
        Math.Abs(NormalizeSourceScale(sourceScale) - SourceRouteVisualDefaults.SourceScale) > 0.001 ||
        Math.Abs(NormalizeSourceOffset(sourceOffsetX) - SourceRouteVisualDefaults.SourceOffsetX) > 0.001 ||
        Math.Abs(NormalizeSourceOffset(sourceOffsetY) - SourceRouteVisualDefaults.SourceOffsetY) > 0.001;

    public static string? RouteToSlot(SourceRoute route)
    {
        if (route.Mode is SourceRouteMode.Fixed or SourceRouteMode.Spotlight)
        {
            return route.ParticipantId;
        }

        if (route.Mode == SourceRouteMode.CaptureDevice)
        {
            return route.CaptureDeviceId is { Length: > 0 } deviceId
                ? $"capture:{deviceId}"
                : null;
        }

        return route.Mode == SourceRouteMode.ScreenShare ? "screen-share" : null;
    }

    private static string? DescribeRoute(SourceRoute route, IReadOnlyList<Participant> participants) =>
        route.Mode switch
        {
            SourceRouteMode.ScreenShare => "screen share",
            SourceRouteMode.CaptureDevice => route.CaptureDeviceId is { Length: > 0 } deviceId
                ? $"capture {deviceId}"
                : "capture input",
            SourceRouteMode.ActiveSpeaker => ResolveRouteParticipant(route, participants)?.Name ?? "active speaker",
            SourceRouteMode.None => null,
            _ => ResolveRouteParticipant(route, participants)?.Name
        };

    public static Participant? ResolveRouteParticipant(SourceRoute route, IReadOnlyList<Participant> participants) =>
        ResolveRouteParticipantInternal(route, participants);

    private static Participant? ResolveRouteParticipantInternal(SourceRoute route, IReadOnlyList<Participant> participants)
    {
        if (route.Mode == SourceRouteMode.Fixed)
        {
            return participants.FirstOrDefault(participant => participant.Id == route.ParticipantId);
        }

        if (route.Mode == SourceRouteMode.ActiveSpeaker)
        {
            return participants.FirstOrDefault(participant =>
                participant.IsActiveSpeaker && participant.Health != FeedHealth.VideoOff);
        }

        if (route.Mode == SourceRouteMode.Spotlight)
        {
            return participants.FirstOrDefault(participant => participant.Id == route.ParticipantId)
                ?? participants.ElementAtOrDefault(route.SpotlightIndex ?? 0);
        }

        return null;
    }

    private static SourceRoute RouteFromSlot(string sceneId, int index, string? slot)
    {
        if (slot == "screen-share")
        {
            return new SourceRoute
            {
                Id = $"{sceneId}-{index + 1}",
                Mode = SourceRouteMode.ScreenShare,
                AudioRole = SourceAudioRole.Audience
            };
        }

        if (slot?.StartsWith("capture:", StringComparison.Ordinal) == true)
        {
            return new SourceRoute
            {
                Id = $"{sceneId}-{index + 1}",
                Mode = SourceRouteMode.CaptureDevice,
                CaptureDeviceId = slot["capture:".Length..],
                AudioRole = SourceAudioRole.Isolated
            };
        }

        return new SourceRoute
        {
            Id = $"{sceneId}-{index + 1}",
            Mode = slot is not null ? SourceRouteMode.Fixed : SourceRouteMode.ActiveSpeaker,
            ParticipantId = slot,
            AudioRole = slot is not null ? SourceAudioRole.Isolated : SourceAudioRole.Mix
        };
    }

    private static IReadOnlyList<string> GetSlotDefaults(
        Scene scene,
        IReadOnlyList<SourceRoute>? existingRoutes,
        IReadOnlyList<Participant> participants)
    {
        var existingSlots = existingRoutes?
            .Select(RouteToSlot)
            .Where(slot => slot is not null && slot != "screen-share")
            .Cast<string>()
            .ToList() ?? [];

        var sortedParticipants = SortParticipantsForProduction(participants);
        var slotCount = GetSceneSlotCount(scene, participants);
        var defaults = new List<string>(existingSlots);

        foreach (var participant in sortedParticipants)
        {
            if (defaults.Count >= slotCount)
            {
                break;
            }

            if (!defaults.Contains(participant.Id))
            {
                defaults.Add(participant.Id);
            }
        }

        return defaults.Take(slotCount).ToList();
    }

    private static int GetRouteSlotCount(Scene scene, IReadOnlyList<Participant> participants) =>
        scene.Layout == "speaker-slides" ? 2 : GetSceneSlotCount(scene, participants);

    /// <summary>
    /// Layout slot capacity — independent of live roster so routes can be pre-built before join.
    /// </summary>
    private static int GetSceneSlotCount(Scene scene, IReadOnlyList<Participant> participants) =>
        SceneCanvasLayoutService.TemplateSlotCount(scene.Layout) ??
        scene.Layout switch
        {
            "grid" or "smart-grid" => 6,
            _ => 1
        };

    private static IReadOnlyList<Participant> SortParticipantsForProduction(IReadOnlyList<Participant> participants)
    {
        int RoleWeight(Participant participant) => participant.Role switch
        {
            ParticipantRole.Host => 4,
            ParticipantRole.Presenter => 3,
            ParticipantRole.Guest => 2,
            ParticipantRole.Panelist => 1,
            _ => 0
        };

        return participants
            .OrderByDescending(RoleWeight)
            .ThenBy(participant => participant.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private static IReadOnlyList<Participant> DedupeParticipants(IReadOnlyList<Participant> participants)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var result = new List<Participant>();

        foreach (var participant in participants)
        {
            if (seen.Add(participant.Id))
            {
                result.Add(participant);
            }
        }

        return result;
    }

    private static IReadOnlyList<string> GetDuplicateIds(IReadOnlyList<string> ids)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var duplicates = new HashSet<string>(StringComparer.Ordinal);

        foreach (var id in ids)
        {
            if (!seen.Add(id))
            {
                duplicates.Add(id);
            }
        }

        return duplicates.ToList();
    }
}
