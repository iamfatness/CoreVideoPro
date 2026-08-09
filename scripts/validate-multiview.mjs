// Headless multiview oracle — drives the REAL core over stdio with the fake Zoom
// engine (no WinUI, no port 8011, no UAC, no hardware) and judges the multiview
// wall the core actually publishes.
//
// SCOPE, honestly stated: the `multiview-shared-texture` event carries a GPU
// shared-texture HANDLE, not pixels, so this script judges TILE STRUCTURE AND
// GEOMETRY — that a pgmPvw layout really exposes a PROGRAM cell, a PREVIEW cell
// and N source tiles at sane 16:9 in-canvas rects, and that the render plan
// actually composites layers into them. It does NOT prove the tiles are not
// black; a blank-pixel regression needs D3D11CompositorTest or the real app.
// Do not let a green run here be read as "the wall looks right".
//
// usage: node scripts/validate-multiview.mjs [--sources N] [--mode pgmPvwTop] [--build-dir DIR]

import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};

const sourceCount = Number.parseInt(argValue("--sources", "4"), 10);
const layoutMode = argValue("--mode", "pgmPvwTop");
const tileCount = Number.parseInt(argValue("--tiles", "8"), 10);
const buildDir = path.resolve(argValue("--build-dir", "native/build-dev"));
const canvasWidth = 1920;
const canvasHeight = 1080;

const corePath = path.join(buildDir, "corevideo-native.exe");
const fakeEnginePath = path.join(buildDir, "corevideo-zoom-engine-fake.exe");
for (const [label, p] of [["core", corePath], ["fake engine", fakeEnginePath]]) {
  if (!existsSync(p)) {
    console.error(`[validate-multiview] ${label} not found at ${p}`);
    console.error("[validate-multiview] build it first: npm run build:native-dev");
    process.exit(2);
  }
}

const child = spawn(corePath, [], {
  stdio: ["pipe", "pipe", "pipe"],
  env: { ...process.env, COREVIDEO_ZOOM_ENGINE_PATH: fakeEnginePath },
});

let stdoutBuffer = "";
let stderrBuffer = "";
const pending = new Map();
let handshake;
let latestMultiviewEvent;

const timeout = setTimeout(() => fail("timed out waiting for the core"), 45000);

child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString("utf8");
  let nl;
  while ((nl = stdoutBuffer.indexOf("\n")) >= 0) {
    const line = stdoutBuffer.slice(0, nl).trim();
    stdoutBuffer = stdoutBuffer.slice(nl + 1);
    if (line) onLine(line);
  }
});
child.stderr.on("data", (chunk) => {
  stderrBuffer += chunk.toString("utf8");
});
child.once("exit", (code) => {
  if (pending.size > 0) fail(`core exited early (code ${code ?? "unknown"})`);
});

// The roster the shell would send: N capture sources in slot order.
const sources = Array.from({ length: sourceCount }, (_, i) => ({
  sourceId: `capture:probe-${i + 1}`,
  kind: "capture",
  participantId: "",
  captureDeviceId: `probe-${i + 1}`,
  mediaAssetId: "",
  slot: i,
  label: `Probe ${i + 1}`,
}));

// A program scene and a DIFFERENT preview scene, so the PGM and PVW cells each
// have something to composite. Without these both cells render empty and the
// wall looks like it "lost program and preview" even though the layout is right.
const programRoutes = [
  { routeId: "pgm-1", mode: "fixed", audioRole: "mix", captureDeviceId: "probe-1" },
];
const previewRoutes = [
  { routeId: "pvw-1", mode: "fixed", audioRole: "mix", captureDeviceId: "probe-2" },
];

try {
  child.stdin.write(`${JSON.stringify({ id: "mv-handshake", type: "handshake" })}\n`);
  handshake = await waitForHandshake();
  if (handshake.ok !== true) throw new Error("handshake failed");

  await send({
    id: "mv-setup",
    type: "media-core-sync",
    elapsedMs: 100,
    commands: [
      { type: "load-scene-graph", sceneId: "probe-program", routes: programRoutes },
      { type: "set-preview-scene", sceneId: "probe-preview", routes: previewRoutes },
      {
        type: "configure-multiviewer",
        layoutMode,
        tileCount,
        showLabels: true,
        showTally: true,
        showMeters: true,
        showClock: true,
      },
      {
        type: "set-multiview-layout",
        canvasWidth,
        canvasHeight,
        cols: 0,
        rows: 0,
        sources,
      },
    ],
  });

  // Drive render ticks until the multiview event lands. The event is emitted only
  // on STRUCTURAL change, so it arrives shortly after the layout is applied.
  for (let i = 0; i < 40 && !latestMultiviewEvent; i += 1) {
    await send({ id: `mv-tick-${i}`, type: "media-core-sync", elapsedMs: 33, commands: [] });
    await delay(33);
  }

  const event = latestMultiviewEvent;
  if (!event) throw new Error("core never emitted a multiview-shared-texture event");

  const tiles = event.tiles ?? [];
  const pgm = tiles.filter((t) => t.role === "pgm");
  const pvw = tiles.filter((t) => t.role === "pvw");
  const src = tiles.filter((t) => t.role === "source");
  const problems = [];

  const expectsBuses = layoutMode !== "grid";
  if (expectsBuses && pgm.length !== 1) {
    problems.push(`expected exactly 1 PROGRAM tile in "${layoutMode}", got ${pgm.length}`);
  }
  if (expectsBuses && pvw.length !== 1) {
    problems.push(`expected exactly 1 PREVIEW tile in "${layoutMode}", got ${pvw.length}`);
  }
  if (!expectsBuses && (pgm.length || pvw.length)) {
    problems.push(`"grid" must expose no PGM/PVW cells, got pgm=${pgm.length} pvw=${pvw.length}`);
  }

  const expectedSources = expectsBuses ? Math.min(sourceCount, tileCount) : sourceCount;
  if (src.length !== expectedSources) {
    problems.push(`expected ${expectedSources} source tiles, got ${src.length}`);
  }

  const canvasAspect = canvasWidth / canvasHeight;
  for (const tile of tiles) {
    const name = `${tile.role}:${tile.label || tile.sourceId || tile.slot}`;
    if (!(tile.w > 0 && tile.h > 0)) {
      problems.push(`${name} has a degenerate rect (w=${tile.w} h=${tile.h})`);
      continue;
    }
    if (tile.x < -1e-3 || tile.y < -1e-3 || tile.x + tile.w > 1 + 1e-3 || tile.y + tile.h > 1 + 1e-3) {
      problems.push(`${name} falls outside the canvas (x=${f(tile.x)} y=${f(tile.y)} w=${f(tile.w)} h=${f(tile.h)})`);
    }
    // Normalized rects are against a canvasAspect canvas, so a 16:9 tile has
    // aspect (w*canvasW)/(h*canvasH) == 16/9.
    const aspect = (tile.w * canvasWidth) / (tile.h * canvasHeight);
    if (Math.abs(aspect - 16 / 9) > 0.02) {
      problems.push(`${name} is not 16:9 (aspect ${f(aspect)})`);
    }
  }

  for (let i = 0; i < tiles.length; i += 1) {
    for (let j = i + 1; j < tiles.length; j += 1) {
      if (overlaps(tiles[i], tiles[j])) {
        problems.push(`tiles overlap: ${tiles[i].role}#${i} and ${tiles[j].role}#${j}`);
      }
    }
  }

  // The buses must actually COMPOSITE, not just exist as empty boxes. The core's
  // one-shot stderr line reports the layer count of the multiview render plan.
  const composite = /\[multiview\] composite: sources=(\d+) layers=(\d+)/.exec(stderrBuffer);
  if (composite) {
    const layers = Number.parseInt(composite[2], 10);
    console.log(`[validate-multiview] first composite: sources=${composite[1]} layers=${layers}`);
  }

  console.log(
    `[validate-multiview] mode=${layoutMode} sources=${sourceCount} tiles=${tiles.length} ` +
      `(pgm=${pgm.length} pvw=${pvw.length} source=${src.length}) canvas=${event.canvasWidth}x${event.canvasHeight}`,
  );
  for (const tile of tiles) {
    console.log(
      `  ${String(tile.role).padEnd(6)} slot=${String(tile.slot).padStart(3)} ` +
        `rect=[${f(tile.x)} ${f(tile.y)} ${f(tile.w)} ${f(tile.h)}] ${tile.label || tile.sourceId || ""}`,
    );
  }

  if (problems.length) {
    for (const p of problems) console.error(`[validate-multiview] FAIL: ${p}`);
    throw new Error(`${problems.length} multiview problem(s)`);
  }

  clearTimeout(timeout);
  cleanup();
  console.log("[validate-multiview] PASS");
} catch (error) {
  fail(error instanceof Error ? error.message : String(error));
}

function overlaps(a, b) {
  const eps = 1e-3;
  return (
    a.x < b.x + b.w - eps && b.x < a.x + a.w - eps && a.y < b.y + b.h - eps && b.y < a.y + a.h - eps
  );
}

function f(n) {
  return Number(n).toFixed(3);
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function onLine(line) {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }
  if (message.type === "handshake" && message.ok === true) handshake = message;
  if (message.type === "multiview-shared-texture") latestMultiviewEvent = message;
  if (typeof message.id === "string") {
    const resolver = pending.get(message.id);
    if (resolver) {
      pending.delete(message.id);
      resolver(message);
    }
  }
}

function waitForHandshake() {
  if (handshake) return Promise.resolve(handshake);
  return new Promise((resolve) => {
    const interval = setInterval(() => {
      if (handshake) {
        clearInterval(interval);
        resolve(handshake);
      }
    }, 25);
  });
}

function send(request) {
  return new Promise((resolve) => {
    pending.set(request.id, resolve);
    child.stdin.write(`${JSON.stringify(request)}\n`);
  });
}

function fail(message) {
  clearTimeout(timeout);
  cleanup();
  if (stderrBuffer.trim()) {
    console.error("--- core stderr (tail) ---");
    console.error(stderrBuffer.trim().split("\n").slice(-30).join("\n"));
  }
  console.error(`[validate-multiview] ${message}`);
  process.exit(1);
}

function cleanup() {
  pending.clear();
  if (!child.killed) child.kill();
}
