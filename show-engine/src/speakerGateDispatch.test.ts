// show-engine/src/speakerGateDispatch.test.ts
import { describe, expect, it } from "vitest";
import { ShowEngine } from "./showEngine.js";
import { MockHost } from "./mockHost.js";
import { StateStore } from "./persistence.js";
import { parseShowEngineConfig } from "./config.js";
import { FiloAssigner, VisibleSetAssigner } from "./speakerRecency.js";
import { ROLES } from "./contracts.js";
import type { PlacementChange, PositionAssigner } from "./speakerRecency.js";
import type { OverrideRecord } from "./overrideDb.js";
import type { Role } from "./contracts.js";
import type { StateFs } from "./persistence.js";
import type { ZoomEvent } from "./zoomIngest.js";

/** Wraps a real assigner and records every dispatch that reaches it. */
class RecordingAssigner implements PositionAssigner {
  readonly seen: string[] = [];
  readonly changes: PlacementChange[] = [];
  constructor(private readonly inner: PositionAssigner) {}
  onActiveSpeaker(participantId: string): PlacementChange[] {
    this.seen.push(participantId);
    const out = this.inner.onActiveSpeaker(participantId);
    this.changes.push(...out);
    return out;
  }
  positions(): Map<number, string> {
    return this.inner.positions();
  }
  reset(participantIds: readonly string[]): void {
    this.inner.reset(participantIds);
  }
}

function memoryFs(): StateFs {
  const files = new Map<string, string>();
  return {
    readFile: async (p) => {
      const v = files.get(p);
      if (v === undefined) throw new Error(`ENOENT ${p}`);
      return v;
    },
    writeFile: async (p, c) => void files.set(p, c),
    rename: async (from, to) => {
      const v = files.get(from);
      if (v !== undefined) {
        files.set(to, v);
        files.delete(from);
      }
    },
    mkdir: async () => undefined
  };
}

const config = parseShowEngineConfig({
  capacity: 8,
  statePath: "/state/show.json",
  looks: [
    {
      id: "teatime",
      label: "Teatime",
      scenePreset: "scene-teatime",
      boxes: 2,
      includesHost: true,
      includesReader: false
    }
  ]
});

function joined(id: string, name: string): ZoomEvent {
  return {
    kind: "joined",
    participant: {
      participantId: id,
      rawName: name,
      online: true,
      videoOn: true,
      audioOn: true,
      handRaised: false,
      zoomRole: 0
    }
  };
}

function override(personKey: string, role: Role): OverrideRecord {
  return { personKey, displayName: "Ann", location: "", role };
}

function build(assigner: PositionAssigner) {
  return new ShowEngine({
    config,
    host: new MockHost(),
    clock: { now: () => 1000 },
    store: new StateStore(config.statePath, { fs: memoryFs() }),
    assigner
  });
}

/**
 * THE PLAN 3 OBLIGATION, discharged as a property.
 *
 * Quantified over, deliberately — a property that holds only at one point is
 * how Plan 4's degradation bug survived 404 tests:
 *   - every role in ROLES (5), so adding a role to skipRoles cannot silently
 *     escape coverage;
 *   - both shipped assigner types, since FILO evicts and VisibleSet swaps and
 *     the bug looks different in each;
 *   - active-speaker follow both on and off, since the gate must not be
 *     accidentally implemented as a side effect of the follow flag.
 *
 * The invariant it must break on: any dispatch to a POSITION ASSIGNER for a
 * role in skipRoles. If the engine's gate is removed, or reordered below the
 * dispatch, `recorder.seen` is non-empty for "aslinterpreter" in every
 * combination and this fails.
 *
 * Honest scoping of the ProgramBus assertion below: it is CONFIRMATORY, not
 * independent. `ProgramBus.onActiveSpeaker` (programBus.ts:76-79) applies
 * `shouldFollowSpeaker` internally before touching `activeSpeakerId`, so that
 * half would still pass with the engine's gate entirely absent. The assigner
 * assertions are the real proof — ProgramBus's internal gate does not protect
 * a position assigner, which is exactly why the engine needs its own. Keep
 * both, but do not mistake the ProgramBus half for a second guarantee.
 */
describe("active-speaker dispatch gate", () => {
  const assigners: Array<[string, () => PositionAssigner]> = [
    ["FiloAssigner", () => new FiloAssigner({ capacity: 4 })],
    ["VisibleSetAssigner", () => new VisibleSetAssigner({ capacity: 8, visible: 4 })]
  ];

  for (const [assignerName, make] of assigners) {
    for (const role of ROLES) {
      for (const follow of [true, false]) {
        const skipped = config.skipRoles.includes(role);
        const verb = skipped ? "blocks" : "dispatches";

        it(`${verb} a ${role} speaker with ${assignerName}, follow=${follow}`, async () => {
          const recorder = new RecordingAssigner(make());
          const e = build(recorder);
          e.onZoomEvent(joined("p1", "Ann"));
          e.onZoomEvent(joined("p2", "Bo"));
          await e.tick();
          const key = e.snapshot().panelists.find((p) => p.participantId === "p1")?.personKey;
          expect(key).toBeDefined();
          if (key !== undefined) e.setOverride(override(key, role));
          e.setActiveSpeakerFollow(follow);
          await e.tick();

          recorder.seen.length = 0;
          recorder.changes.length = 0;
          e.onActiveSpeaker("p1");
          const snap = await e.tick();

          if (skipped) {
            expect(recorder.seen).toEqual([]);
            expect(recorder.changes).toEqual([]);
            expect(snap.program.activeSpeakerId).toBeNull();
          } else {
            expect(recorder.seen).toEqual(["p1"]);
            expect(snap.program.activeSpeakerId).toBe("p1");
          }
        });
      }
    }
  }

  it("follows a speaker whose role is unknown to the roster", async () => {
    const recorder = new RecordingAssigner(new FiloAssigner({ capacity: 4 }));
    const e = build(recorder);
    e.onActiveSpeaker("ghost");
    await e.tick();
    expect(recorder.seen).toEqual(["ghost"]);
  });

  it("drops a pending speaker after one tick rather than replaying it", async () => {
    const recorder = new RecordingAssigner(new FiloAssigner({ capacity: 4 }));
    const e = build(recorder);
    e.onZoomEvent(joined("p1", "Ann"));
    e.onActiveSpeaker("p1");
    await e.tick();
    recorder.seen.length = 0;
    await e.tick();
    expect(recorder.seen).toEqual([]);
  });

  it("keeps only the latest speaker when several arrive within one tick", async () => {
    const recorder = new RecordingAssigner(new FiloAssigner({ capacity: 4 }));
    const e = build(recorder);
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    recorder.seen.length = 0;
    e.onActiveSpeaker("p1");
    e.onActiveSpeaker("p2");
    await e.tick();
    expect(recorder.seen).toEqual(["p2"]);
  });
});
