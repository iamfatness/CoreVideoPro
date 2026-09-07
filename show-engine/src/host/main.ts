#!/usr/bin/env node
/**
 * The Windows shell's process entry point: parses argv, reads and parses
 * the config file, wires the real Node adapters (`nodeStateFs`,
 * `nodeFetch`, `StdioHostAdapter`, `systemClock`) into a `ShowEngine`, and
 * drives a `HostLoop` over `process.stdin`/`process.stdout` on a 250ms
 * poll cadence. This file — together with `hostLoop.ts`'s pure core it
 * glues to `process` — is the only place in `src/host/` (indeed the only
 * place in this package) permitted to touch `process`, `readline`, or
 * `setInterval`. No unit test covers it directly; `scripts/smoke-host.mjs`
 * (run against the BUILT `dist/host/main.js`) is the coverage.
 */

import { createInterface } from "node:readline";
import { readFile } from "node:fs/promises";
import { parseShowEngineConfig } from "../config.js";
import { systemClock } from "../clock.js";
import { StateStore } from "../persistence.js";
import { ShowEngine } from "../showEngine.js";
import { MukanaClient } from "../mukanaClient.js";
import { HostLoop, type HostRuntime } from "./hostLoop.js";
import { runHostConformance } from "./conformance.js";
import { StdioHostAdapter, WINDOWS_SHELL_CAPABILITIES, type LineSink } from "./stdioHostAdapter.js";
import { nodeStateFs } from "./nodeStateFs.js";
import { nodeFetch } from "./nodeFetch.js";
import { decodeRequest, encodeLine } from "./protocol.js";

const EX_USAGE = 64;
const EX_CONFIG = 78;
const EX_SOFTWARE = 70;

/** package.json isn't imported directly (NodeNext + no resolveJsonModule wiring here); a fixed literal is fine for a log-only field. */
const ENGINE_VERSION = "0.1.0";

const TICK_INTERVAL_MS = 250;

type Argv = { configPath: string; generation: number };

/**
 * `--generation` alone. The conformance mode (`--conformance`) needs no
 * config: it constructs its engines from `CONFORMANCE_CONFIG` internally, by
 * contract with the cases, which name that exact config. `--config` is still
 * ACCEPTED and ignored so a caller can build one uniform spawn request for
 * both modes (which is exactly what `AdapterConformanceTests.cs` does — the
 * real `ProcessShowEngineChild` always passes `--config`).
 */
function parseGenerationOnly(argv: readonly string[]): number | null {
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i] !== "--generation") continue;
    const value = argv[i + 1];
    if (value === undefined) return null;
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }
  return 0;
}

function parseArgv(argv: readonly string[]): Argv | null {
  let configPath: string | null = null;
  let generation = 0;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--config") {
      const value = argv[i + 1];
      if (value === undefined) return null;
      configPath = value;
      i += 1;
    } else if (arg === "--generation") {
      const value = argv[i + 1];
      if (value === undefined) return null;
      const parsed = Number(value);
      if (!Number.isFinite(parsed)) return null;
      generation = parsed;
      i += 1;
    }
  }
  if (configPath === null) return null;
  return { configPath, generation };
}

function stdoutSink(): LineSink {
  return (line: string): void => {
    process.stdout.write(line + "\n");
  };
}

/**
 * Write one line to `stream` and exit ONLY after the write's callback fires.
 * A piped stdout/stderr is asynchronous on Windows, so `process.exit()` on
 * the very next statement can race the OS write and truncate — or entirely
 * drop — the diagnostic line the caller most needs to see. Every exit path
 * in this file (usage, config error, uncaught/unhandled) goes through this;
 * `shutdown()`'s clean-exit path already did, which is what exposed the gap
 * on the three failure paths.
 */
function writeThenExit(stream: NodeJS.WritableStream, text: string, code: number): void {
  stream.write(text, () => {
    process.exit(code);
  });
}

function exitWithErrorLine(message: string, code: number): void {
  writeThenExit(process.stdout, encodeLine({ event: "log", level: "error", message }) + "\n", code);
}

/** How long the conformance mode waits for the parent to speak before running anyway. */
const CONFORMANCE_START_SIGNAL_TIMEOUT_MS = 10_000;

/** How long it waits after the tally for a shutdown/EOF before exiting on its own. */
const CONFORMANCE_EXIT_SIGNAL_TIMEOUT_MS = 60_000;

/** Resolve when `promise` settles, or after `ms` — a wait here may delay a run, never hang one. */
function settledOrAfter(promise: Promise<void>, ms: number): Promise<void> {
  return new Promise<void>((resolve) => {
    const timer = setTimeout(resolve, ms);
    void promise.then(() => {
      clearTimeout(timer);
      resolve();
    });
  });
}

/**
 * `--conformance`: run the host conformance suite in-process and exit 0 iff every case passed.
 *
 * **It is bracketed by the parent on BOTH ends, and both brackets were put there by running the
 * real `AdapterConformanceTests` rather than by reasoning about it.** The C# supervisor is the
 * only consumer, and it has two windows in which a `hostCommand` is thrown away:
 *
 *  - *Before* it has parsed the handshake, `_currentGeneration` is still 0 and every command is
 *    dropped as stale. Its reader thread and its handshake parse are DIFFERENT threads, so a mode
 *    that started its cases immediately after announcing raced that write — and lost, under a full
 *    test suite's load: 19 commands silently counted as stale. So the run waits for the parent to
 *    issue any request (a parent that can issue one has necessarily finished its handshake).
 *  - *After* it has claimed the child as exited, every remaining command in the pipe is dropped as
 *    stale too. Exiting the instant the tally is printed leaves the tail of the run unread in the
 *    pipe while the exit watcher detaches, so the last case's commands vanish — intermittently,
 *    and invisibly, because a dropped command is a counter and not an error. So the run holds the
 *    process open until the parent says shutdown or closes stdin. (This is a deliberate deviation
 *    from the task brief's "it exits by itself" ruling; the ruling's own fallback — tolerate the
 *    resulting health — does not cover the lost commands.)
 *
 * Every well-formed request is answered `ok` so the supervisor's stop path and heartbeat never
 * have to time out. Both waits are BOUNDED: a caller that never speaks delays the run, never hangs
 * it. Interactive use (a TTY stdin — nobody is driving it) skips both brackets entirely.
 */
async function runConformanceMode(sink: LineSink, generation: number): Promise<void> {
  const finishWith = (result: { passed: number; total: number }): void => {
    writeThenExit(process.stdout, "", result.passed === result.total ? 0 : EX_SOFTWARE);
  };

  if (process.stdin.isTTY) {
    finishWith(await runHostConformance({ sink, generation, engineVersion: ENGINE_VERSION }));
    return;
  }

  let parentSpoke = (): void => {};
  const parentReady = new Promise<void>((resolve) => {
    parentSpoke = resolve;
  });
  let parentReleased = (): void => {};
  const parentFinished = new Promise<void>((resolve) => {
    parentReleased = resolve;
  });

  const rl = createInterface({ input: process.stdin });
  rl.on("line", (line: string) => {
    const decoded = decodeRequest(line);
    if (decoded.kind !== "request") return;
    sink(encodeLine({ id: decoded.request.id, ok: true }));
    parentSpoke();
    if (decoded.request.type === "shutdown") parentReleased();
  });
  rl.on("close", () => {
    // EOF: there is no parent left to synchronise with, on either end.
    parentSpoke();
    parentReleased();
  });

  const result = await runHostConformance({
    sink,
    generation,
    engineVersion: ENGINE_VERSION,
    beforeCases: () => settledOrAfter(parentReady, CONFORMANCE_START_SIGNAL_TIMEOUT_MS)
  });

  await settledOrAfter(parentFinished, CONFORMANCE_EXIT_SIGNAL_TIMEOUT_MS);
  rl.close();
  finishWith(result);
}

async function main(): Promise<void> {
  const sink = stdoutSink();

  if (process.argv.includes("--conformance")) {
    const generation = parseGenerationOnly(process.argv.slice(2));
    if (generation === null) {
      exitWithErrorLine("usage: show-engine-host --conformance [--generation <n>]", EX_USAGE);
      return;
    }

    await runConformanceMode(sink, generation);
    return;
  }

  const args = parseArgv(process.argv.slice(2));

  if (args === null) {
    exitWithErrorLine("usage: show-engine-host --config <path> [--generation <n>]", EX_USAGE);
    return;
  }

  let engine: ShowEngine;
  let loop: HostLoop;
  try {
    const raw = await readFile(args.configPath, "utf8");
    const parsed: unknown = JSON.parse(raw);
    const engineRaw =
      typeof parsed === "object" && parsed !== null && "engine" in parsed
        ? (parsed as { engine: unknown }).engine
        : parsed;
    const config = parseShowEngineConfig(engineRaw);

    const store = new StateStore(config.statePath, { fs: nodeStateFs() });
    const host = new StdioHostAdapter({ sink, generation: args.generation, capabilities: WINDOWS_SHELL_CAPABILITIES });
    const mukana = config.mukana === null ? undefined : new MukanaClient(config.mukana, { fetch: nodeFetch });

    engine = new ShowEngine({ config, host, clock: systemClock, store, mukana });

    const runtime: HostRuntime = {
      engine,
      generation: args.generation,
      engineVersion: ENGINE_VERSION,
      capacity: config.capacity,
      sink,
      now: () => Date.now()
    };
    loop = new HostLoop(runtime);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    exitWithErrorLine(`config error: ${message}`, EX_CONFIG);
    return;
  }

  await engine.restore();
  loop.announce();

  const interval = setInterval(() => void loop.tick(), TICK_INTERVAL_MS);

  let shuttingDownStarted = false;
  const shutdown = (): void => {
    if (shuttingDownStarted) return;
    shuttingDownStarted = true;
    clearInterval(interval);
    writeThenExit(process.stdout, "", 0);
  };

  const rl = createInterface({ input: process.stdin });
  rl.on("line", (line: string) => {
    loop.handleLine(line);
    if (loop.shuttingDown) {
      rl.close();
      shutdown();
    }
  });
  rl.on("close", () => {
    shutdown();
  });
}

process.on("uncaughtException", (error: unknown) => {
  const message = error instanceof Error ? error.message : String(error);
  writeThenExit(process.stderr, encodeLine({ event: "log", level: "error", message }) + "\n", EX_SOFTWARE);
});

process.on("unhandledRejection", (reason: unknown) => {
  const message = reason instanceof Error ? reason.message : String(reason);
  writeThenExit(process.stderr, encodeLine({ event: "log", level: "error", message }) + "\n", EX_SOFTWARE);
});

void main();
