import { describe, expect, it } from "vitest";
import { handleLine } from "./service.js";

describe("native-core service", () => {
  it("handles sync requests from a JSON line", () => {
    const response = handleLine(
      JSON.stringify({
        id: "line-1",
        type: "sync",
        commands: [
          {
            type: "load-scene-graph",
            sceneId: "interview",
            routes: [{ routeId: "guest", mode: "active-speaker", audioRole: "mix" }]
          }
        ]
      })
    );

    expect(response).toMatchObject({
      id: "line-1",
      ok: true,
      appliedCommandCount: 1,
      state: {
        sceneId: "interview",
        routeCount: 1
      }
    });
  });

  it("rejects malformed JSON lines", () => {
    expect(handleLine("{")).toMatchObject({
      id: "unknown",
      ok: false,
      error: {
        code: "invalid-request"
      }
    });
  });

  it("handles tick requests from a JSON line", () => {
    handleLine(
      JSON.stringify({
        id: "line-2",
        type: "sync",
        commands: [
          {
            type: "load-scene-graph",
            sceneId: "panel",
            routes: [{ routeId: "active", mode: "active-speaker", audioRole: "mix" }]
          }
        ]
      })
    );
    const response = handleLine(JSON.stringify({ id: "tick-1", type: "tick", elapsedMs: 100 }));

    expect(response).toMatchObject({
      id: "tick-1",
      ok: true,
      appliedCommandCount: 0,
      state: {
        frameCount: 1
      }
    });
  });
});
