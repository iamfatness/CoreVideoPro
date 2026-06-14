import { describe, expect, it } from "vitest";
import { initialParticipants } from "../domain/production";
import { buildSmartAudioMix, SimulatedAudioMixEngine } from "./audioMix";

describe("buildSmartAudioMix", () => {
  it("balances loud, quiet, and muted participants", () => {
    const mix = buildSmartAudioMix(initialParticipants);

    expect(mix.participants.find((participant) => participant.participantId === "p2")?.status).toBe("ducking");
    expect(mix.participants.find((participant) => participant.participantId === "p4")?.status).toBe("boosting");
    expect(mix.participants.find((participant) => participant.participantId === "p3")?.muted).toBe(true);
    expect(mix.summary).toContain("boosted");
    expect(mix.summary).toContain("ducked");
    expect(mix.masterLevel).toBeGreaterThan(0);
  });

  it("allows producer mute overrides", async () => {
    const engine = new SimulatedAudioMixEngine();
    const mix = await engine.setParticipantMuted("p2", true, initialParticipants);

    const presenterMix = mix.participants.find((participant) => participant.participantId === "p2");

    expect(presenterMix?.muted).toBe(true);
    expect(presenterMix?.outputLevel).toBe(0);
    expect(presenterMix?.status).toBe("muted");
  });

  it("allows producer gain overrides and keeps them across mix rebuilds", async () => {
    const engine = new SimulatedAudioMixEngine();
    const boosted = await engine.setParticipantGain("p4", 4, initialParticipants);
    const rebuilt = await engine.buildMix(initialParticipants);

    const boostedMix = boosted.participants.find((participant) => participant.participantId === "p4");
    const rebuiltMix = rebuilt.participants.find((participant) => participant.participantId === "p4");

    expect(boostedMix?.manualGainDb).toBe(4);
    expect(boostedMix?.gainDb).toBe(7);
    expect(boostedMix?.outputLevel).toBeGreaterThan(65);
    expect(boosted.summary).toContain("manual");
    expect(rebuiltMix?.manualGainDb).toBe(4);
  });
});
