/**
 * `HostLoop` — the pure (no `process`, no timers) request/response/event
 * pump `main.ts` drives over stdio. Built the way `actionsPipeline.test.ts`
 * builds its rig (`memoryFs`, `CONFORMANCE_CONFIG`, an injected clock), but
 * with `StdioHostAdapter` as the engine's `HostAdapter` so a tick's
 * `hostCommand` lines land in the SAME `lines` array the loop's own
 * responses and events do — a real host process only has one stdout.
 *
 * Fixture invariant (rule 1): `CONFORMANCE_CONFIG.capacity` is 8 and its
 * only look is `CONFORMANCE_LOOK_ID` ("conformance.panel", manual-fill, two
 * boxes, both chairs) — the only look id `ohg.look.set` may reference here.
 */

import { describe, expect, it } from "vitest";
import {
  CONFORMANCE_CONFIG,
  CONFORMANCE_LOOK_ID,
  OHG_ACTIONS,
  OHG_FIELD_TEMPLATES,
  ShowEngine,
  StateStore,
  type Participant,
  type StateFs
} from "../index.js";
import { HostLoop, type HostRuntime } from "./hostLoop.js";
import { StdioHostAdapter, WINDOWS_SHELL_CAPABILITIES } from "./stdioHostAdapter.js";

function memoryFs(seed: Record<string, string> = {}): StateFs {
  const files = new Map(Object.entries(seed));
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

function participant(participantId: string, rawName: string): Participant {
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: true,
    handRaised: false,
    zoomRole: 0
  };
}

type Rig = {
  loop: HostLoop;
  engine: ShowEngine;
  lines: () => string[];
  events: () => Array<Record<string, unknown>>;
  responses: () => Array<Record<string, unknown>>;
  revisionOf: () => number;
};

function rig(): Rig {
  let t = 0;
  const lines: string[] = [];
  const sink = (line: string): void => {
    lines.push(line);
  };
  const host = new StdioHostAdapter({ sink, generation: 5, capabilities: WINDOWS_SHELL_CAPABILITIES });
  const engine = new ShowEngine({
    config: CONFORMANCE_CONFIG,
    host,
    clock: { now: () => t },
    store: new StateStore(CONFORMANCE_CONFIG.statePath, { fs: memoryFs() })
  });
  const runtime: HostRuntime = {
    engine,
    generation: 5,
    engineVersion: "test-1.0.0",
    capacity: CONFORMANCE_CONFIG.capacity,
    sink,
    now: () => t
  };
  const loop = new HostLoop(runtime);
  return {
    loop,
    engine,
    lines: () => lines.slice(),
    events: () =>
      lines
        .map((l) => JSON.parse(l) as Record<string, unknown>)
        .filter((m) => "event" in m),
    responses: () =>
      lines
        .map((l) => JSON.parse(l) as Record<string, unknown>)
        .filter((m) => "id" in m),
    revisionOf: () => engine.revision()
  };
}

describe("HostLoop", () => {
  it("announce emits a handshake with the full action registry and field templates", () => {
    const { loop, events } = rig();
    loop.announce();
    const hs = events().find((e) => e.event === "handshake");
    expect(hs).toMatchObject({ protocolVersion: 1, generation: 5 });
    expect((hs as { actions: Array<{ id: string }> }).actions.map((a) => a.id)).toEqual(
      OHG_ACTIONS.map((a) => a.id)
    );
    expect((hs as { fieldTemplates: unknown }).fieldTemplates).toEqual(OHG_FIELD_TEMPLATES);
  });

  it("invoke answers with the ActionResult and publishes a snapshot in the same turn when revision moved", () => {
    const { loop, lines } = rig();
    loop.announce();
    const before = lines().length;
    loop.handleLine(
      JSON.stringify({ id: "a", type: "invoke", action: "ohg.look.set", args: [CONFORMANCE_LOOK_ID] })
    );
    const out = lines()
      .slice(before)
      .map((l) => JSON.parse(l) as Record<string, unknown>);
    expect(out[0]).toEqual({ id: "a", ok: true, result: { kind: "ok" } });
    expect(out.some((m) => m.event === "snapshot")).toBe(true);
  });

  it("a refused or malformed action is a RESULT, never a transport failure", () => {
    const { loop, responses } = rig();
    loop.handleLine(JSON.stringify({ id: "b", type: "invoke", action: "ohg.nope", args: [] }));
    loop.handleLine(
      JSON.stringify({ id: "c", type: "invoke", action: "ohg.panelist.remove", args: ["0042"] })
    );
    expect(responses()[0]).toMatchObject({ id: "b", ok: true, result: { kind: "error" } });
    expect(responses()[1]).toMatchObject({ id: "c", ok: true, result: { kind: expect.stringMatching(/refused|error/) } });
  });

  it("a malformed line is answered, not fatal, and the loop keeps serving", () => {
    const { loop, responses } = rig();
    loop.handleLine("{{{");
    loop.handleLine(JSON.stringify({ id: "p", type: "ping" }));
    expect(responses()[0]).toMatchObject({ id: null, ok: false });
    expect(responses()[1]).toMatchObject({ id: "p", ok: true });
  });

  it("tick publishes a snapshot only when the revision changed", async () => {
    const { loop, events } = rig();
    loop.announce();
    const n0 = events().filter((e) => e.event === "snapshot").length;
    await loop.tick(); // nothing changed
    expect(events().filter((e) => e.event === "snapshot").length).toBe(n0);
    loop.handleLine(
      JSON.stringify({
        id: "z",
        type: "zoomEvent",
        event: { kind: "roster", participants: [participant("p1", "Ada 0042")] }
      })
    );
    await loop.tick();
    expect(events().filter((e) => e.event === "snapshot").length).toBe(n0 + 1);
  });

  it("hostCommand lines carry the loop's generation and appear in engine call order", async () => {
    const { loop, events } = rig();
    loop.announce();
    loop.handleLine(
      JSON.stringify({
        id: "z",
        type: "zoomEvent",
        event: { kind: "roster", participants: [participant("p1", "Ada 0042")] }
      })
    );
    loop.handleLine(JSON.stringify({ id: "a", type: "invoke", action: "ohg.panelist.add", args: ["p1", 1] }));
    await loop.tick();
    const cmds = events().filter((e) => e.event === "hostCommand") as Array<{
      generation: number;
      seq: number;
    }>;
    expect(cmds.length).toBeGreaterThan(0);
    expect(cmds.every((c) => c.generation === 5)).toBe(true);
    expect(cmds.map((c) => c.seq)).toEqual(cmds.map((_, i) => i + 1));
  });

  it("capacity that disagrees with config is a warn log, not a failure", () => {
    const { loop, responses, events } = rig();
    loop.handleLine(JSON.stringify({ id: "k", type: "capacity", capacity: 99 }));
    expect(responses()[0]).toMatchObject({ id: "k", ok: true });
    expect(
      events().some((e) => e.event === "log" && e.level === "warn" && /capacity 99/.test(e.message as string))
    ).toBe(true);
  });

  it("shutdown flips shuttingDown after answering", () => {
    const { loop, responses } = rig();
    loop.handleLine(JSON.stringify({ id: "s", type: "shutdown" }));
    expect(responses()[0]).toEqual({ id: "s", ok: true });
    expect(loop.shuttingDown).toBe(true);
  });

  it("a single bad tick does not kill the show — it logs an error and the loop keeps serving", async () => {
    const { loop, engine, events, responses } = rig();
    loop.announce();
    // Force this one tick to reject, the way a real `store.save` failure or
    // a thrown derived-layer bug would (`ShowEngine.tick()`'s own doc
    // comment: a `StateFs` failure propagates out of `tick()` rather than
    // being swallowed there — `HostLoop.tick()` is the layer that must not
    // let it take the process down).
    engine.tick = async () => {
      throw new Error("bad tick: synthetic failure");
    };

    await expect(loop.tick()).resolves.toBeUndefined();

    expect(
      events().some(
        (e) => e.event === "log" && e.level === "error" && /bad tick: synthetic failure/.test(e.message as string)
      )
    ).toBe(true);

    // The loop is still alive and answering — a rejected tick must not
    // leave `handleLine` (or any later `tick()`) broken.
    loop.handleLine(JSON.stringify({ id: "after", type: "ping" }));
    expect(responses().some((r) => r.id === "after" && r.ok === true)).toBe(true);
  });
});
