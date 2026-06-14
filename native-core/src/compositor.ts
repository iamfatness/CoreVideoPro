import type { MediaCoreCompositorState, MediaCoreProgramFrame, MediaCoreRenderPlan } from "./protocol.js";

export class ProgramCompositor {
  private frameNumber = 0;
  private droppedFrameCount = 0;
  private degradedFrameCount = 0;
  private renderPlanId?: string;
  private lastReconfigureReason?: string;
  private lastFrame?: MediaCoreProgramFrame;

  compose(renderPlan: MediaCoreRenderPlan, timestampMs: number): MediaCoreProgramFrame | undefined {
    if (!renderPlan.sceneId) {
      this.lastFrame = undefined;
      return undefined;
    }

    if (this.renderPlanId !== renderPlan.renderPlanId) {
      this.lastReconfigureReason = this.renderPlanId
        ? `Render plan changed from ${this.renderPlanId} to ${renderPlan.renderPlanId}.`
        : `Initial render plan ${renderPlan.renderPlanId}.`;
      this.renderPlanId = renderPlan.renderPlanId;
    }

    this.frameNumber += 1;
    const hasMissingRoutes = renderPlan.routes.some((route) => route.status === "missing");
    const health = this.frameNumber % 120 === 0 ? "dropped" : hasMissingRoutes || renderPlan.warnings.length > 0 ? "degraded" : "live";
    if (health === "dropped") {
      this.droppedFrameCount += 1;
    }
    if (health === "degraded") {
      this.degradedFrameCount += 1;
    }

    this.lastFrame = {
      frameNumber: this.frameNumber,
      timestampMs,
      renderPlanId: renderPlan.renderPlanId,
      sceneId: renderPlan.sceneId,
      width: renderPlan.outputProfile.width,
      height: renderPlan.outputProfile.height,
      fps: renderPlan.outputProfile.fps,
      layerCount: renderPlan.layers.length,
      colorGrade: renderPlan.colorGrade,
      health,
      warning: health === "degraded" ? renderPlan.warnings[0] ?? "Program frame degraded by incomplete render plan." : undefined
    };

    return this.lastFrame;
  }

  snapshot(): MediaCoreCompositorState {
    const status = this.lastFrame
      ? this.lastFrame.health === "dropped"
        ? "degraded"
        : this.lastFrame.health
      : "idle";

    return {
      status,
      renderPlanId: this.renderPlanId,
      programFrameCount: this.frameNumber,
      droppedFrameCount: this.droppedFrameCount,
      degradedFrameCount: this.degradedFrameCount,
      lastReconfigureReason: this.lastReconfigureReason,
      lastFrame: this.lastFrame ? { ...this.lastFrame, colorGrade: { ...this.lastFrame.colorGrade } } : undefined
    };
  }
}

export function withRenderPlanId(renderPlan: Omit<MediaCoreRenderPlan, "renderPlanId">): MediaCoreRenderPlan {
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
      status: route.status
    })),
    layers: renderPlan.layers.map((layer) => ({
      layerId: layer.layerId,
      kind: layer.kind,
      sourceId: layer.sourceId,
      participantId: layer.participantId,
      overlayId: layer.overlayId,
      position: layer.position,
      order: layer.order
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
