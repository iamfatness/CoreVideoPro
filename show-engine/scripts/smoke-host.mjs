#!/usr/bin/env node
/**
 * Smoke test for `dist/host/main.js` — the BUILT host process, not source.
 * Mirrors `verify-dist-barrel.mjs`'s reasoning: `npm test` never builds, so
 * a vitest test importing `dist/` would either fail on a clean checkout or
 * quietly "skip when dist is missing", which looks like coverage and isn't.
 *
 * Builds a temp config from `dist/index.js`'s own `CONFORMANCE_CONFIG`
 * (statePath rewritten under `os.tmpdir()` so the real process can actually
 * write it), spawns the real entry point against it, waits up to 5s for a
 * `handshake` event with `protocolVersion === 1` and the full 28-action
 * registry, sends `shutdown`, and requires a clean exit 0 within a SECOND
 * bounded wait. Both waits are independently bounded: a script that hangs
 * forever on the exact failure it exists to catch (the child answers the
 * handshake but never actually exits after `shutdown`) is worse than a
 * script that fails fast, so `shutdown()` not landing within
 * `SHUTDOWN_TIMEOUT_MS` kills the child and fails loudly rather than
 * awaiting an `exit` event that may never come. Any other outcome is a hard
 * `process.exit(1)` with the collected stdio printed — this script IS the
 * coverage for `main.ts`, which has no unit test.
 */

import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const packageRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const distEntry = path.join(packageRoot, "dist", "index.js");
const hostEntry = path.join(packageRoot, "dist", "host", "main.js");

const HANDSHAKE_TIMEOUT_MS = 5000;
const SHUTDOWN_TIMEOUT_MS = 5000;
const EXPECTED_ACTION_COUNT = 28;

function fail(message, stdout, stderr) {
  process.stderr.write(`smoke-host: FAIL — ${message}\n`);
  if (stdout.length > 0) process.stderr.write(`--- stdout ---\n${stdout.join("")}\n`);
  if (stderr.length > 0) process.stderr.write(`--- stderr ---\n${stderr.join("")}\n`);
  process.exit(1);
}

async function main() {
  const { CONFORMANCE_CONFIG } = await import(pathToFileURL(distEntry).href);

  const tmpDir = mkdtempSync(path.join(tmpdir(), "show-engine-host-smoke-"));
  const statePath = path.join(tmpDir, "show-state.json");
  const configPath = path.join(tmpDir, "config.json");
  const config = { ...CONFORMANCE_CONFIG, statePath };
  writeFileSync(configPath, JSON.stringify(config, null, 2), "utf8");

  const child = spawn(process.execPath, [hostEntry, "--config", configPath, "--generation", "1"], {
    stdio: ["pipe", "pipe", "pipe"]
  });

  const stdoutChunks = [];
  const stderrChunks = [];
  let stdoutBuffer = "";
  let handshakeSeen = false;
  let exitCode = null;

  const exitPromise = new Promise((resolve) => {
    child.on("exit", (code) => {
      exitCode = code;
      resolve();
    });
  });

  child.stdout.on("data", (chunk) => {
    const text = chunk.toString("utf8");
    stdoutChunks.push(text);
    stdoutBuffer += text;
    let newlineIndex = stdoutBuffer.indexOf("\n");
    while (newlineIndex !== -1) {
      const line = stdoutBuffer.slice(0, newlineIndex);
      stdoutBuffer = stdoutBuffer.slice(newlineIndex + 1);
      newlineIndex = stdoutBuffer.indexOf("\n");
      if (line.trim().length === 0) continue;
      let message;
      try {
        message = JSON.parse(line);
      } catch {
        continue;
      }
      if (message.event === "handshake" && !handshakeSeen) {
        if (message.protocolVersion !== 1) {
          fail(`handshake protocolVersion was ${message.protocolVersion}, expected 1`, stdoutChunks, stderrChunks);
          return;
        }
        if (!Array.isArray(message.actions) || message.actions.length !== EXPECTED_ACTION_COUNT) {
          fail(
            `handshake actions.length was ${Array.isArray(message.actions) ? message.actions.length : "n/a"}, expected ${EXPECTED_ACTION_COUNT}`,
            stdoutChunks,
            stderrChunks
          );
          return;
        }
        handshakeSeen = true;
        process.stdout.write(
          `smoke-host: OK — handshake protocolVersion=1, actions=${message.actions.length}\n`
        );
        child.stdin.write(JSON.stringify({ id: "s", type: "shutdown" }) + "\n");
      }
    }
  });

  child.stderr.on("data", (chunk) => {
    stderrChunks.push(chunk.toString("utf8"));
  });

  const timeout = new Promise((resolve) => {
    setTimeout(resolve, HANDSHAKE_TIMEOUT_MS, "timeout");
  });

  const outcome = await Promise.race([exitPromise.then(() => "exited"), timeout]);

  if (!handshakeSeen) {
    child.kill();
    fail(
      outcome === "timeout"
        ? `no handshake event within ${HANDSHAKE_TIMEOUT_MS}ms`
        : `process exited (code ${exitCode}) before a handshake event arrived`,
      stdoutChunks,
      stderrChunks
    );
    return;
  }

  if (exitCode === null) {
    const shutdownTimeout = new Promise((resolve) => {
      setTimeout(resolve, SHUTDOWN_TIMEOUT_MS, "timeout");
    });
    const shutdownOutcome = await Promise.race([exitPromise.then(() => "exited"), shutdownTimeout]);
    if (shutdownOutcome === "timeout") {
      child.kill();
      fail(
        `handshake arrived and shutdown was sent, but the process did not exit within ${SHUTDOWN_TIMEOUT_MS}ms`,
        stdoutChunks,
        stderrChunks
      );
      return;
    }
  }

  if (exitCode !== 0) {
    fail(`process exited with code ${exitCode}, expected 0`, stdoutChunks, stderrChunks);
    return;
  }

  process.stdout.write("smoke-host: PASS\n");
  process.exit(0);
}

main().catch((error) => {
  process.stderr.write(`smoke-host: FAIL — unhandled: ${error instanceof Error ? error.stack : String(error)}\n`);
  process.exit(1);
});
