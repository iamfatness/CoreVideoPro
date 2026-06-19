using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Builds media-core-sync command batches from production context.
/// Mirrors <c>src/engine/nativeMediaCoreCommands.ts</c>.
/// </summary>
public static class MediaCoreCommandBuilder
{
    private static readonly MediaCoreOutputProfileWire DefaultOutputProfile = new(
        ProfileId: "1080p60",
        Resolution: "1920x1080",
        Width: 1920,
        Height: 1080,
        Fps: 60,
        TargetBitrateMbps: 8.2);

    public static IReadOnlyList<NativeMediaCoreCommand> BuildSyncCommands(MediaCoreProductionSyncContext context)
    {
        var commands = new List<NativeMediaCoreCommand>
        {
            BuildZoomSourceRosterCommand(context.Participants),
            BuildActiveSpeakerCommand(context.Participants),
            BuildScreenShareCommand(context.Participants),
            BuildSceneGraphCommand(context.ActiveSceneId, context.SceneRoutes),
            BuildColorGradeCommand(context.ColorGrade),
            BuildOutputProfileCommand(),
            BuildBrandKitCommand(context.BrandKit),
            BuildAudioMixCommand(context.AudioMixChannels)
        };

        commands.AddRange(BuildOverlayCommands(context.Graphics));
        commands.AddRange(BuildCaptionCommands(context.CaptionText, context.CaptionSpeaker));

        var outputCommand = BuildOutputCommand(context);
        if (outputCommand is not null)
        {
            commands.Add(Command("prepare-encoder-session", new Dictionary<string, object?>
            {
                ["reason"] = "Production outputs armed."
            }));
            commands.Add(outputCommand);
            commands.Add(Command("start-encoder-session", new Dictionary<string, object?>()));
        }
        else
        {
            commands.Add(Command("stop-encoder-session", new Dictionary<string, object?>
            {
                ["reason"] = "Outputs disabled in production state."
            }));
        }

        commands.AddRange(BuildRecordingCommands(context));
        return commands;
    }

    public static NativeMediaCoreCommand BuildSceneGraphCommand(
        string sceneId,
        IReadOnlyList<MediaCoreSceneRouteWire> routes) =>
        Command("load-scene-graph", new Dictionary<string, object?>
        {
            ["sceneId"] = sceneId,
            ["routes"] = routes.Select(route =>
            {
                var payload = new Dictionary<string, object?>
                {
                    ["routeId"] = route.RouteId,
                    ["mode"] = route.Mode,
                    ["audioRole"] = route.AudioRole,
                    ["participantId"] = route.ParticipantId
                };

                if (route.RectX is not null && route.RectY is not null &&
                    route.RectWidth is not null && route.RectHeight is not null)
                {
                    payload["rect"] = new Dictionary<string, object?>
                    {
                        ["x"] = route.RectX,
                        ["y"] = route.RectY,
                        ["width"] = route.RectWidth,
                        ["height"] = route.RectHeight
                    };
                }

                if (route.ZIndex is not null)
                {
                    payload["zIndex"] = route.ZIndex;
                }

                return payload;
            }).ToList()
        });

    private static NativeMediaCoreCommand BuildZoomSourceRosterCommand(IReadOnlyList<MediaCoreParticipantWire> participants) =>
        Command("set-zoom-source-roster", new Dictionary<string, object?>
        {
            ["sources"] = participants.Select(participant => new
            {
                sourceId = $"participant:{participant.Id}",
                participantId = participant.Id,
                displayName = participant.Name,
                role = participant.Role,
                breakoutRoomId = participant.BreakoutRoomId,
                breakoutRoomName = participant.BreakoutRoomName,
                hasVideo = !participant.Health.Equals("video-off", StringComparison.Ordinal),
                hasAudio = true,
                isMuted = participant.IsMuted,
                isActiveSpeaker = participant.IsActiveSpeaker,
                isScreenSharing = participant.IsScreenSharing,
                audioLevel = participant.AudioLevel,
                health = participant.Health
            }).ToList()
        });

    private static NativeMediaCoreCommand BuildActiveSpeakerCommand(IReadOnlyList<MediaCoreParticipantWire> participants)
    {
        var activeSpeakerId = participants
            .FirstOrDefault(participant =>
                participant.IsActiveSpeaker &&
                !participant.Health.Equals("video-off", StringComparison.Ordinal))
            ?.Id;

        return Command("set-active-speaker", new Dictionary<string, object?>
        {
            ["participantId"] = activeSpeakerId
        });
    }

    private static NativeMediaCoreCommand BuildScreenShareCommand(IReadOnlyList<MediaCoreParticipantWire> participants)
    {
        var screenShareId = participants
            .FirstOrDefault(participant => participant.IsScreenSharing)
            ?.Id;

        return Command("set-screen-share-source", new Dictionary<string, object?>
        {
            ["participantId"] = screenShareId
        });
    }

    private static NativeMediaCoreCommand BuildColorGradeCommand(MediaCoreColorGradeWire colorGrade) =>
        Command("set-color-grade", new Dictionary<string, object?>
        {
            ["lut"] = colorGrade.Lut,
            ["exposure"] = colorGrade.Exposure,
            ["contrast"] = colorGrade.Contrast,
            ["saturation"] = colorGrade.Saturation,
            ["temperature"] = colorGrade.Temperature
        });

    private static NativeMediaCoreCommand BuildOutputProfileCommand() =>
        Command("set-output-profile", new Dictionary<string, object?>
        {
            ["profileId"] = DefaultOutputProfile.ProfileId,
            ["resolution"] = DefaultOutputProfile.Resolution,
            ["width"] = DefaultOutputProfile.Width,
            ["height"] = DefaultOutputProfile.Height,
            ["fps"] = DefaultOutputProfile.Fps,
            ["targetBitrateMbps"] = DefaultOutputProfile.TargetBitrateMbps
        });

    private static NativeMediaCoreCommand BuildBrandKitCommand(MediaCoreBrandKitWire brandKit) =>
        Command("set-brand-kit", new Dictionary<string, object?>
        {
            ["name"] = brandKit.Name,
            ["logoText"] = brandKit.LogoText,
            ["brandColor"] = brandKit.BrandColor,
            ["accentColor"] = brandKit.AccentColor,
            ["backgroundColor"] = brandKit.BackgroundColor,
            ["fontFamily"] = brandKit.FontFamily,
            ["lowerThirdStyle"] = brandKit.LowerThirdStyle
        });

    private static NativeMediaCoreCommand BuildAudioMixCommand(IReadOnlyList<MediaCoreAudioMixChannelWire> channels) =>
        Command("sync-participant-audio-mix", new Dictionary<string, object?>
        {
            ["channels"] = channels.Select(channel => new
            {
                participantId = channel.ParticipantId,
                inputLevel = channel.InputLevel,
                muted = channel.Muted,
                noiseSuppression = channel.NoiseSuppression,
                manualGainDb = channel.ManualGainDb
            }).ToList()
        });

    private static IEnumerable<NativeMediaCoreCommand> BuildOverlayCommands(IReadOnlyList<MediaCoreGraphicWire> graphics) =>
        graphics
            .Where(graphic => graphic.Enabled)
            .Select(graphic => Command("set-overlay-asset", new Dictionary<string, object?>
            {
                ["overlayId"] = graphic.Id,
                ["text"] = graphic.Text,
                ["position"] = graphic.Position
            }));

    private static IEnumerable<NativeMediaCoreCommand> BuildCaptionCommands(string? captionText, string? captionSpeaker)
    {
        yield return Command("set-caption-enabled", new Dictionary<string, object?> { ["enabled"] = true });

        if (!string.IsNullOrWhiteSpace(captionText))
        {
            yield return Command("push-caption-cue", new Dictionary<string, object?>
            {
                ["text"] = captionText.Trim(),
                ["speaker"] = captionSpeaker,
                ["atMs"] = 0
            });
        }
    }

    private static NativeMediaCoreCommand? BuildOutputCommand(MediaCoreProductionSyncContext context)
    {
        if (!context.Recording && !context.Streaming)
        {
            return null;
        }

        var destinations = new List<string>();
        if (context.Recording)
        {
            destinations.Add("recording");
        }

        if (context.Streaming)
        {
            destinations.AddRange(context.StreamDestinations);
        }

        return Command("start-program-output", new Dictionary<string, object?>
        {
            ["destinations"] = destinations.Distinct(StringComparer.Ordinal).ToList(),
            ["isoParticipantIds"] = context.Recording
                ? context.RecordingTargets.IsoParticipantIds
                : Array.Empty<string>()
        });
    }

    private static IEnumerable<NativeMediaCoreCommand> BuildRecordingCommands(MediaCoreProductionSyncContext context)
    {
        if (!context.Recording)
        {
            yield return Command("stop-recording-session", new Dictionary<string, object?>
            {
                ["reason"] = "Recording disabled in production state."
            });
            yield break;
        }

        var targets = context.RecordingTargets;
        var isoSuffix = targets.IsoParticipantIds.Count > 0
            ? string.Join("-", targets.IsoParticipantIds)
            : "program";
        var sessionId = $"{targets.FilenamePrefix}-{isoSuffix}";

        yield return Command("set-recording-targets", new Dictionary<string, object?>
        {
            ["targetFolder"] = targets.TargetFolder,
            ["filenamePrefix"] = targets.FilenamePrefix,
            ["format"] = targets.Format,
            ["quality"] = targets.Quality,
            ["isoParticipantIds"] = targets.IsoParticipantIds
        });

        yield return Command("start-recording-session", new Dictionary<string, object?>
        {
            ["sessionId"] = sessionId,
            ["targetFolder"] = targets.TargetFolder,
            ["filenamePrefix"] = targets.FilenamePrefix,
            ["format"] = targets.Format,
            ["quality"] = targets.Quality,
            ["isoParticipantIds"] = targets.IsoParticipantIds
        });
    }

    private static NativeMediaCoreCommand Command(string type, IReadOnlyDictionary<string, object?> payload) =>
        new()
        {
            Type = type,
            ExtensionData = payload.ToDictionary(
                pair => pair.Key,
                pair => JsonSerializer.SerializeToElement(pair.Value))
        };

    private sealed record MediaCoreOutputProfileWire(
        string ProfileId,
        string Resolution,
        int Width,
        int Height,
        int Fps,
        double TargetBitrateMbps);
}