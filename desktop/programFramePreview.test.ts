import { describe, expect, it } from "vitest";
import { dispatchCoreRequest } from "./coreStub.ts";
import {
  PROGRAM_FRAME_PREVIEW_MAX_HEIGHT,
  PROGRAM_FRAME_PREVIEW_MAX_WIDTH,
  colorFromParticipantId,
  computeProgramFramePreviewSize,
  fillSyntheticProgramFramePreview,
  programFramePreviewEventFromSnapshot
} from "./programFramePreview.ts";
import type { NativeMediaCoreCommand } from "../src/engine/nativeMediaCoreProtocol";
import { synthesizeSnapshot } from "./syntheticMediaCore.ts";

describe("programFramePreview", () => {
  it("downscales to the native 320x180 preview cap", () => {
    expect(computeProgramFramePreviewSize(1920, 1080)).toEqual({ width: 320, height: 180 });
    expect(computeProgramFramePreviewSize(640, 480)).toEqual({ width: 240, height: 180 });
  });

  it("fills a synthetic preview for a live program frame", () => {
    const commands: NativeMediaCoreCommand[] = [
      {
        type: "load-scene-graph",
        sceneId: "preview-test",
        routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "speaker-1" }]
      },
      { type: "start-program-output", destinations: ["recording"], isoParticipantIds: [] }
    ];
    const snapshot = synthesizeSnapshot(commands, 1000, 3);
    const preview = fillSyntheticProgramFramePreview(snapshot.renderPlan, snapshot.frames, snapshot.programFrame!);
    expect(preview).toBeDefined();
    expect(preview!.width).toBeLessThanOrEqual(PROGRAM_FRAME_PREVIEW_MAX_WIDTH);
    expect(preview!.height).toBeLessThanOrEqual(PROGRAM_FRAME_PREVIEW_MAX_HEIGHT);
    expect(preview!.bgra.length).toBe(preview!.width * preview!.height * 4);
  });

  it("matches native participant tint hashing", () => {
    expect(colorFromParticipantId("speaker-1")).toBe(colorFromParticipantId("speaker-1"));
    expect(colorFromParticipantId("speaker-1")).not.toBe(colorFromParticipantId("speaker-2"));
  });

  it("emits a program-frame-preview event after media-core-sync", () => {
    const dispatch = dispatchCoreRequest(
      JSON.stringify({
        id: "core-1",
        type: "media-core-sync",
        elapsedMs: 1000,
        commands: [
          {
            type: "load-scene-graph",
            sceneId: "preview-test",
            routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "speaker-1" }]
          },
          { type: "start-program-output", destinations: ["recording"], isoParticipantIds: [] }
        ]
      })
    );

    expect(dispatch?.response.ok).toBe(true);
    const previewEvent = dispatch?.events?.find((event) => event.type === "program-frame-preview");
    expect(previewEvent).toBeDefined();
    expect(previewEvent?.preview.pixelFormat).toBe("bgra");
    expect(previewEvent?.preview.bgraBase64.length).toBeGreaterThan(0);
    expect(programFramePreviewEventFromSnapshot(synthesizeSnapshot([], 0, 1))).toBeUndefined();
  });
});