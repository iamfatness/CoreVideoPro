import { describe, expect, it } from "vitest";
import { AudioMixSessionModel } from "./audioMixSession.js";

describe("AudioMixSessionModel", () => {
  it("returns an idle snapshot when no channels are synced", () => {
    const model = new AudioMixSessionModel();

    expect(model.snapshot()).toMatchObject({
      status: "idle",
      masterLevel: 0,
      loudnessLufs: -60,
      summary: "Audio mix idle.",
      participants: []
    });
  });

  it("balances participant levels and reports boosting or ducking", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync([
      { participantId: "p1", inputLevel: 18, muted: false, noiseSuppression: false },
      { participantId: "p2", inputLevel: 82, muted: false, noiseSuppression: false, manualGainDb: -2 }
    ]);

    expect(snapshot.status).toBe("warning");
    expect(snapshot.participants).toHaveLength(2);
    expect(snapshot.participants[0]?.status).toBe("boosting");
    expect(snapshot.participants[1]?.status).toBe("ducking");
    expect(snapshot.summary).toContain("boosted");
    expect(snapshot.summary).toContain("ducked");
  });
});