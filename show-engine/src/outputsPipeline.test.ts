import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  deriveTally,
  GalleryDirector,
  LiveSlots,
  OverlayDirector,
  OverrideDb,
  ProgramBus,
  clampPage,
  parseHandsPayload,
  parseMukanaQuestion,
  resolveLook,
  stripChairs,
  tallyEquals,
  ZoomIngest,
  type LookDefinition,
  type MukanaDb,
  type Participant
} from "./index.js";

function participant(participantId: string, rawName: string): Participant {
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3
  };
}

const mukana: MukanaDb = {
  "1383": { pin: "1383", displayName: "J.J. Mc Kenna", location: "CA", role: "host", online: true },
  "2001": { pin: "2001", displayName: "Reader Rose", location: "NY", role: "reader", online: true },
  "4242": { pin: "4242", displayName: "Ann Lee", location: "TX", role: "panelist", online: true },
  "5555": { pin: "5555", displayName: "Bo Diaz", location: "PE", role: "panelist", online: true },
  "7777": {
    pin: "7777",
    displayName: "Sam Signer",
    location: "WA",
    role: "aslinterpreter",
    online: true
  }
};

function seatedRoster(): LiveSlots {
  const ingest = new ZoomIngest();
  for (const [id, pin] of [
    ["z-ann", "4242"],
    ["z-asl", "7777"],
    ["z-bo", "5555"],
    ["z-host", "1383"],
    ["z-reader", "2001"]
  ] as const) {
    ingest.apply({ kind: "joined", participant: participant(id, `Name | ${pin}`) });
  }
  ingest.commit();
  const slots = new LiveSlots({ capacity: 8, utilityPinBase: 9000 });
  slots.rebuild([...buildPanelistDb(ingest.snapshot(), mukana, new OverrideDb().entries()).values()]);
  return slots;
}

const teatime: LookDefinition = {
  id: "teatime",
  label: "Teatime",
  scenePreset: "scene-teatime",
  boxes: 2,
  includesHost: true,
  includesReader: true,
  plateTone: "accent",
  tallySource: "boxes",
  boxFill: "queue"
};

describe("outputs pipeline", () => {
  it("puts exactly the people a look shows on air", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const queue = stripChairs(parsed.queue, { hostPin: "1383", readerPin: "2001" });
    const look = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });
    const tally = deriveTally({
      source: { kind: "look", lookId: "teatime" },
      slots: slots.slots(),
      gallery: [],
      look,
      activeSpeakerSlot: null
    });

    expect(tally.onAirPins.sort()).toEqual(["1383", "2001", "4242", "5555"]);
    expect(tally.onAirPins).not.toContain("7777");
  });

  it("keeps the ASL interpreter off air and out of the gate", () => {
    const slots = seatedRoster();
    const aslSlot = slots.slotOf("z-asl");
    const asl = aslSlot === null ? null : slots.slots()[aslSlot - 1]?.panelist ?? null;
    expect(asl?.role).toBe("aslinterpreter");

    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    bus.onActiveSpeaker("z-ann", "panelist");
    bus.onActiveSpeaker("z-asl", asl?.role ?? "panelist");

    // The property that actually breaks if ProgramBus stops gating the
    // interpreter: the bus's own active-speaker id must still be the last
    // panelist who was allowed through, not the interpreter who just spoke.
    expect(bus.state().activeSpeakerId).toBe("z-ann");

    const activeSpeakerId = bus.state().activeSpeakerId;
    const activeSpeakerSlot = activeSpeakerId === null ? null : slots.slotOf(activeSpeakerId);

    const tally = deriveTally({
      source: bus.state().program,
      slots: slots.slots(),
      gallery: [],
      look: null,
      activeSpeakerSlot
    });
    expect(tally.onAirParticipantIds).toEqual(["z-ann"]);
  });

  it("labels exactly the people it puts on air", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const queue = stripChairs(parsed.queue, { hostPin: "1383", readerPin: "2001" });
    const look = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });

    const overlay = new OverlayDirector();
    overlay.update({ look, question: null, questionVisible: false });

    const tally = deriveTally({
      source: { kind: "look", lookId: "teatime" },
      slots: slots.slots(),
      gallery: [],
      look,
      activeSpeakerSlot: null
    });

    expect(overlay.state().nameplates.map((plate) => plate.slot).sort()).toEqual(
      [...tally.onAirSlots].sort()
    );
  });

  it("survives a queue that shrinks under a paged look", () => {
    const slots = seatedRoster();
    const full = parseHandsPayload("5555,7777,1383\n4242\nNONE");
    expect(full.kind).toBe("data");
    if (full.kind !== "data") return;

    const page = clampPage(teatime, full.queue, 1);
    expect(() => resolveLook(teatime, { queue: full.queue, slots: slots.slots(), page })).not.toThrow();

    const drained = parseHandsPayload("NONE\n4242\nNONE");
    expect(drained.kind).toBe("data");
    if (drained.kind !== "data") return;

    const clamped = clampPage(teatime, drained.queue, page);
    expect(clamped).toBe(0);
    expect(() =>
      resolveLook(teatime, { queue: drained.queue, slots: slots.slots(), page: clamped })
    ).not.toThrow();
  });

  it("reports a tally change only when who is live actually changes", () => {
    const slots = seatedRoster();
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots.slots());

    const first = deriveTally({
      source: { kind: "gallery" },
      slots: slots.slots(),
      gallery: gallery.cells(),
      look: null,
      activeSpeakerSlot: null
    });
    const again = deriveTally({
      source: { kind: "gallery" },
      slots: slots.slots(),
      gallery: gallery.cells(),
      look: null,
      activeSpeakerSlot: null
    });
    expect(tallyEquals(first, again)).toBe(true);

    gallery.remove(1);
    const after = deriveTally({
      source: { kind: "gallery" },
      slots: slots.slots(),
      gallery: gallery.cells(),
      look: null,
      activeSpeakerSlot: null
    });
    expect(tallyEquals(first, after)).toBe(false);
  });

  it("carries a parsed question through to the overlay", () => {
    const outcome = parseMukanaQuestion(
      JSON.stringify({ q: { n: "Douglas", q: "Why?", tag: "General", v: 2, ts: 1, key: "k" } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;

    const overlay = new OverlayDirector();
    expect(overlay.update({ look: null, question: outcome.question, questionVisible: true })).toBe(
      true
    );
    expect(overlay.state().question).toEqual({
      askerName: "Douglas",
      text: "Why?",
      tag: "General",
      votes: 2
    });
  });
});
