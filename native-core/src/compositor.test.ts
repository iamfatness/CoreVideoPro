import { describe, expect, it } from "vitest";
import { ProgramCompositor } from "./compositor.js";
import { buildRenderPlan } from "./renderPlan.js";
import type { MediaCoreCommand, MediaCoreColorGrade, MediaCoreOutputProfile, MediaCoreZoomSource } from "./protocol.js";

const outputProfile: MediaCoreOutputProfile = {
  profileId: "1080p60",
  resolution: "1920x1080",
  width: 1920,
  height: 1080,
  fps: 60,
  targetBitrateMbps: 8.2
};

const colorGrade: MediaCoreColorGrade = {
  lut: "none",
  exposure: 0,
  contrast: 0,
  saturation: 0,
  temperature: 0
};

const sources: MediaCoreZoomSource[] = [
  {
    sourceId: "participant:p1",
    participantId: "p1",
    displayName: "Sophia Martinez",
    role: "Host",
    breakoutRoomId: "main",
    breakoutRoomName: "Main room",
    hasVideo: true,
    hasAudio: true,
    isMuted: false,
    isActiveSpeaker: true,
    isScreenSharing: false,
    audioLevel: 64,
    health: "live"
  }
];

const sceneGraph: Extract<MediaCoreCommand, { type: "load-scene-graph" }> = {
  type: "load-scene-graph",
  sceneId: "interview",
  routes: [{ routeId: "active", mode: "active-speaker", audioRole: "mix" }]
};

describe("ProgramCompositor", () => {
  it("generates stable render plan ids from meaningful compositor inputs", () => {
    const first = buildRenderPlan({
      sceneGraph,
      sources,
      activeSpeakerId: "p1",
      overlays: [],
      outputProfile,
      colorGrade
    });
    const same = buildRenderPlan({
      sceneGraph,
      sources,
      activeSpeakerId: "p1",
      overlays: [],
      outputProfile,
      colorGrade
    });
    const changed = buildRenderPlan({
      sceneGraph,
      sources,
      activeSpeakerId: "p1",
      overlays: [],
      outputProfile,
      colorGrade: { ...colorGrade, lut: "warm-film" }
    });

    expect(first.renderPlanId).toBe(same.renderPlanId);
    expect(first.renderPlanId).not.toBe(changed.renderPlanId);
  });

  it("reconfigures when the render plan id changes and marks incomplete plans degraded", () => {
    const compositor = new ProgramCompositor();
    const livePlan = buildRenderPlan({
      sceneGraph,
      sources,
      activeSpeakerId: "p1",
      overlays: [],
      outputProfile,
      colorGrade
    });
    const degradedPlan = buildRenderPlan({
      sceneGraph,
      sources: [],
      activeSpeakerId: "p1",
      overlays: [],
      outputProfile,
      colorGrade
    });

    expect(compositor.compose(livePlan, 0)).toMatchObject({
      frameNumber: 1,
      renderPlanId: livePlan.renderPlanId,
      health: "live"
    });
    expect(compositor.compose(degradedPlan, 33)).toMatchObject({
      frameNumber: 2,
      renderPlanId: degradedPlan.renderPlanId,
      health: "degraded"
    });
    expect(compositor.snapshot()).toMatchObject({
      status: "degraded",
      programFrameCount: 2,
      degradedFrameCount: 1,
      lastReconfigureReason: `Render plan changed from ${livePlan.renderPlanId} to ${degradedPlan.renderPlanId}.`
    });
  });
});
