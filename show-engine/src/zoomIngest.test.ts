import { describe, expect, it } from "vitest";
import { ZoomIngest } from "./zoomIngest.js";
import type { Participant } from "./contracts.js";

function participant(overrides: Partial<Participant> & { participantId: string }): Participant {
  return {
    rawName: "Guest",
    online: true,
    videoOn: false,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    ...overrides
  };
}

describe("ZoomIngest", () => {
  it("publishes nothing before the first commit", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    expect(ingest.snapshot()).toEqual([]);
    expect(ingest.dirty).toBe(true);
  });

  it("publishes the working set on commit", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    expect(ingest.commit()).toBe(true);
    expect(ingest.snapshot().map((p) => p.participantId)).toEqual(["a"]);
    expect(ingest.dirty).toBe(false);
  });

  it("reports no change when committing twice", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    expect(ingest.commit()).toBe(false);
  });

  it("orders the snapshot by participant id for stable downstream diffing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "c" }) });
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.apply({ kind: "joined", participant: participant({ participantId: "b" }) });
    ingest.commit();
    expect(ingest.snapshot().map((p) => p.participantId)).toEqual(["a", "b", "c"]);
  });

  it("replaces the whole roster on a roster event", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    ingest.apply({
      kind: "roster",
      participants: [participant({ participantId: "b" }), participant({ participantId: "c" })]
    });
    ingest.commit();
    expect(ingest.snapshot().map((p) => p.participantId)).toEqual(["b", "c"]);
  });

  it("applies video, audio, hand and rename events", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.apply({ kind: "video", participantId: "a", on: true });
    ingest.apply({ kind: "audio", participantId: "a", on: true });
    ingest.apply({ kind: "hand", participantId: "a", raised: true });
    ingest.apply({ kind: "renamed", participantId: "a", rawName: "Ann | 1383" });
    ingest.commit();
    const [first] = ingest.snapshot();
    expect(first).toMatchObject({
      videoOn: true,
      audioOn: true,
      handRaised: true,
      rawName: "Ann | 1383"
    });
  });

  it("keeps a departed participant but marks them offline with video off", () => {
    const ingest = new ZoomIngest();
    ingest.apply({
      kind: "joined",
      participant: participant({ participantId: "a", videoOn: true })
    });
    ingest.apply({ kind: "left", participantId: "a" });
    ingest.commit();
    expect(ingest.snapshot()[0]).toMatchObject({ online: false, videoOn: false });
  });

  it("ignores events for unknown participants", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "video", participantId: "ghost", on: true });
    expect(ingest.commit()).toBe(false);
    expect(ingest.snapshot()).toEqual([]);
  });

  it("does not mark dirty when an event changes nothing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({
      kind: "joined",
      participant: participant({ participantId: "a", videoOn: true })
    });
    ingest.commit();
    ingest.apply({ kind: "video", participantId: "a", on: true });
    expect(ingest.dirty).toBe(false);
  });

  it("isolates published snapshot from internal mutations", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a", videoOn: false }) });
    ingest.commit();
    const snapshot1 = ingest.snapshot();
    // Mutate the snapshot object
    snapshot1[0].videoOn = true;
    snapshot1[0].rawName = "Hacked";
    // The next snapshot should be unaffected
    expect(ingest.snapshot()[0]).toMatchObject({ videoOn: false, rawName: "Guest" });
  });

  it("does not mark dirty when a duplicate joined event has identical data", () => {
    const ingest = new ZoomIngest();
    const p = participant({ participantId: "a", videoOn: true, audioOn: true });
    ingest.apply({ kind: "joined", participant: p });
    ingest.commit();
    ingest.apply({ kind: "joined", participant: p });
    expect(ingest.dirty).toBe(false);
    expect(ingest.commit()).toBe(false);
  });

  it("does not mark dirty when a duplicate roster event has identical participants", () => {
    const ingest = new ZoomIngest();
    const roster = [participant({ participantId: "a" }), participant({ participantId: "b" })];
    ingest.apply({ kind: "roster", participants: roster });
    ingest.commit();
    ingest.apply({ kind: "roster", participants: roster });
    expect(ingest.dirty).toBe(false);
    expect(ingest.commit()).toBe(false);
  });

  it("marks dirty when a roster event has genuine changes", () => {
    const ingest = new ZoomIngest();
    ingest.apply({
      kind: "roster",
      participants: [participant({ participantId: "a", videoOn: false })]
    });
    ingest.commit();
    ingest.apply({
      kind: "roster",
      participants: [participant({ participantId: "a", videoOn: true })]
    });
    expect(ingest.dirty).toBe(true);
    expect(ingest.commit()).toBe(true);
  });
});

describe("ZoomIngest revision", () => {
  it("starts at zero", () => {
    expect(new ZoomIngest().revision).toBe(0);
  });

  it("increments once per publishing commit", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    expect(ingest.revision).toBe(1);

    ingest.apply({ kind: "video", participantId: "a", on: true });
    ingest.commit();
    expect(ingest.revision).toBe(2);
  });

  it("does not move when a commit publishes nothing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    const before = ingest.revision;
    expect(ingest.commit()).toBe(false);
    expect(ingest.revision).toBe(before);
  });

  it("does not move for an event that changes nothing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({
      kind: "joined",
      participant: participant({ participantId: "a", videoOn: true })
    });
    ingest.commit();
    const before = ingest.revision;
    ingest.apply({ kind: "video", participantId: "a", on: true });
    ingest.commit();
    expect(ingest.revision).toBe(before);
  });
});
