import { describe, expect, it } from "vitest";
import type { OutputLifecycle } from "./generated/lifecycle";
import { recordingReadModel } from "./recordingReadModel";

const optimisticLegacy = { active: true, status: "recording" as const, totalFramesWritten: 0 };
const lifecycle = (state: OutputLifecycle["state"]): OutputLifecycle => ({
  sessionId: "take-1", desiredActive: true, state, health: "healthy", finalized: false
});

describe("recordingReadModel", () => {
  it("keeps requested activity separate from confirmed writer output", () => {
    expect(recordingReadModel(undefined, true)).toMatchObject({ active: false, status: "starting" });
    expect(recordingReadModel(optimisticLegacy, true)).toMatchObject({ active: false, status: "starting" });
    expect(recordingReadModel({ ...optimisticLegacy, totalFramesWritten: 1 })).toMatchObject({ active: true });
  });

  it.each(["starting", "stopping", "finalizing", "failed", "interrupted"] as const)(
    "does not let legacy active or old frame counts override %s", (state) => {
      expect(recordingReadModel({ ...optimisticLegacy, totalFramesWritten: 999, lifecycle: lifecycle(state) }))
        .toMatchObject({ active: false, status: state, finalized: false });
    }
  );

  it("preserves degraded live recording and requires an explicit completion result", () => {
    expect(recordingReadModel({ ...optimisticLegacy, lifecycle: { ...lifecycle("live"), health: "degraded" } }))
      .toMatchObject({ active: true, status: "warning" });
    expect(recordingReadModel({ ...optimisticLegacy, lifecycle: lifecycle("completed") })).toMatchObject({ finalized: false });
    expect(recordingReadModel({ ...optimisticLegacy, lifecycle: { ...lifecycle("completed"), finalized: true } }))
      .toMatchObject({ active: false, finalized: true });
  });

  it("fails closed for unknown lifecycle data", () => {
    expect(recordingReadModel({ ...optimisticLegacy, lifecycle: { ...lifecycle("live"), state: "future-state" } as unknown as OutputLifecycle }))
      .toMatchObject({ active: false, status: "failed", finalized: false });
  });

  it("does not show live recording when lifecycle health is unknown", () => {
    expect(recordingReadModel({
      ...optimisticLegacy, totalFramesWritten: 999,
      lifecycle: { ...lifecycle("live"), health: "unknown" }
    })).toMatchObject({ active: false, status: "starting", finalized: false });
  });
});
