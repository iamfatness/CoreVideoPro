import type {
  GraphicOverlay,
  OutputDestination,
  Participant,
  ParticipantVideoEffect,
  ProductionState,
  SceneTemplate,
  SourceRoute
} from "../domain/production";
import type { NativeMediaCoreCommand } from "./nativeMediaCoreProtocol";

export function buildNativeMediaCoreCommands(state: ProductionState): NativeMediaCoreCommand[] {
  const activeScene = state.scenes.find((scene) => scene.id === state.activeSceneId) ?? state.scenes[0];
  const commands: NativeMediaCoreCommand[] = [
    {
      type: "load-scene-graph",
      sceneId: activeScene.id,
      routes: getSceneRoutes(activeScene, state.participants).map((route) => ({
        routeId: route.id,
        mode: route.mode,
        participantId: route.participantId,
        audioRole: route.audioRole
      }))
    },
    ...state.videoEffects.map(buildTransformCommand),
    ...state.graphics.filter((graphic) => graphic.enabled).map(buildOverlayCommand),
    buildOutputProfileCommand(state)
  ];

  const outputCommand = buildOutputCommand(state);
  if (outputCommand) {
    commands.push(outputCommand);
  }
  commands.push(...buildRecordingCommands(state));

  return commands;
}

function getSceneRoutes(scene: SceneTemplate, participants: Participant[]): SourceRoute[] {
  if (scene.routes?.length) {
    return scene.routes.map((route, index) => normalizeRoute(route, scene.id, index, participants));
  }

  const slots = scene.layout === "speaker-slides" ? [...(scene.slots ?? []), "screen-share"] : scene.slots ?? [];
  const slotCount = scene.layout === "speaker-slides" ? 2 : getSceneSlotCount(scene, participants);
  const fallbackParticipants = participants.filter((participant) => participant.health !== "video-off");

  return Array.from({ length: slotCount }, (_, index) => {
    const slot = slots[index] ?? fallbackParticipants[index]?.id;
    const route: SourceRoute =
      slot === "screen-share"
        ? { id: `${scene.id}-${index + 1}`, mode: "screen-share", audioRole: "audience" }
        : {
            id: `${scene.id}-${index + 1}`,
            mode: slot ? "fixed" : "active-speaker",
            participantId: slot,
            audioRole: slot ? "isolated" : "mix"
          };

    return normalizeRoute(route, scene.id, index, participants);
  });
}

function normalizeRoute(route: SourceRoute, sceneId: string, index: number, participants: Participant[]): SourceRoute {
  if (route.mode === "fixed") {
    return {
      ...route,
      id: route.id || `${sceneId}-${index + 1}`,
      participantId: route.participantId ?? participants.find((participant) => participant.health !== "video-off")?.id,
      audioRole: route.audioRole === "audience" ? "isolated" : route.audioRole
    };
  }

  if (route.mode === "spotlight") {
    return {
      ...route,
      id: route.id || `${sceneId}-${index + 1}`,
      participantId: route.participantId ?? participants[route.spotlightIndex ?? 0]?.id,
      audioRole: route.audioRole === "audience" ? "mix" : route.audioRole
    };
  }

  if (route.mode === "screen-share" || route.mode === "none") {
    return { ...route, id: route.id || `${sceneId}-${index + 1}`, participantId: undefined, audioRole: "audience" };
  }

  return {
    ...route,
    id: route.id || `${sceneId}-${index + 1}`,
    participantId: undefined,
    audioRole: route.audioRole === "isolated" ? "mix" : route.audioRole
  };
}

function buildTransformCommand(effect: ParticipantVideoEffect): NativeMediaCoreCommand {
  const inset = effect.cropMode === "manual" ? Math.min(0.24, Math.max(0, (effect.manualZoom - 1) / 4)) : 0;

  return {
    type: "set-participant-transform",
    participantId: effect.participantId,
    crop: {
      x: Number(inset.toFixed(3)),
      y: Number(inset.toFixed(3)),
      width: Number((1 - inset * 2).toFixed(3)),
      height: Number((1 - inset * 2).toFixed(3))
    },
    scale: effect.cropMode === "manual" ? effect.manualZoom : 1,
    chromaKey: effect.chromaKeyEnabled
      ? {
          enabled: true,
          color: effect.chromaKeyColor,
          spillSuppression: effect.spillSuppression
        }
      : undefined
  };
}

function buildOverlayCommand(graphic: GraphicOverlay): NativeMediaCoreCommand {
  return {
    type: "set-overlay-asset",
    overlayId: graphic.id,
    text: graphic.text,
    position: graphic.position
  };
}

function buildOutputProfileCommand(state: ProductionState): NativeMediaCoreCommand {
  const profile = state.outputProfiles.find((candidate) => candidate.id === state.selectedOutputProfileId) ?? state.outputProfiles[0];
  const [width, height] = profile.resolution.split("x").map((part) => Number(part));

  return {
    type: "set-output-profile",
    profileId: profile.id,
    resolution: profile.resolution,
    width,
    height,
    fps: profile.fps,
    targetBitrateMbps: profile.targetBitrateMbps
  };
}

function buildOutputCommand(state: ProductionState): NativeMediaCoreCommand | undefined {
  if (!state.recording && !state.streaming) {
    return undefined;
  }

  const destinations = [
    ...(state.recording ? ["recording" as const] : []),
    ...(state.streaming ? state.outputDestinations.filter((destination) => destination.enabled).map(mapDestinationProtocol) : [])
  ];

  return {
    type: "start-program-output",
    destinations: [...new Set(destinations)],
    isoParticipantIds: state.recording ? state.recordingSettings.isoParticipantIds : []
  };
}

function buildRecordingCommands(state: ProductionState): NativeMediaCoreCommand[] {
  if (!state.recording) {
    return [{ type: "stop-recording-session", reason: "Recording disabled in production state." }];
  }

  const targets = {
    targetFolder: state.recordingSettings.folder,
    filenamePrefix: state.recordingSettings.filenamePrefix,
    format: state.recordingSettings.format,
    quality: state.recordingSettings.quality,
    isoParticipantIds: state.recordingSettings.isoParticipantIds
  };

  return [
    {
      type: "set-recording-targets",
      ...targets
    },
    {
      type: "start-recording-session",
      sessionId: `${targets.filenamePrefix}-${targets.isoParticipantIds.join("-") || "program"}`,
      ...targets
    }
  ];
}

function mapDestinationProtocol(destination: OutputDestination) {
  return destination.protocol.toLowerCase() as "rtmp" | "ndi" | "srt" | "webrtc";
}

function getSceneSlotCount(scene: SceneTemplate, participants: Participant[]) {
  if (scene.layout === "two-up") {
    return Math.min(2, participants.length);
  }

  if (scene.layout === "smart-grid") {
    return Math.min(6, participants.length);
  }

  return Math.min(1, participants.length);
}
