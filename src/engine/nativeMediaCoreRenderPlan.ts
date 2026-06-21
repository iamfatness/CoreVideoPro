import type {
  NativeMediaCoreColorGrade,
  NativeMediaCoreCommand,
  NativeMediaCoreOutputProfile,
  NativeMediaCoreRenderPlan,
  NativeMediaCoreResolvedRoute,
  NativeMediaCoreZoomSource
} from "./nativeMediaCoreProtocol";

type SceneGraph = Extract<NativeMediaCoreCommand, { type: "load-scene-graph" }>;
type Overlay = Extract<NativeMediaCoreCommand, { type: "set-overlay-asset" }>;

export function buildNativeMediaCoreRenderPlan(input: {
  sceneGraph?: SceneGraph;
  sources: NativeMediaCoreZoomSource[];
  activeSpeakerId?: string;
  screenShareParticipantId?: string;
  overlays: Overlay[];
  outputProfile: NativeMediaCoreOutputProfile;
  colorGrade: NativeMediaCoreColorGrade;
}): NativeMediaCoreRenderPlan {
  const warnings: string[] = [];
  const assignedParticipants = new Set<string>();
  const routes = (input.sceneGraph?.routes ?? []).map((route): NativeMediaCoreResolvedRoute => {
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
      routeId: route.routeId,
      fitMode: route.fitMode,
      sourceScale: route.sourceScale,
      sourceOffsetX: route.sourceOffsetX,
      sourceOffsetY: route.sourceOffsetY,
      ptz: route.ptz
    }));
  const overlayLayers = input.overlays.map((overlay, index) => ({
    layerId: `overlay:${overlay.overlayId}`,
    kind: "overlay" as const,
    overlayId: overlay.overlayId,
    order: videoLayers.length + index,
    position: overlay.position
  }));

  return withRenderPlanId({
    sceneId: input.sceneGraph?.sceneId,
    outputProfile: input.outputProfile,
    colorGrade: input.colorGrade,
    sourceCount: input.sources.length,
    resolvedRouteCount: routes.filter((route) => route.status === "resolved").length,
    layers: [...videoLayers, ...overlayLayers],
    routes,
    warnings: [...new Set(warnings)]
  });
}

function withRenderPlanId(renderPlan: Omit<NativeMediaCoreRenderPlan, "renderPlanId">): NativeMediaCoreRenderPlan {
  const stableInput = JSON.stringify({
    sceneId: renderPlan.sceneId,
    outputProfile: renderPlan.outputProfile,
    colorGrade: renderPlan.colorGrade,
    routes: renderPlan.routes.map((route) => ({
      routeId: route.routeId,
      mode: route.mode,
      audioRole: route.audioRole,
      sourceId: route.sourceId,
      participantId: route.participantId,
      kind: route.kind,
      status: route.status,
      fitMode: route.fitMode,
      sourceScale: route.sourceScale,
      sourceOffsetX: route.sourceOffsetX,
      sourceOffsetY: route.sourceOffsetY,
      ptz: route.ptz
    })),
    layers: renderPlan.layers.map((layer) => ({
      layerId: layer.layerId,
      kind: layer.kind,
      sourceId: layer.sourceId,
      participantId: layer.participantId,
      overlayId: layer.overlayId,
      position: layer.position,
      order: layer.order,
      fitMode: layer.fitMode,
      sourceScale: layer.sourceScale,
      sourceOffsetX: layer.sourceOffsetX,
      sourceOffsetY: layer.sourceOffsetY,
      ptz: layer.ptz
    }))
  });

  return {
    ...renderPlan,
    renderPlanId: `rp-${stableHash(stableInput)}`
  };
}

function stableHash(value: string) {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(16).padStart(8, "0");
}

function resolveRoute(
  route: SceneGraph["routes"][number],
  sources: NativeMediaCoreZoomSource[],
  activeSpeakerId?: string,
  screenShareParticipantId?: string
): NativeMediaCoreResolvedRoute {
  if (route.mode === "none") {
    return { routeId: route.routeId, mode: route.mode, audioRole: route.audioRole, status: "disabled" };
  }

  if (route.mode === "screen-share") {
    const source = screenShareParticipantId
      ? sources.find((candidate) => candidate.participantId === screenShareParticipantId && candidate.isScreenSharing)
      : sources.find((candidate) => candidate.isScreenSharing);

    return source
      ? resolved(route, source, "screen-share")
      : missing(route, "Screen share route requested but no active screen share source is available.");
  }

  const participantId = route.mode === "active-speaker" ? activeSpeakerId : route.participantId;
  if (!participantId) {
    return missing(route, `${route.mode} route has no participant assignment.`);
  }

  const source = sources.find((candidate) => candidate.participantId === participantId);
  if (!source) {
    return missing(route, `${participantId} is not present in the Zoom source roster.`);
  }

  if (!source.hasVideo || source.health === "video-off") {
    return missing(route, `${source.displayName} has no clean video feed.`);
  }

  return resolved(route, source, "participant-video");
}

function resolved(route: SceneGraph["routes"][number], source: NativeMediaCoreZoomSource, kind: "participant-video" | "screen-share"): NativeMediaCoreResolvedRoute {
  return {
    routeId: route.routeId,
    mode: route.mode,
    audioRole: route.audioRole,
    sourceId: kind === "screen-share" ? `screen-share:${source.participantId}` : source.sourceId,
    participantId: source.participantId,
    kind,
    fitMode: route.fitMode,
    ...normalizeRouteFraming(route),
    status: "resolved"
  };
}

function missing(route: SceneGraph["routes"][number], warning: string): NativeMediaCoreResolvedRoute {
  return {
    routeId: route.routeId,
    mode: route.mode,
    audioRole: route.audioRole,
    status: "missing",
    warning
  };
}

function normalizeRouteFraming(route: SceneGraph["routes"][number]) {
  const sourceScale = normalizeNumber(route.ptz?.zoom ?? route.sourceScale, 1, 0.25, 4);
  const sourceOffsetX = normalizeNumber(route.ptz?.pan ?? route.sourceOffsetX, 0, -1, 1);
  const sourceOffsetY = normalizeNumber(route.ptz?.tilt ?? route.sourceOffsetY, 0, -1, 1);
  return {
    sourceScale,
    sourceOffsetX,
    sourceOffsetY,
    ptz: {
      zoom: sourceScale,
      pan: sourceOffsetX,
      tilt: sourceOffsetY
    }
  };
}

function normalizeNumber(value: number | undefined, fallback: number, min: number, max: number) {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return fallback;
  }

  return Math.min(max, Math.max(min, Math.round(value * 1000) / 1000));
}
