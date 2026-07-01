using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Builds media-core-sync command batches from production context.
/// Mirrors <c>src/engine/nativeMediaCoreCommands.ts</c>.
/// </summary>
public static class MediaCoreCommandBuilder
{
    public static IReadOnlyList<NativeMediaCoreCommand> BuildSyncCommands(MediaCoreProductionSyncContext context)
    {
        var commands = new List<NativeMediaCoreCommand>
        {
            BuildZoomSourceRosterCommand(context.Participants),
            BuildActiveSpeakerCommand(context.Participants),
            BuildScreenShareCommand(context.Participants),
            BuildSceneGraphCommand(context.ActiveSceneId, context.SceneRoutes, context.SceneBackground),
            BuildMultiviewLayoutCommand(
                context.MultiviewSources,
                context.MultiviewCanvasWidth,
                context.MultiviewCanvasHeight,
                context.MultiviewColumns,
                context.MultiviewRows),
            BuildColorGradeCommand(context.ColorGrade),
            BuildOutputProfileCommand(context.CanvasOutputProfile),
            BuildSrtIngestSourcesCommand(context.SrtIngestSources),
            BuildBrandKitCommand(context.BrandKit),
            BuildAudioMixCommand(context.AudioMixChannels, context.AudioLimiterEnabled),
            BuildAudioMonitorCommand(context.AudioMonitor),
            BuildAudioRoutingMatrixCommand(context.AudioRoutingSends),
            BuildCaptureAudioSourcesCommand(context.CaptureAudioSources)
        };

        commands.AddRange(BuildOverlayCommands(context.Graphics, context.LowerThirdKey));
        commands.AddRange(BuildCaptionCommands(context.CaptionText, context.CaptionSpeaker));

        var mediaPlaybackCommand = BuildMediaPlaybackCommand(context);
        if (mediaPlaybackCommand is not null)
        {
            commands.Add(mediaPlaybackCommand);
        }

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
        IReadOnlyList<MediaCoreSceneRouteWire> routes,
        MediaCoreSceneBackgroundWire? background = null) =>
        Command("load-scene-graph", new Dictionary<string, object?>
        {
            ["sceneId"] = sceneId,
            ["background"] = background is null
                ? null
                : new Dictionary<string, object?>
                {
                    ["mediaAssetId"] = background.MediaAssetId,
                    ["mediaAssetName"] = background.MediaAssetName,
                    ["mediaAssetKind"] = background.MediaAssetKind,
                    ["mediaAssetPath"] = background.MediaAssetPath,
                    ["playing"] = background.Playing
                },
            ["routes"] = routes.Select(route =>
            {
                var payload = new Dictionary<string, object?>
                {
                    ["routeId"] = route.RouteId,
                    ["mode"] = route.Mode,
                    ["audioRole"] = route.AudioRole,
                    ["participantId"] = route.ParticipantId,
                    ["captureDeviceId"] = route.CaptureDeviceId,
                    ["fitMode"] = route.FitMode,
                    ["borderStyle"] = route.BorderStyle,
                    ["borderColor"] = route.BorderColor,
                    ["borderThickness"] = route.BorderThickness,
                    ["sourceScale"] = route.SourceScale,
                    ["sourceOffsetX"] = route.SourceOffsetX,
                    ["sourceOffsetY"] = route.SourceOffsetY,
                    ["ptz"] = new Dictionary<string, object?>
                    {
                        ["zoom"] = route.SourceScale,
                        ["pan"] = route.SourceOffsetX,
                        ["tilt"] = route.SourceOffsetY
                    },
                    ["mediaAssetId"] = route.MediaAssetId,
                    ["mediaAssetName"] = route.MediaAssetName,
                    ["mediaAssetKind"] = route.MediaAssetKind,
                    ["mediaAssetPath"] = route.MediaAssetPath,
                    ["mediaPlaybackKey"] = route.MediaPlaybackKey,
                    ["mediaAssetPlaying"] = route.MediaAssetPlaying,
                    ["colorGrade"] = route.ColorGrade is null
                        ? null
                        : new Dictionary<string, object?>
                        {
                            ["lut"] = route.ColorGrade.Lut,
                            ["exposure"] = route.ColorGrade.Exposure,
                            ["contrast"] = route.ColorGrade.Contrast,
                            ["saturation"] = route.ColorGrade.Saturation,
                            ["temperature"] = route.ColorGrade.Temperature
                        }
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

    /// <summary>
    /// Builds <c>set-multiview-layout</c> from the ordered Show Input roster. The core composites
    /// these into ONE shared texture and computes the grid cells itself; cols/rows are advisory.
    /// An empty source list clears the layout (turns the multiview composite pass off).
    /// </summary>
    public static NativeMediaCoreCommand BuildMultiviewLayoutCommand(
        IReadOnlyList<MediaCoreMultiviewSourceWire> sources,
        int canvasWidth,
        int canvasHeight,
        int columns,
        int rows) =>
        Command("set-multiview-layout", new Dictionary<string, object?>
        {
            ["canvasWidth"] = canvasWidth > 0 ? canvasWidth : 1920,
            ["canvasHeight"] = canvasHeight > 0 ? canvasHeight : 1080,
            ["cols"] = columns,
            ["rows"] = rows,
            ["sources"] = sources.Select(source => new Dictionary<string, object?>
            {
                ["sourceId"] = source.SourceId,
                ["kind"] = source.Kind,
                ["participantId"] = source.ParticipantId,
                ["captureDeviceId"] = source.CaptureDeviceId,
                ["mediaAssetId"] = source.MediaAssetId,
                ["slot"] = source.Slot,
                ["label"] = source.Label
            }).ToList()
        });

    /// <summary>
    /// Builds <c>configure-multiviewer</c> from the operator's multiviewer preferences. This carries
    /// the user-chosen layout mode, tile count, and overlay toggles (labels/tally/meters/clock) to
    /// the core, independent of the source roster carried by <c>set-multiview-layout</c>.
    /// </summary>
    public static NativeMediaCoreCommand BuildConfigureMultiviewerCommand(
        string layoutMode,
        int tileCount,
        bool showLabels,
        bool showTally,
        bool showMeters,
        bool showClock) =>
        Command("configure-multiviewer", new Dictionary<string, object?>
        {
            ["layoutMode"] = layoutMode,
            ["tileCount"] = tileCount,
            ["showLabels"] = showLabels,
            ["showTally"] = showTally,
            ["showMeters"] = showMeters,
            ["showClock"] = showClock
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

    private static NativeMediaCoreCommand BuildOutputProfileCommand(MediaCoreOutputProfileWire profile) =>
        Command("set-output-profile", new Dictionary<string, object?>
        {
            ["profileId"] = profile.ProfileId,
            ["resolution"] = profile.Resolution,
            ["width"] = profile.Width,
            ["height"] = profile.Height,
            ["fps"] = profile.Fps,
            ["targetBitrateMbps"] = profile.TargetBitrateMbps,
            ["codec"] = profile.Codec
        });

    private static NativeMediaCoreCommand BuildSrtIngestSourcesCommand(IReadOnlyList<MediaCoreSrtIngestSourceWire> sources) =>
        Command("configure-srt-ingest-sources", new Dictionary<string, object?>
        {
            ["sources"] = sources.Select(source => new Dictionary<string, object?>
            {
                ["id"] = source.Id,
                ["deviceId"] = source.DeviceId,
                ["name"] = source.Name,
                ["mode"] = source.Mode,
                ["host"] = source.Host,
                ["port"] = source.Port,
                ["latencyMs"] = source.LatencyMs,
                ["streamId"] = source.StreamId,
                ["passphrase"] = source.Passphrase
            }).ToList()
        });

    private static NativeMediaCoreCommand BuildBrandKitCommand(MediaCoreBrandKitWire brandKit) =>
        Command("set-brand-kit", new Dictionary<string, object?>
        {
            ["name"] = brandKit.Name,
            ["logoText"] = brandKit.LogoText,
            ["logoAssetId"] = brandKit.LogoAssetId,
            ["logoAssetName"] = brandKit.LogoAssetName,
            ["logoAssetPath"] = brandKit.LogoAssetPath,
            ["brandColor"] = brandKit.BrandColor,
            ["accentColor"] = brandKit.AccentColor,
            ["backgroundColor"] = brandKit.BackgroundColor,
            ["fontFamily"] = brandKit.FontFamily,
            ["lowerThirdStyle"] = brandKit.LowerThirdStyle,
            ["captionStyle"] = brandKit.CaptionStyle,
            ["defaultOverlayBehavior"] = brandKit.DefaultOverlayBehavior
        });

    private static NativeMediaCoreCommand BuildAudioMixCommand(IReadOnlyList<MediaCoreAudioMixChannelWire> channels, bool limiterEnabled) =>
        Command("sync-participant-audio-mix", new Dictionary<string, object?>
        {
            ["limiterEnabled"] = limiterEnabled,
            ["channels"] = channels.Select(channel => new
            {
                participantId = channel.ParticipantId,
                inputLevel = channel.InputLevel,
                muted = channel.Muted,
                noiseSuppression = channel.NoiseSuppression,
                manualGainDb = channel.ManualGainDb,
                pan = channel.Pan,
                solo = channel.Solo,
                pluginInserts = channel.PluginInserts ?? []
            }).ToList()
        });

    private static NativeMediaCoreCommand BuildAudioMonitorCommand(MediaCoreAudioMonitorWire monitor) =>
        Command("sync-audio-monitor", new Dictionary<string, object?>
        {
            ["enabled"] = monitor.Enabled,
            ["deviceId"] = monitor.DeviceId,
            ["deviceName"] = monitor.DeviceName,
            ["volume"] = Math.Clamp(monitor.Volume, 0, 1)
        });

    private static NativeMediaCoreCommand BuildAudioRoutingMatrixCommand(IReadOnlyList<MediaCoreAudioRoutingSendWire> sends) =>
        Command("sync-audio-routing-matrix", new Dictionary<string, object?>
        {
            ["sends"] = sends.Select(send => new
            {
                sourceId = send.SourceId,
                busId = send.BusId,
                gainDb = send.GainDb,
                busPluginInserts = send.BusPluginInserts ?? []
            }).ToList()
        });

    private static NativeMediaCoreCommand BuildCaptureAudioSourcesCommand(IReadOnlyList<MediaCoreCaptureAudioSourceWire> sources) =>
        Command("sync-capture-audio-sources", new Dictionary<string, object?>
        {
            ["sources"] = sources.Select(source => new
            {
                captureDeviceId = source.CaptureDeviceId,
                audioDeviceId = source.AudioDeviceId,
                audioDeviceName = source.AudioDeviceName,
                audioSourceKind = source.AudioSourceKind,
                nativeAudioDeviceId = source.NativeAudioDeviceId,
                audioDriverName = source.AudioDriverName,
                embedded = source.Embedded,
                audioSyncOffsetMs = source.AudioSyncOffsetMs
            }).ToList()
        });

    private static IEnumerable<NativeMediaCoreCommand> BuildOverlayCommands(
        IReadOnlyList<MediaCoreGraphicWire> graphics,
        MediaCoreLowerThirdKeyWire? lowerThirdKey)
    {
        if (lowerThirdKey is { Enabled: true } && !string.IsNullOrWhiteSpace(lowerThirdKey.SourceName))
        {
            yield return Command("set-overlay-asset", new Dictionary<string, object?>
            {
                ["overlayId"] = "key:lower-third",
                ["text"] = lowerThirdKey.SourceName,
                ["position"] = "lower-third",
                ["enabled"] = true,
                ["sourceId"] = lowerThirdKey.SourceId,
                ["sourceName"] = lowerThirdKey.SourceName,
                ["title"] = lowerThirdKey.Title,
                ["org"] = lowerThirdKey.Org,
                ["keyPosition"] = lowerThirdKey.Position,
                ["keyPhase"] = lowerThirdKey.Phase,
                ["buildInMs"] = Math.Clamp(lowerThirdKey.BuildInMs, 50, 2000),
                ["buildOutMs"] = Math.Clamp(lowerThirdKey.BuildOutMs, 50, 2000),
                ["keyer"] = "downstream"
            });
        }
        else
        {
            yield return Command("set-overlay-asset", new Dictionary<string, object?>
            {
                ["overlayId"] = "key:lower-third",
                ["text"] = string.Empty,
                ["position"] = "lower-third",
                ["enabled"] = false,
                ["keyPhase"] = "hidden",
                ["buildInMs"] = 250,
                ["buildOutMs"] = 200,
                ["keyer"] = "downstream"
            });
        }

        foreach (var graphic in graphics.Where(graphic => graphic.Enabled))
        {
            yield return Command("set-overlay-asset", new Dictionary<string, object?>
            {
                ["overlayId"] = graphic.Id,
                ["text"] = graphic.Text,
                ["position"] = graphic.Position,
                ["enabled"] = true
            });
        }
    }

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

    public static NativeMediaCoreCommand? BuildMediaPlaybackCommand(MediaCoreProductionSyncContext context)
    {
        if (string.IsNullOrWhiteSpace(context.SelectedMediaAssetId))
        {
            return null;
        }

        return Command("set-media-playback", new Dictionary<string, object?>
        {
            ["mediaAssetId"] = context.SelectedMediaAssetId.Trim(),
            ["mediaAssetName"] = context.SelectedMediaAssetName?.Trim() ?? string.Empty,
            ["mediaAssetKind"] = context.SelectedMediaAssetKind?.Trim() ?? string.Empty,
            ["mediaAssetPath"] = context.SelectedMediaAssetPath?.Trim() ?? string.Empty,
            ["mediaPlaybackKey"] = context.SelectedMediaPlaybackKey?.Trim() ?? string.Empty,
            ["playing"] = context.SelectedMediaAssetPlaying
        });
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
            destinations.AddRange(context.StreamDestinations.Where(destination => !string.IsNullOrWhiteSpace(destination)));
        }

        if (destinations.Count == 0)
        {
            return null;
        }

        return Command("start-program-output", new Dictionary<string, object?>
        {
            ["destinations"] = destinations.Distinct(StringComparer.Ordinal).ToList(),
            ["streamOutputProfile"] = OutputProfilePayload(context.StreamOutputProfile),
            ["recordingOutputProfile"] = OutputProfilePayload(context.RecordingOutputProfile),
            ["destinationSettings"] = context.StreamDestinationSettings.Select(destination => new Dictionary<string, object?>
            {
                ["id"] = destination.Id,
                ["label"] = destination.Label,
                ["protocol"] = destination.Protocol,
                ["url"] = destination.Url,
                ["streamKey"] = destination.StreamKey,
                ["mode"] = destination.Mode,
                ["host"] = destination.Host,
                ["port"] = destination.Port,
                ["latencyMs"] = destination.LatencyMs,
                ["latencyUs"] = destination.LatencyUs,
                ["passphrase"] = destination.Passphrase,
                ["keyLength"] = destination.KeyLength,
                ["streamId"] = destination.StreamId,
                ["ndiName"] = destination.NdiName,
                ["ndiGroup"] = destination.NdiGroup,
                ["ffmpegBinDirectory"] = destination.FfmpegBinDirectory,
                ["fps"] = destination.Fps,
                ["targetBitrateMbps"] = destination.TargetBitrateMbps,
                ["audioBitrateKbps"] = destination.AudioBitrateKbps,
                ["videoCodec"] = destination.VideoCodec,
                ["encoderMode"] = destination.EncoderMode
            }).ToList(),
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
            ["targetBitrateMbps"] = context.RecordingOutputProfile.TargetBitrateMbps,
            ["audioBitrateKbps"] = context.RecordingOutputProfile.AudioBitrateKbps,
            ["renderProfile"] = OutputProfilePayload(context.RecordingOutputProfile),
            ["isoParticipantIds"] = targets.IsoParticipantIds
        });

        yield return Command("start-recording-session", new Dictionary<string, object?>
        {
            ["sessionId"] = sessionId,
            ["targetFolder"] = targets.TargetFolder,
            ["filenamePrefix"] = targets.FilenamePrefix,
            ["format"] = targets.Format,
            ["quality"] = targets.Quality,
            ["targetBitrateMbps"] = context.RecordingOutputProfile.TargetBitrateMbps,
            ["audioBitrateKbps"] = context.RecordingOutputProfile.AudioBitrateKbps,
            ["renderProfile"] = OutputProfilePayload(context.RecordingOutputProfile),
            ["isoParticipantIds"] = targets.IsoParticipantIds
        });
    }

    private static Dictionary<string, object?> OutputProfilePayload(MediaCoreOutputProfileWire profile) =>
        new()
        {
            ["profileId"] = profile.ProfileId,
            ["resolution"] = profile.Resolution,
            ["width"] = profile.Width,
            ["height"] = profile.Height,
            ["fps"] = profile.Fps,
            ["targetBitrateMbps"] = profile.TargetBitrateMbps,
            ["audioBitrateKbps"] = profile.AudioBitrateKbps,
            ["codec"] = profile.Codec
        };

    private static NativeMediaCoreCommand Command(string type, IReadOnlyDictionary<string, object?> payload) =>
        new()
        {
            Type = type,
            ExtensionData = payload.ToDictionary(
                pair => pair.Key,
                pair => JsonSerializer.SerializeToElement(pair.Value))
        };
}
