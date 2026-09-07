/**
 * Plan 7a Task 13 — the `--conformance` mode of the host process.
 *
 * This drives `runHostConformance` over an IN-MEMORY sink (no process, no
 * stdio, no child): the mode's contract is "what appears on the wire", and
 * a `LineSink` is exactly that wire. `main.ts`'s argv dispatch and the
 * stdin hold-open are NOT covered here — they are `process`-level glue, the
 * same gap `scripts/smoke-host.mjs` covers for the normal loop, and the C#
 * `AdapterConformanceTests` covers for this mode end to end.
 *
 * What the assertions are actually FOR: the C# side buckets host commands
 * per case by watching for the `conformance: begin <name>` log line, because
 * a `hostCommand` line carries no case name. So "a begin line precedes every
 * case's commands" and "the ok line follows them" are not cosmetics — they
 * are the framing the adapter conformance run parses.
 */

import { describe, expect, it } from "vitest";
import { HOST_CONFORMANCE_CASES } from "../conformance.js";
import type { HostAdapter } from "../hostAdapter.js";
import { MockHost } from "../mockHost.js";
import { RecordingStdioFacade, runHostConformance } from "./conformance.js";

type Line = Record<string, unknown> & { event?: string };

async function run(generation = 1): Promise<{ result: Awaited<ReturnType<typeof runHostConformance>>; lines: Line[] }> {
  const raw: string[] = [];
  const result = await runHostConformance({
    sink: (line) => raw.push(line),
    generation,
    engineVersion: "0.1.0"
  });
  return { result, lines: raw.map((line) => JSON.parse(line) as Line) };
}

function logs(lines: Line[]): string[] {
  return lines.filter((line) => line.event === "log").map((line) => String(line.message));
}

/**
 * Every method on the `HostAdapter` port, written out so it can be checked at
 * RUNTIME. `satisfies Record<keyof HostAdapter, true>` pins it in both
 * directions against the interface itself: a method added to `HostAdapter`
 * and not listed here is a missing key (compile error), and a name here that
 * is not on `HostAdapter` is an excess property (compile error). So the list
 * cannot rot into a stale copy of the port.
 */
const HOST_ADAPTER_METHODS = {
  capabilities: true,
  assignSlot: true,
  applyLook: true,
  setPreview: true,
  cut: true,
  auto: true,
  setGallery: true,
  setNameplates: true,
  setQuestion: true
} satisfies Record<keyof HostAdapter, true>;

describe("RecordingStdioFacade (the wrapper law)", () => {
  /**
   * THE WRAPPER LAW. `RecordingStdioFacade` extends `MockHost`, whose methods
   * all RECORD — so an un-overridden method is invisible twice over: it is not
   * a compile error (the base satisfies the type), and it is not a test
   * failure (every conformance case reads the recorder, and the recorder was
   * written to). The call is simply never put on the wire, and the shell never
   * hears about that command. This is the exact `WinUiCaptureDeviceAdapter`
   * bug this repo documents — an inherited permissive default that swallowed
   * every ingested audio frame while video flowed perfectly.
   *
   * `hasOwnProperty` on the prototype is the check that catches it: an
   * inherited method is on `MockHost.prototype`, never on this one.
   */
  it("overrides every HostAdapter method rather than inheriting a record-only one", () => {
    const missing = Object.keys(HOST_ADAPTER_METHODS).filter(
      (name) => !Object.prototype.hasOwnProperty.call(RecordingStdioFacade.prototype, name)
    );

    expect(missing).toEqual([]);
  });

  /** The list above is only a guard if the names are real methods; this is what says so. */
  it("names methods that MockHost actually implements", () => {
    const host = new MockHost();
    for (const name of Object.keys(HOST_ADAPTER_METHODS)) {
      expect(typeof (host as unknown as Record<string, unknown>)[name]).toBe("function");
    }
  });
});

describe("runHostConformance", () => {
  it("reports every shipped case as ok and tallies them", async () => {
    const { result, lines } = await run();

    expect(result.failures).toEqual([]);
    expect(result.passed).toBe(HOST_CONFORMANCE_CASES.length);
    expect(result.total).toBe(HOST_CONFORMANCE_CASES.length);

    const messages = logs(lines);
    for (const conformanceCase of HOST_CONFORMANCE_CASES) {
      expect(messages).toContain(`conformance: begin ${conformanceCase.name}`);
      expect(messages).toContain(`conformance: ${conformanceCase.name} ok`);
    }
    expect(messages.at(-1)).toBe(`conformance: ${HOST_CONFORMANCE_CASES.length}/${HOST_CONFORMANCE_CASES.length}`);
  });

  it("announces a handshake before the first case so a supervisor reaches Running", async () => {
    const { lines } = await run(7);

    expect(lines[0]?.event).toBe("handshake");
    expect(lines[0]?.generation).toBe(7);
    expect(lines[0]?.protocolVersion).toBe(1);
    expect(Array.isArray(lines[0]?.actions)).toBe(true);
  });

  it("emits every case's hostCommands BETWEEN its begin and its ok line", async () => {
    const { lines } = await run(4);

    for (const conformanceCase of HOST_CONFORMANCE_CASES) {
      const begin = lines.findIndex(
        (line) => line.event === "log" && line.message === `conformance: begin ${conformanceCase.name}`
      );
      const ok = lines.findIndex(
        (line) => line.event === "log" && line.message === `conformance: ${conformanceCase.name} ok`
      );
      expect(begin).toBeGreaterThanOrEqual(0);
      expect(ok).toBeGreaterThan(begin);

      const commands = lines.slice(begin + 1, ok).filter((line) => line.event === "hostCommand");
      // Every shipped case seats the cast, so every case binds slots at least.
      expect(commands.length).toBeGreaterThan(0);
      expect(commands.every((line) => line.generation === 4)).toBe(true);
    }
  });

  it("gives the whole run ONE monotonic seq stream stamped with the argv generation", async () => {
    const { lines } = await run(9);

    const seqs = lines.filter((line) => line.event === "hostCommand").map((line) => Number(line.seq));
    expect(seqs.length).toBeGreaterThan(0);
    expect(seqs).toEqual(seqs.map((_, index) => index + 1));
  });

  it("reports a failing case as FAIL with the case's own message and keeps going", async () => {
    const raw: string[] = [];
    const result = await runHostConformance({
      sink: (line) => raw.push(line),
      generation: 1,
      engineVersion: "0.1.0",
      cases: [
        { name: "bad", run: async () => { throw new Error("host conformance [bad]: nope"); } },
        { name: "good", run: async () => {} }
      ]
    });

    expect(result.passed).toBe(1);
    expect(result.total).toBe(2);
    expect(result.failures).toEqual(["bad: host conformance [bad]: nope"]);

    const messages = logs(raw.map((line) => JSON.parse(line) as Line));
    expect(messages).toContain("conformance: bad FAIL host conformance [bad]: nope");
    expect(messages).toContain("conformance: good ok");
    expect(messages.at(-1)).toBe("conformance: 1/2");
  });
});
