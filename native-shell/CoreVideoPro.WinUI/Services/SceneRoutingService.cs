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

    public static string ModeToWire(SourceRouteMode mode) => mode switch
    {
        SourceRouteMode.Fixed => "fixed",
        SourceRouteMode.ActiveSpeaker => "active-speaker",
        SourceRouteMode.Spotlight => "spotlight",
        SourceRouteMode.ScreenShare => "screen-share",
        SourceRouteMode.None => "none",
        _ => "active-speaker"
    };

    public static SourceRouteMode ModeFromWire(string wire) => wire switch
    {
        "fixed" => SourceRouteMode.Fixed,
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

    public static IReadOnlyList<SourceRoute> GetRouteDefaults(
        Scene scene,
        IReadOnlyList<SourceRoute>? existingRoutes,
        IReadOnlyList<Participant> participants)
    {
        var slotCount = GetRouteSlotCount(scene, participants);
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
            .Select(route => ResolveRouteParticipant(route, sorted))
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
        if (route.Mode == SourceRouteMode.Fixed)
        {
            return new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = route.ParticipantId ?? participants.FirstOrDefault()?.Id,
                SpotlightIndex = route.SpotlightIndex,
                AudioRole = route.AudioRole == SourceAudioRole.Audience
                    ? SourceAudioRole.Isolated
                    : route.AudioRole
            };
        }

        if (route.Mode == SourceRouteMode.Spotlight)
        {
            var spotlightIndex = route.SpotlightIndex ?? 0;
            return new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = route.ParticipantId ?? participants.ElementAtOrDefault(spotlightIndex)?.Id,
                SpotlightIndex = spotlightIndex,
                AudioRole = route.AudioRole == SourceAudioRole.Audience
                    ? SourceAudioRole.Mix
                    : route.AudioRole
            };
        }

        if (route.Mode is SourceRouteMode.ScreenShare or SourceRouteMode.None)
        {
            return new SourceRoute
            {
                Id = route.Id,
                Mode = route.Mode,
                ParticipantId = null,
                SpotlightIndex = route.SpotlightIndex,
                AudioRole = SourceAudioRole.Audience
            };
        }

        return new SourceRoute
        {
            Id = route.Id,
            Mode = route.Mode,
            ParticipantId = null,
            SpotlightIndex = route.SpotlightIndex,
            AudioRole = route.AudioRole == SourceAudioRole.Isolated
                ? SourceAudioRole.Mix
                : route.AudioRole
        };
    }

    public static string? RouteToSlot(SourceRoute route)
    {
        if (route.Mode is SourceRouteMode.Fixed or SourceRouteMode.Spotlight)
        {
            return route.ParticipantId;
        }

        return route.Mode == SourceRouteMode.ScreenShare ? "screen-share" : null;
    }

    private static string? DescribeRoute(SourceRoute route, IReadOnlyList<Participant> participants) =>
        route.Mode switch
        {
            SourceRouteMode.ScreenShare => "screen share",
            SourceRouteMode.ActiveSpeaker => ResolveRouteParticipant(route, participants)?.Name ?? "active speaker",
            SourceRouteMode.None => null,
            _ => ResolveRouteParticipant(route, participants)?.Name
        };

    private static Participant? ResolveRouteParticipant(SourceRoute route, IReadOnlyList<Participant> participants)
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

    private static int GetSceneSlotCount(Scene scene, IReadOnlyList<Participant> participants) =>
        scene.Layout switch
        {
            "two-up" => Math.Min(2, participants.Count),
            "smart-grid" => Math.Min(6, participants.Count),
            _ => Math.Min(1, participants.Count)
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