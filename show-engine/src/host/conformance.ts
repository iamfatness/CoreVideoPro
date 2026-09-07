/**
 * The `--conformance` mode of the host process (Plan 7a Task 13, spec §11's
 * "conformance" row): run `HOST_CONFORMANCE_CASES` inside the REAL host
 * process so every host call the cases provoke also goes out on the REAL
 * wire, and a native shell's own test runner can watch its REAL adapter
 * receive them.
 *
 * The suite's runner contract (see `../conformance.ts`'s header) wants three
 * things per case, and this mode gives them:
 *
 * 1. **A fresh, never-ticked `ShowEngine`** built from `CONFORMANCE_CONFIG`
 *    with an in-memory `StateFs`. The config's `statePath` is a ROOT path on
 *    purpose (`/conformance/show-state.json`); wiring `nodeStateFs` here
 *    would try to `mkdir /conformance` and fail on a permission error that
 *    has nothing to do with the adapter under test.
 * 2. **A host that records every call.** Here that is a RECORDING FACADE
 *    over `StdioHostAdapter`: it appends the same `HostCall` shapes
 *    `MockHost` does (it IS a `MockHost`, so the cases' `callsOfKind`
 *    assertions run the same code vitest runs) AND forwards each call to the
 *    wire, so the same call is asserted here and delivered to the shell.
 *    Capabilities come from the WIRE, never from the recorder — the
 *    transport case branches on `capabilities()`, and a recorder that
 *    answered for itself could branch differently from what the shell was
 *    told.
 * 3. **A `flush`.** `async () => {}` — `CONFORMANCE_CONFIG` configures no
 *    integrations, so nothing is ever in flight; the suite's own header
 *    states a runner may pass a no-op and be conformant, and
 *    `verify-dist-barrel.mjs` already runs it that way.
 *
 * **The wire framing.** A `hostCommand` line carries no case name, so a
 * consumer bucketing commands per case has nothing to key on. This mode
 * therefore brackets each case with `log` lines — `conformance: begin
 * <name>` before it, `conformance: <name> ok` / `conformance: <name> FAIL
 * <message>` after it — and closes with `conformance: <passed>/<total>`.
 * Those three shapes are a CONTRACT with `AdapterConformanceTests.cs`; a
 * change here is a change there.
 *
 * **One `StdioHostAdapter` for the whole run, a fresh recorder per case.**
 * The recorder must be fresh (the cases assert on first-tick emission), but
 * `seq` must NOT restart: it is the wire's monotonic ordering field, and a
 * per-case restart would make one run look like N interleaved streams to any
 * consumer that trusts it.
 *
 * No `process`, no `readline`, no timers here — `main.ts` owns those and owns
 * the exit code. This module is driven by `conformance.test.ts` over an
 * in-memory sink.
 */

import { CONFORMANCE_CONFIG, HOST_CONFORMANCE_CASES, type ConformanceCase } from "../conformance.js";
import type { ProgramSource } from "../contracts.js";
import type { HostCapabilities, LookPlacement } from "../hostAdapter.js";
import type { Nameplate } from "../lookDirector.js";
import { MockHost } from "../mockHost.js";
import type { QuestionOverlay } from "../overlayDirector.js";
import { StateStore, type StateFs } from "../persistence.js";
import { ShowEngine } from "../showEngine.js";
import { HostLoop } from "./hostLoop.js";
import { encodeLine } from "./protocol.js";
import { StdioHostAdapter, WINDOWS_SHELL_CAPABILITIES, type LineSink } from "./stdioHostAdapter.js";

export type ConformanceModeOptions = {
  sink: LineSink;
  /** `--generation`; stamped on the handshake and on every `hostCommand`. */
  generation: number;
  /** Mirrors `main.ts`'s `ENGINE_VERSION` — passed in rather than duplicated. */
  engineVersion: string;
  /**
   * Awaited AFTER the handshake and BEFORE the first case. `main.ts` uses it
   * to hold the run until the parent supervisor has spoken once.
   *
   * This is not politeness, it is a correctness gate found by running the
   * real thing (Task 13): `ShowEngineSupervisor` sets `_currentGeneration`
   * only once it has PARSED the handshake, on a different thread from its
   * stdout reader, and it drops every `hostCommand` whose generation does
   * not match. A mode that ran its cases the microsecond after announcing
   * beat that write on a loaded machine and had its first ~19 commands
   * counted as stale — invisibly, because a dropped command is a counter,
   * not an error. A parent that has issued a request has necessarily
   * finished its handshake.
   *
   * Optional: a caller with no parent (this package's own tests) omits it.
   */
  beforeCases?: () => Promise<void>;
  /** Override for tests only; production always runs the shipped suite. */
  cases?: readonly ConformanceCase[];
};

export type ConformanceModeResult = {
  passed: number;
  total: number;
  /** `"<case name>: <message>"` per failure, in case order. Empty on a clean run. */
  failures: readonly string[];
};

/**
 * The suite asserts on emission, never on persistence (its header says so),
 * so this never has to survive a process — it only has to exist, because
 * `StateStore` refuses to invent a filesystem for itself.
 */
function memoryStateFs(): StateFs {
  const files = new Map<string, string>();
  return {
    readFile: async (path) => {
      const value = files.get(path);
      if (value === undefined) throw new Error(`ENOENT ${path}`);
      return value;
    },
    writeFile: async (path, content) => void files.set(path, content),
    rename: async (from, to) => {
      const value = files.get(from);
      if (value !== undefined) {
        files.set(to, value);
        files.delete(from);
      }
    },
    mkdir: async () => undefined
  };
}

/**
 * Records like a `MockHost` AND forwards to the wire. Extending `MockHost`
 * rather than reimplementing it is deliberate: `ConformanceHost` is defined
 * as a `Pick<MockHost, …>`, so this cannot drift from what the cases read,
 * and the recorded shapes are identical to the ones vitest asserts on.
 */
class RecordingStdioFacade extends MockHost {
  private readonly wire: StdioHostAdapter;

  constructor(wire: StdioHostAdapter) {
    super(WINDOWS_SHELL_CAPABILITIES);
    this.wire = wire;
  }

  /** The WIRE is the authority — the shell is told these, so the cases must branch on these. */
  override capabilities(): HostCapabilities {
    return this.wire.capabilities();
  }

  override assignSlot(slot: number, participantId: string | null): void {
    super.assignSlot(slot, participantId);
    this.wire.assignSlot(slot, participantId);
  }

  override applyLook(placement: LookPlacement): void {
    super.applyLook(placement);
    this.wire.applyLook(placement);
  }

  override setPreview(source: ProgramSource): void {
    super.setPreview(source);
    this.wire.setPreview(source);
  }

  override cut(): void {
    super.cut();
    this.wire.cut();
  }

  override auto(transitionId?: string): void {
    super.auto(transitionId);
    this.wire.auto(transitionId);
  }

  override setGallery(cells: ReadonlyMap<number, number>): void {
    super.setGallery(cells);
    this.wire.setGallery(cells);
  }

  override setNameplates(plates: readonly Nameplate[]): void {
    super.setNameplates(plates);
    this.wire.setNameplates(plates);
  }

  override setQuestion(question: QuestionOverlay | null): void {
    super.setQuestion(question);
    this.wire.setQuestion(question);
  }
}

function conformanceEngine(host: MockHost): ShowEngine {
  return new ShowEngine({
    config: CONFORMANCE_CONFIG,
    host,
    clock: { now: () => 1000 },
    store: new StateStore(CONFORMANCE_CONFIG.statePath, { fs: memoryStateFs() })
  });
}

/**
 * Emit the unsolicited `handshake` the supervisor waits for before it will
 * report Running — built by `HostLoop.announce()` off a THROWAWAY engine, so
 * the manifest is the one the normal loop announces rather than a second
 * hand-rolled shape that could drift. The throwaway engine's host is a plain
 * `MockHost`: `announce()` only reads `engine.snapshot()`, which emits
 * nothing, but wiring the real wire here would risk putting a command on the
 * pipe outside any case's begin/ok bracket.
 */
function announceHandshake(options: ConformanceModeOptions): void {
  const engine = conformanceEngine(new MockHost(WINDOWS_SHELL_CAPABILITIES));
  new HostLoop({
    engine,
    generation: options.generation,
    engineVersion: options.engineVersion,
    capacity: CONFORMANCE_CONFIG.capacity,
    sink: options.sink,
    now: () => 1000
  }).announce();
}

/** Run every case, narrating the run on `sink`. Never throws: a case that throws is a FAIL line. */
export async function runHostConformance(options: ConformanceModeOptions): Promise<ConformanceModeResult> {
  const cases = options.cases ?? HOST_CONFORMANCE_CASES;
  const emit = (level: "info" | "error", message: string): void => {
    options.sink(encodeLine({ event: "log", level, message }));
  };

  announceHandshake(options);
  if (options.beforeCases !== undefined) await options.beforeCases();

  const wire = new StdioHostAdapter({
    sink: options.sink,
    generation: options.generation,
    capabilities: WINDOWS_SHELL_CAPABILITIES
  });

  let passed = 0;
  const failures: string[] = [];

  for (const conformanceCase of cases) {
    emit("info", `conformance: begin ${conformanceCase.name}`);
    const host = new RecordingStdioFacade(wire);
    try {
      await conformanceCase.run(conformanceEngine(host), host, async () => {});
      passed += 1;
      emit("info", `conformance: ${conformanceCase.name} ok`);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      failures.push(`${conformanceCase.name}: ${message}`);
      emit("error", `conformance: ${conformanceCase.name} FAIL ${message}`);
    }
  }

  emit("info", `conformance: ${passed}/${cases.length}`);
  return { passed, total: cases.length, failures };
}
