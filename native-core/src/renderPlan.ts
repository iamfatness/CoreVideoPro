import type {
  MediaCoreColorGrade,
  MediaCoreOutputProfile,
  MediaCoreRenderPlan,
  MediaCoreResolvedRoute,
  MediaCoreRouteMode,
  MediaCoreZoomSource
} from "./protocol.js";
import type { MediaCoreCommand } from "./protocol.js";

type SceneGraphState = Extract<MediaCoreCommand, { type: "load-scene-graph" }>;
type OverlayState = Extract<MediaCoreCommand, { type: "set-overlay-asset" }>;

export function buildRenderPlan(input: {
  sceneGraph?: SceneGraphState;
  sources: MediaCoreZoomSource[];
  activeSpeakerId?: string;
  screenShareParticipantId?: string;
  overlays: OverlayState[];
  outputProfile: MediaCoreOutputProfile;
  colorGrade: MediaCoreColorGrade;
}): MediaCoreRenderPlan {
  const warnings: string[] = [];
  const assignedParticipants = new Set<string>();
  const routes = (input.sceneGraph?.routes ?? []).map((route): MediaCoreResolvedRoute => {
    const resolved = resolveRoute(route, input.sources, input.activeSpeakerId, input.screenShareParticipantId);

    if (resolved.participantId && route.audioRole === "isolated") {
      const source = input.sources.find((candidate) => candidate.participantId === resolved.participantId);
      if (source?.isMuted) {
        resolved.warning = `${source.displayName} is muted but assigned isolated audio.`;
      }
    }

    if (resolved.participantId && resolved.kind !== "screen-share" && assignedParticipants.has(resolved.participantId)) {
      resolved.warning = `${resolved.participantId} is assigned to multiple program routes.`;
    }

    if (resolved.participantId && resolved.kind !== "screen-share") {
      assignedParticipants.add(resolved.participantId);
    }

    if (resolved.warning) {
      warnings.push(resolved.warning);
    }

    return resolved;
  });

  const videoLayers = routes
    .filter((route) => route.status === "resolved" && route.sourceId && route.kind)
    .map((route, index) => ({
      layerId: `route:${route.routeId}`,
      kind: route.kind!,
      sourceId: route.sourceId,
      participantId: route.participantId,
      order: index,
      routeId: route.routeId
    }));

  const overlayLayers = input.overlays.map((overlay, index) => ({
    layerId: `overlay:${overlay.overlayId}`,
    kind: "overlay" as const,
    overlayId: overlay.overlayId,
    order: videoLayers.length + index,
    position: overlay.position
  }));

  return {
    sceneId: input.sceneGraph?.sceneId,
    outputProfile: input.outputProfile,
    colorGrade: input.colorGrade,
    sourceCount: input.sources.length,
    resolvedRouteCount: routes.filter((route) => route.status === "resolved").length,
    layers: [...videoLayers, ...overlayLayers],
    routes,
    warnings: [...new Set(warnings)]
  };
}

function resolveRoute(
  route: SceneGraphState["routes"][number],
  sources: MediaCoreZoomSource[],
  activeSpeakerId?: string,
  screenShareParticipantId?: string
): MediaCoreResolvedRoute {
  if (route.mode === "none") {
    return {
      routeId: route.routeId,
      mode: route.mode,
      audioRole: route.audioRole,
      status: "disabled"
    };
  }

  if (route.mode === "screen-share") {
    const screenSource = resolveScreenShare(sources, screenShareParticipantId);
    return screenSource
      ? resolvedRoute(route.routeId, route.mode, route.audioRole, screenSource, "screen-share")
      : missingRoute(route.routeId, route.mode, route.audioRole, "Screen share route requested but no active screen share source is available.");
  }

  const participantId = route.mode === "active-speaker" ? activeSpeakerId : route.participantId;
  if (!participantId) {
    return missingRoute(route.routeId, route.mode, route.audioRole, `${route.mode} route has no participant assignment.`);
  }

  const source = sources.find((candidate) => candidate.participantId === participantId);
  if (!source) {
    return missingRoute(route.routeId, route.mode, route.audioRole, `${participantId} is not present in the Zoom source roster.`);
  }

  if (!source.hasVideo || source.health === "video-off") {
    return missingRoute(route.routeId, route.mode, route.audioRole, `${source.displayName} has no clean video feed.`);
  }

  return resolvedRoute(route.routeId, route.mode, route.audioRole, source, "participant-video");
}

function resolveScreenShare(sources: MediaCoreZoomSource[], screenShareParticipantId?: string) {
  if (screenShareParticipantId) {
    return sources.find((source) => source.participantId === screenShareParticipantId && source.isScreenSharing);
  }

  return sources.find((source) => source.isScreenSharing);
}

function resolvedRoute(
  routeId: string,
  mode: MediaCoreRouteMode,
  audioRole: SceneGraphState["routes"][number]["audioRole"],
  source: MediaCoreZoomSource,
  kind: "participant-video" | "screen-share"
): MediaCoreResolvedRoute {
  return {
    routeId,
    mode,
    audioRole,
    sourceId: kind === "screen-share" ? `screen-share:${source.participantId}` : source.sourceId,
    participantId: source.participantId,
    kind,
    status: "resolved"
  };
}

function missingRoute(
  routeId: string,
  mode: MediaCoreRouteMode,
  audioRole: SceneGraphState["routes"][number]["audioRole"],
  warning: string
): MediaCoreResolvedRoute {
  return {
    routeId,
    mode,
    audioRole,
    status: "missing",
    warning
  };
}
