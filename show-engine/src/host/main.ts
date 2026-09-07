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
import { StdioHostAdapter, WINDOWS_SHELL_CAPABILITIES, type LineSink } from "./stdioHostAdapter.js";
import { nodeStateFs } from "./nodeStateFs.js";
import { nodeFetch } from "./nodeFetch.js";
import { encodeLine } from "./protocol.js";

const EX_USAGE = 64;
const EX_CONFIG = 78;
const EX_SOFTWARE = 70;

/** package.json isn't imported directly (NodeNext + no resolveJsonModule wiring here); a fixed literal is fine for a log-only field. */
const ENGINE_VERSION = "0.1.0";

const TICK_INTERVAL_MS = 250;

type Argv = { configPath: string; generation: number };

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

async function main(): Promise<void> {
  const args = parseArgv(process.argv.slice(2));
  const sink = stdoutSink();

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
