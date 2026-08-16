/**
 * Headless pixel oracle for the T1 core-composited Tiles wall.
 *
 * Every other validator in this repo that touches the wall (validate-multiview.mjs)
 * explicitly disclaims judging pixels — it only proves STRUCTURE (rect count,
 * aspect, no-overlap). This repo has shipped a defect PAST a structural/proxy
 * validator TWICE (a 320x180 thumbnail muxed into program for months while every
 * validator checked stream presence; a sender capped at 50fps while its container
 * read a healthy 59.9fps). This script exists so Tiles cannot do the same: it
 * spawns the REAL core over stdio with the fake Zoom engine (distinct animated
 * I420 per participant), records the wall to an MP4, decodes real frames with
 * ffmpeg, and judges what is ACTUALLY DRAWN.
 *
 * Ground truth for "who is in this box" is NOT read from the core's own `tiles`
 * snapshot node's sourceId. That field is written by the same expansion loop
 * that draws the layer (`layer.sourceId = admitted[index]` in the same iteration
 * that sets `layer.rect` and `layer.participantId`), so it is tautologically
 * self-consistent even if the loop draws the WRONG participant at a rect — an
 * oracle that trusted it would be exactly the kind of proxy this task exists to
 * retire. Instead this script:
 *   - reads RECT GEOMETRY from the published `tiles` node (never re-solves
 *     layout with compositor::solveTilesLayout — that would be judging the
 *     solver with itself),
 *   - but decides WHO SHOULD BE at each geometric slot from its OWN knowledge:
 *     the member order it sent in `tiles.members`, paired with solved rects in
 *     row-major reading order (solveTilesLayout's own documented output order,
 *     confirmed by reading TilesLayout.h — NOT re-implemented here, just relied
 *     upon as an ordering CONTRACT: rects[i] pairs with admitted[i]),
 *   - identifies WHO is actually drawn at a rect by sampling real decoded
 *     pixels and recovering per-participant chrominance (U,V), which the fake
 *     engine holds CONSTANT across an entire participant's frame
 *     (`std::memset(up, 90+(pid*53)%110, ...)` / `vp, 90+(pid*97)%110` in
 *     fake-engine.cpp) — so a median-filtered patch anywhere inside a tile
 *     recovers a stable per-participant signal regardless of the animated
 *     luma gradient/moving band. The recovered chroma is matched against an
 *     EMPIRICALLY MEASURED per-participant fingerprint (phase 0 below: each
 *     participant placed at an explicit, harness-chosen rect via plain scene
 *     routes, never the tiles wall/solver) rather than a closed-form formula
 *     — the real encode/decode round trip shifts absolute chroma enough that
 *     a formula-only prediction left nearest-match margins uncomfortably
 *     close to noise when this was first measured against the real pipeline.
 *
 * This is why the deliberate-break in Step 3 (reversing `admitted` right
 * before the rect-assignment loop) is detectable at all: the reversal keeps
 * `sourceId`/`rect`/actual-drawn-frame mutually consistent (self-referential),
 * but it breaks the relationship between MY sent order and the geometric
 * position — which is exactly what this oracle checks.
 *
 * Scope (T1 only — see task-6-brief.md): implements per-tile identity,
 * background colour, fill-not-letterbox, and reflow-after-departure. Glow is
 * T2 (not built yet). Byte-identical-at-rest is skipped: T1 has no animation
 * toggle to hold constant against, so there is nothing to freeze and compare.
 *
 * Usage: node scripts/validate-tiles.mjs [--participants 3] [--build-dir DIR]
 *          [--phase-seconds 3] [--keep-artifacts]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, statSync, rmSync } from "node:fs";
import { dirname, join, resolve, isAbsolute } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDirDefault = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const buildDir = resolve(argValue("build-dir", buildDirDefault));
const participantCount = Math.max(2, Math.min(6, Number(argValue("participants", 3))));
const phaseSeconds = Math.max(1.5, Number(argValue("phase-seconds", 3)));
const keepArtifacts = args.includes("--keep-artifacts");

const nativeCore = join(buildDir, `corevideo-native${exeSuffix}`);
const fakeEngine = join(buildDir, `corevideo-zoom-engine-fake${exeSuffix}`);
if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Run scripts/build-native-dev.ps1 first.`);
  process.exit(2);
}

function resolveBinary(candidates) {
  for (const bin of candidates) {
    const probe = spawnSync(bin, ["-version"], { encoding: "utf8", timeout: 10000 });
    if (!probe.error && probe.status === 0) return bin;
  }
  return candidates[0];
}
const ffmpegBin = resolveBinary(["ffmpeg", "C:\\ffmpeg\\bin\\ffmpeg.exe"]);
const ffprobeBin = resolveBinary(["ffprobe", "C:\\ffmpeg\\bin\\ffprobe.exe"]);

const canvasWidth = 1920;
const canvasHeight = 1080;
const backgroundColor = "#101418";
const bgRgb = { r: 0x10, g: 0x14, b: 0x18 };
const targetFolder = "Recordings/CoreVideoPro/validate-tiles";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ── Ground-truth participant colour model (from fake-engine.cpp, NOT the wall
// solver under test — this is a completely separate subsystem: the synthetic
// video generator's per-participant chroma formula). Held constant across the
// WHOLE frame per participant (memset), so we never need to predict luma. ──
function predictedChroma(pid) {
  const u = 90 + ((pid * 53) % 110);
  const v = 90 + ((pid * 97) % 110);
  return { u, v };
}
// Inverse BT.709 full-range (matches D3D11CompositorAdapter's
// kCompositorYuvPixelShader colour space, per CLAUDE.md): recovers (Y,U,V)
// from a decoded RGB sample so we can compare chroma without needing to
// predict luma (which the fake engine's diagonal gradient + moving band make
// position/time dependent and therefore unpredictable at a point).
function rgbToYuv({ r, g, b }) {
  const y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  const u = (b - y) / 1.8556 + 128;
  const v = (r - y) / 1.5748 + 128;
  return { y, u, v };
}
// `chromaOf(pid)` is a lookup, not the formula directly — see the fingerprint
// pass below. The closed-form fake-engine formula (predictedChroma) turned
// out to be a poor absolute predictor once real h264 encode + ffmpeg decode
// are in the loop (measured: nearest-match margins as low as ~3 chroma units
// against a formula target, uncomfortably close to noise) — the round trip
// through the compositor's GPU YUV shader, the encoder, and ffmpeg's own
// decode colour matrix each shift the absolute chroma a little. Comparing
// against an EMPIRICALLY MEASURED per-participant fingerprint (recorded
// through the exact same pipeline, in the same file, moments earlier)
// cancels that shift out because it affects the fingerprint and the sample
// identically, and it doesn't touch what's under test (the wall's rect/
// identity mapping) at all.
function nearestParticipant(sampleUv, candidatePids, chromaOf) {
  let best = null;
  let bestDist = Infinity;
  let secondDist = Infinity;
  for (const pid of candidatePids) {
    const p = chromaOf(pid);
    const d = Math.hypot(sampleUv.u - p.u, sampleUv.v - p.v);
    if (d < bestDist) {
      secondDist = bestDist;
      bestDist = d;
      best = pid;
    } else if (d < secondDist) {
      secondDist = d;
    }
  }
  return { pid: best, dist: bestDist, margin: secondDist - bestDist };
}

// ── ffmpeg: crop a small patch at 1 frame and return the per-channel MEDIAN
// (not a single pixel — a single pixel can land on the fake engine's moving
// white band and flap; the brief calls this out explicitly). ──
function samplePatchMedianRgb(videoPath, tSeconds, cxPx, cyPx, sizePx) {
  const half = Math.floor(sizePx / 2);
  const x0 = Math.max(0, Math.round(cxPx - half));
  const y0 = Math.max(0, Math.round(cyPx - half));
  const w = Math.max(2, sizePx);
  const h = Math.max(2, sizePx);
  const result = spawnSync(
    ffmpegBin,
    [
      "-y", "-ss", tSeconds.toFixed(3), "-i", videoPath,
      "-vframes", "1",
      "-vf", `crop=${w}:${h}:${x0}:${y0}`,
      "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1",
    ],
    { encoding: "buffer", maxBuffer: 64 * 1024 * 1024, timeout: 20000 },
  );
  if (result.status !== 0 || !result.stdout || result.stdout.length < w * h * 3) {
    throw new Error(
      `ffmpeg patch sample failed at t=${tSeconds} (${w}x${h}+${x0}+${y0}): ${
        (result.stderr || Buffer.alloc(0)).toString("utf8").slice(-400)
      }`,
    );
  }
  const buf = result.stdout;
  const n = w * h;
  const rs = new Array(n), gs = new Array(n), bs = new Array(n);
  for (let i = 0; i < n; i += 1) {
    rs[i] = buf[i * 3];
    gs[i] = buf[i * 3 + 1];
    bs[i] = buf[i * 3 + 2];
  }
  const median = (arr) => {
    const s = [...arr].sort((a, b) => a - b);
    return s[Math.floor(s.length / 2)];
  };
  return { r: median(rs), g: median(gs), b: median(bs) };
}

function colorClose(a, b, tol) {
  return Math.abs(a.r - b.r) <= tol && Math.abs(a.g - b.g) <= tol && Math.abs(a.b - b.b) <= tol;
}

async function waitForReadableDuration(absPath, maxTries = 60, intervalMs = 250) {
  // `stop-recording-session` returns BEFORE the MP4 moov atom is written, and
  // file size stabilises well before the moov lands — a size-based wait reads
  // an unfinalized file that decodes as ZERO frames (looks exactly like a dead
  // feed). Wait until ffprobe can actually read a duration.
  for (let i = 0; i < maxTries; i += 1) {
    await sleep(intervalMs);
    if (!existsSync(absPath)) continue;
    const probe = spawnSync(
      ffprobeBin,
      ["-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", absPath],
      { encoding: "utf8", timeout: 10000 },
    );
    if (probe.status === 0) {
      const dur = Number.parseFloat((probe.stdout || "").trim());
      if (Number.isFinite(dur) && dur > 0) return dur;
    }
  }
  return null;
}

// Reading order (row-major, top-left first) — same convention
// compositor::solveTilesLayout documents its own output in (TilesLayout.h):
// rects are pushed row by row, left to right within a row. Sorting the
// published tiles.members by (y, x) recovers that order regardless of the
// array's emission order, so this does not depend on trusting sourceId.
function sortReadingOrder(members) {
  const eps = 0.03;
  const rows = [];
  for (const m of members) {
    let row = rows.find((r) => Math.abs(r.y - m.rect.y) < eps);
    if (!row) {
      row = { y: m.rect.y, items: [] };
      rows.push(row);
    }
    row.items.push(m);
  }
  rows.sort((a, b) => a.y - b.y);
  const out = [];
  for (const row of rows) {
    row.items.sort((a, b) => a.rect.x - b.rect.x);
    out.push(...row.items);
  }
  return out;
}

function marginSample(rects) {
  const minX = Math.min(...rects.map((r) => r.x));
  const minY = Math.min(...rects.map((r) => r.y));
  const maxX = Math.max(...rects.map((r) => r.x + r.width));
  const maxY = Math.max(...rects.map((r) => r.y + r.height));
  const candidates = [
    { side: "top", size: minY, x: 0.5, y: minY / 2 },
    { side: "bottom", size: 1 - maxY, x: 0.5, y: maxY + (1 - maxY) / 2 },
    { side: "left", size: minX, x: minX / 2, y: 0.5 },
    { side: "right", size: 1 - maxX, x: maxX + (1 - maxX) / 2, y: 0.5 },
  ];
  candidates.sort((a, b) => b.size - a.size);
  return candidates[0];
}

function gutterSample(rects) {
  let best = null;
  for (let i = 0; i < rects.length; i += 1) {
    for (let j = 0; j < rects.length; j += 1) {
      if (i === j) continue;
      const a = rects[i];
      const b = rects[j];
      const overlapY = Math.min(a.y + a.height, b.y + b.height) - Math.max(a.y, b.y);
      if (overlapY > 0.3 * Math.min(a.height, b.height) && b.x > a.x + a.width) {
        const gap = b.x - (a.x + a.width);
        const midY = Math.max(a.y, b.y) + overlapY / 2;
        const midX = a.x + a.width + gap / 2;
        const cand = { x: midX, y: midY, gap };
        if (!best || cand.gap > best.gap) best = cand;
      }
      const overlapX = Math.min(a.x + a.width, b.x + b.width) - Math.max(a.x, b.x);
      if (overlapX > 0.3 * Math.min(a.width, b.width) && b.y > a.y + a.height) {
        const gap = b.y - (a.y + a.height);
        const midX = Math.max(a.x, b.x) + overlapX / 2;
        const midY = a.y + a.height + gap / 2;
        const cand = { x: midX, y: midY, gap };
        if (!best || cand.gap > best.gap) best = cand;
      }
    }
  }
  return best;
}

const failures = [];
const results = [];
function assertTrue(name, condition, detail) {
  if (condition) {
    results.push(`PASS ${name}`);
    console.log(`[validate-tiles] PASS ${name}${detail ? ` (${detail})` : ""}`);
  } else {
    results.push(`FAIL ${name}`);
    failures.push(`${name}: ${detail ?? "condition false"}`);
    console.error(`[validate-tiles] FAIL ${name}: ${detail ?? "condition false"}`);
  }
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
    COREVIDEO_FAKE_NO_CHURN: "1",
    COREVIDEO_FAKE_ENGINE_PARTICIPANTS: String(participantCount),
  },
  stdio: ["pipe", "pipe", "pipe"],
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let stderrTail = "";
let handshake;
const pending = new Map();

child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString("utf8");
  let idx;
  while ((idx = stdoutBuffer.indexOf("\n")) >= 0) {
    const line = stdoutBuffer.slice(0, idx).trim();
    stdoutBuffer = stdoutBuffer.slice(idx + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch { continue; }
    if (msg.type === "handshake" && msg.ok === true) handshake = msg;
    if (typeof msg.id === "string" && pending.has(msg.id)) {
      const item = pending.get(msg.id);
      pending.delete(msg.id);
      clearTimeout(item.timer);
      item.resolve(msg);
    }
  }
});
child.stderr.on("data", (chunk) => {
  const text = chunk.toString("utf8");
  stderrTail = (stderrTail + text).slice(-8000);
});
child.once("exit", (code) => {
  for (const { reject, timer } of pending.values()) {
    clearTimeout(timer);
    reject(new Error(`native core exited ${code}`));
  }
  pending.clear();
});

function send(type, payload = {}) {
  const id = `tiles-${nextId++}`;
  return new Promise((resolvePromise, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: resolvePromise, reject, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  }).then((response) => {
    if (response.ok === false) throw new Error(`${type} failed: ${response.error?.message ?? "unknown"}`);
    return response;
  });
}
function elapsed() { return Date.now() - startedAt; }

function participantsOf(snapshot) {
  const raw = snapshot?.zoom?.participants ?? snapshot?.participants ?? [];
  return raw
    .map((p) => ({ id: String(p.userId ?? p.id ?? ""), name: String(p.displayName ?? p.name ?? ""), videoOn: p.videoOn !== false }))
    .filter((p) => p.id && p.videoOn)
    .sort((a, b) => Number(a.id) - Number(b.id));
}

function buildSpinePayload(participants) {
  const subscriptions = [
    { participantId: participants[0].id, kind: "meeting-audio", purpose: "program", priority: 0 },
    ...participants.map((p, i) => ({ participantId: p.id, kind: "participant-video", purpose: i === 0 ? "active-speaker" : "program", priority: 10 + i })),
  ];
  return {
    readiness: { status: "ready", platform: process.platform === "darwin" ? "macos" : "windows", sdkVersion: "zoom-engine", checks: [], blockers: [], warnings: [], summary: "tiles pixel oracle" },
    participants: participants.map((p) => ({ sdkUserId: p.id, displayName: p.name, role: "guest", videoOn: true, muted: false, talking: true, audioLevel: 60, networkQuality: "good" })),
    subscriptions,
    startCapture: true,
    blocked: false,
    warnings: [],
    summary: `${participants.length} participants, ${subscriptions.length} subscriptions`,
  };
}

function tilesWallCommand(sceneId, memberIds, tileAspect) {
  return {
    type: "load-scene-graph",
    sceneId,
    routes: [],
    tiles: {
      layerId: "wall",
      order: 0,
      members: memberIds,
      style: {
        tileAspect,
        // Generous gutter/margin (default is ~0.74% ~= 8px at 1080p, too thin
        // to sample reliably against h264 compression bleed). 6% ~= 65px is a
        // legitimate test-fixture choice — it does not change what "correct"
        // means for any assertion, it just gives the oracle room to sample.
        gutterPercent: 6,
        marginPercent: 6,
        backgroundColor,
      },
    },
  };
}

// Fingerprint pass: place each participant at an EXPLICIT, hardcoded rect via
// plain scene routes — NOT the tiles wall, so this never touches
// compositor::solveTilesLayout or the Task 4 expansion loop at all. Position
// here is dictated by the harness, not solved, so "which participant is at
// which rect" is known with certainty and can seed real ground truth.
function fingerprintRoutesCommand(sceneId, participants) {
  const n = participants.length;
  const routes = participants.map((p, i) => ({
    routeId: `fp-${p.id}`,
    mode: "fixed",
    audioRole: "mix",
    participantId: p.id,
    fitMode: "fill",
    rect: { x: i / n, y: 0, width: 1 / n, height: 1 },
  }));
  return { type: "load-scene-graph", sceneId, routes };
}

function tilesNodeOf(snapshot) {
  const node = snapshot?.tiles;
  if (!node || !Array.isArray(node.members)) return [];
  return node.members
    .filter((m) => m.rect && typeof m.rect.width === "number" && typeof m.rect.height === "number")
    .map((m) => ({ sourceId: m.sourceId, rect: { x: m.rect.x, y: m.rect.y, width: m.rect.width, height: m.rect.height } }));
}

const artifacts = [];

try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");
  console.log(`Handshake     : ${handshake.profile?.name ?? "unknown"}`);

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "tiles-pixel-oracle" } });
  console.log("Joined        : fake engine (multi-participant animated I420)");
  await sleep(2500);

  let participants = [];
  for (let attempt = 0; attempt < 15 && participants.length < participantCount; attempt += 1) {
    const snap = (await send("zoom-snapshot")).snapshot;
    participants = participantsOf(snap);
    if (participants.length >= participantCount) {
      await send("zoom-media-spine-sync", { spinePayload: buildSpinePayload(participants), elapsedMs: elapsed() });
      break;
    }
    await sleep(1000);
  }
  if (participants.length < participantCount) {
    throw new Error(`fake engine did not present ${participantCount} video participants (got ${participants.length})`);
  }
  participants = participants.slice(0, participantCount);
  const pids = participants.map((p) => Number(p.id));
  const memberIds = participants.map((p) => `zoom:${p.id}`);
  console.log(`Participants  : ${participants.map((p) => `${p.id}(${p.name})`).join(", ")}`);
  // Let frames actually flow a beat before anything is admitted onto the wall.
  await sleep(1500);

  // Arm recording (video only needed — pixels are the whole point here).
  await send("media-core-sync", {
    elapsedMs: elapsed(),
    commands: [
      { type: "prepare-encoder-session", preparedAtMs: elapsed(), reason: "tiles pixel oracle warmup" },
      { type: "start-program-output", destinations: ["recording"] },
      { type: "set-recording-targets", targetFolder, filenamePrefix: "tiles", format: "mp4", quality: "high" },
    ],
  });
  await sleep(500);

  // ── Phase 0 (fingerprint): each participant at an EXPLICIT, harness-chosen
  // rect via plain scene routes (never the tiles wall / solver). Ground truth
  // for "what does participant P actually look like once encoded, muxed, and
  // decoded back" — measured through the real pipeline so it absorbs whatever
  // colour-space shift the round trip introduces, rather than guessing it. ──
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [fingerprintRoutesCommand("tiles-validate-fingerprint", participants)] });
  await sleep(400);
  await send("media-core-sync", {
    elapsedMs: elapsed(),
    commands: [{ type: "start-recording-session", sessionId: "tiles-pixel-oracle", startedAtMs: Date.now(), targetFolder, filenamePrefix: "tiles", format: "mp4", quality: "high" }],
  });
  const recordStartMs = Date.now();
  console.log(`Recording     : started, phase 0 (fingerprint, explicit rects, no wall)`);
  await sleep(phaseSeconds * 1000);
  const phase0 = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25 };
  console.log(`Phase 0       : @ t=${phase0.tOffsetSec.toFixed(2)}s`);

  // ── Phase A: 16:9 tiles, all N members. Used for per-tile identity +
  // background colour, and as the "before" state for reflow. ──
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [tilesWallCommand("tiles-validate-a", memberIds, "16:9")] });
  await sleep(phaseSeconds * 1000);
  let snap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
  const phaseA = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25, tiles: tilesNodeOf(snap) };
  console.log(`Phase A tiles : ${phaseA.tiles.length} member(s) @ t=${phaseA.tOffsetSec.toFixed(2)}s`);

  // ── Phase B: SAME members, tileAspect switched to 1:1 (mismatched vs the
  // 16:9 source) — fill-not-letterbox check. ──
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [tilesWallCommand("tiles-validate-b", memberIds, "1:1")] });
  await sleep(phaseSeconds * 1000);
  snap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
  const phaseB = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25, tiles: tilesNodeOf(snap) };
  console.log(`Phase B tiles : ${phaseB.tiles.length} member(s) (1:1) @ t=${phaseB.tOffsetSec.toFixed(2)}s`);

  // ── Phase C: drop the LAST member, back to 16:9 — reflow check. ──
  const droppedMemberId = memberIds[memberIds.length - 1];
  const remainingMemberIds = memberIds.slice(0, -1);
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [tilesWallCommand("tiles-validate-c", remainingMemberIds, "16:9")] });
  await sleep(phaseSeconds * 1000);
  snap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
  const phaseC = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25, tiles: tilesNodeOf(snap) };
  console.log(`Phase C tiles : ${phaseC.tiles.length} member(s) (dropped ${droppedMemberId}) @ t=${phaseC.tOffsetSec.toFixed(2)}s`);

  const stopSnap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [{ type: "stop-recording-session", reason: "tiles pixel oracle complete" }] })).snapshot;
  const programPath = stopSnap?.recording?.programPath ??
    (stopSnap?.recording?.streams ?? []).find((s) => s.kind === "program")?.path ?? null;
  if (!programPath) throw new Error("recording snapshot carries no program path");
  const programAbs = isAbsolute(programPath) ? programPath : resolve(buildDir, programPath);
  artifacts.push(programAbs);

  const duration = await waitForReadableDuration(programAbs);
  if (duration == null) throw new Error(`ffprobe never read a duration for ${programAbs} (moov never landed?)`);
  console.log(`Recorded      : ${programAbs} (${duration.toFixed(2)}s, ffprobe-readable)`);

  // Build the empirical fingerprint table from phase 0 (explicit rects, no
  // wall/solver involved) — this is the ground truth every identity
  // assertion below compares against instead of the closed-form formula.
  const fingerprints = new Map();
  for (let i = 0; i < participants.length; i += 1) {
    const pid = pids[i];
    const cx = ((i + 0.5) / participantCount) * canvasWidth;
    const cy = canvasHeight / 2;
    const rgb = samplePatchMedianRgb(programAbs, phase0.tOffsetSec, cx, cy, 40);
    const uv = rgbToYuv(rgb);
    fingerprints.set(pid, uv);
    const formulaUv = predictedChroma(pid);
    console.log(
      `  fingerprint[${pid}] measured rgb=(${rgb.r},${rgb.g},${rgb.b}) uv=(${uv.u.toFixed(1)},${uv.v.toFixed(1)}) ` +
      `vs formula uv=(${formulaUv.u},${formulaUv.v}) [logged for comparison only, not used]`,
    );
  }
  const chromaOf = (pid) => fingerprints.get(pid);

  // ================= Assertion 1: per-tile identity =================
  if (phaseA.tiles.length !== participantCount) {
    failures.push(`phase A tiles.members had ${phaseA.tiles.length}, expected ${participantCount}`);
  }
  const orderedA = sortReadingOrder(phaseA.tiles);
  const identitySamplesA = [];
  for (let k = 0; k < orderedA.length; k += 1) {
    const rect = orderedA[k].rect;
    const cx = (rect.x + rect.width / 2) * canvasWidth;
    const cy = (rect.y + rect.height / 2) * canvasHeight;
    const patch = Math.max(16, Math.min(48, Math.floor(Math.min(rect.width * canvasWidth, rect.height * canvasHeight) * 0.25)));
    const rgb = samplePatchMedianRgb(programAbs, phaseA.tOffsetSec, cx, cy, patch);
    const uv = rgbToYuv(rgb);
    const match = nearestParticipant(uv, pids, chromaOf);
    const expectedPid = pids[k];
    identitySamplesA.push({ k, expectedPid, match, rgb, uv, publishedSourceId: orderedA[k].sourceId });
    console.log(
      `  posA[${k}] expect=${expectedPid} publishedSourceId=${orderedA[k].sourceId} ` +
      `rgb=(${rgb.r},${rgb.g},${rgb.b}) uv=(${uv.u.toFixed(1)},${uv.v.toFixed(1)}) ` +
      `nearest=${match.pid} dist=${match.dist.toFixed(1)} margin=${match.margin.toFixed(1)}`,
    );
  }
  const identityCorrect = identitySamplesA.every((s) => s.match.pid === s.expectedPid);
  const identityDistinct = new Set(identitySamplesA.map((s) => s.match.pid)).size === identitySamplesA.length;
  assertTrue(
    "per-tile identity",
    identityCorrect && identityDistinct,
    identityCorrect
      ? (identityDistinct ? "every rect's pixel content matched its expected participant, all distinct" : "matches correct but two slots resolved to the same participant")
      : `mismatch: ${identitySamplesA.filter((s) => s.match.pid !== s.expectedPid).map((s) => `pos${s.k} expected ${s.expectedPid} got ${s.match.pid}`).join("; ")}`,
  );

  // ================= Assertion 2: background colour =================
  const rectsA = phaseA.tiles.map((m) => m.rect);
  const margin = marginSample(rectsA);
  const gutter = gutterSample(rectsA);
  let bgOk = true;
  const bgDetails = [];
  if (margin && margin.size > 0.005) {
    const patch = Math.max(4, Math.min(20, Math.floor(margin.size * canvasHeight * 0.5)));
    const rgb = samplePatchMedianRgb(programAbs, phaseA.tOffsetSec, margin.x * canvasWidth, margin.y * canvasHeight, patch);
    const ok = colorClose(rgb, bgRgb, 24);
    bgOk = bgOk && ok;
    bgDetails.push(`margin(${margin.side})=rgb(${rgb.r},${rgb.g},${rgb.b}) vs bg(${bgRgb.r},${bgRgb.g},${bgRgb.b}) -> ${ok ? "match" : "MISMATCH"}`);
  } else {
    bgOk = false;
    bgDetails.push("no usable margin band found");
  }
  if (gutter && gutter.gap > 0.01) {
    const patch = Math.max(4, Math.min(20, Math.floor(Math.min(gutter.gap * canvasWidth, gutter.gap * canvasHeight) * 0.5)));
    const rgb = samplePatchMedianRgb(programAbs, phaseA.tOffsetSec, gutter.x * canvasWidth, gutter.y * canvasHeight, patch);
    const ok = colorClose(rgb, bgRgb, 24);
    bgOk = bgOk && ok;
    bgDetails.push(`gutter=rgb(${rgb.r},${rgb.g},${rgb.b}) vs bg(${bgRgb.r},${bgRgb.g},${bgRgb.b}) -> ${ok ? "match" : "MISMATCH"}`);
  } else {
    bgDetails.push("no usable inter-tile gutter found (single row/col layout — margin alone is sufficient)");
  }
  assertTrue("background colour", bgOk, bgDetails.join("; "));

  // ================= Assertion 3: fill, not letterbox =================
  const rectsB = phaseB.tiles.map((m) => m.rect);
  if (rectsB.length === 0) throw new Error("phase B published no tiles");
  const probeRect = sortReadingOrder(phaseB.tiles)[0].rect;
  const cyB = (probeRect.y + probeRect.height / 2) * canvasHeight;
  const inset = 4;
  const leftX = probeRect.x * canvasWidth + inset;
  const rightX = (probeRect.x + probeRect.width) * canvasWidth - inset;
  const leftRgb = samplePatchMedianRgb(programAbs, phaseB.tOffsetSec, leftX, cyB, 6);
  const rightRgb = samplePatchMedianRgb(programAbs, phaseB.tOffsetSec, rightX, cyB, 6);
  const leftIsBg = colorClose(leftRgb, bgRgb, 20);
  const rightIsBg = colorClose(rightRgb, bgRgb, 20);
  assertTrue(
    "fill, not letterbox",
    !leftIsBg && !rightIsBg,
    `1:1 tile vs 16:9 source, left-edge rgb=(${leftRgb.r},${leftRgb.g},${leftRgb.b}) right-edge rgb=(${rightRgb.r},${rightRgb.g},${rightRgb.b}) ` +
      `(bg would be ~(${bgRgb.r},${bgRgb.g},${bgRgb.b})) -> ${leftIsBg || rightIsBg ? "letterbox bar detected" : "content reaches both edges"}`,
  );

  // ================= Assertion 4: reflow after departure =================
  if (phaseC.tiles.length !== participantCount - 1) {
    failures.push(`phase C tiles.members had ${phaseC.tiles.length}, expected ${participantCount - 1}`);
  }
  const orderedC = sortReadingOrder(phaseC.tiles);
  const remainingPids = pids.slice(0, -1);
  let reflowLarger = true;
  let reflowIdentity = true;
  const reflowDetails = [];
  for (let k = 0; k < orderedC.length && k < orderedA.length; k += 1) {
    const beforeArea = orderedA[k].rect.width * orderedA[k].rect.height;
    const afterArea = orderedC[k].rect.width * orderedC[k].rect.height;
    const larger = afterArea > beforeArea * 1.02; // must be MEANINGFULLY larger, not float noise
    reflowLarger = reflowLarger && larger;
    const rect = orderedC[k].rect;
    const cx = (rect.x + rect.width / 2) * canvasWidth;
    const cy = (rect.y + rect.height / 2) * canvasHeight;
    const patch = Math.max(16, Math.min(48, Math.floor(Math.min(rect.width * canvasWidth, rect.height * canvasHeight) * 0.25)));
    const rgb = samplePatchMedianRgb(programAbs, phaseC.tOffsetSec, cx, cy, patch);
    const uv = rgbToYuv(rgb);
    const match = nearestParticipant(uv, pids, chromaOf);
    const expectedPid = remainingPids[k];
    const identOk = match.pid === expectedPid;
    reflowIdentity = reflowIdentity && identOk;
    reflowDetails.push(
      `posC[${k}] area ${beforeArea.toFixed(4)}->${afterArea.toFixed(4)} (${larger ? "larger" : "NOT larger"}), ` +
      `identity expected=${expectedPid} got=${match.pid} (${identOk ? "ok" : "MISMATCH"})`,
    );
  }
  console.log(`  ${reflowDetails.join("\n  ")}`);
  assertTrue(
    "reflow after departure",
    phaseC.tiles.length === participantCount - 1 && reflowLarger && reflowIdentity,
    `${orderedC.length} tiles remain (dropped ${droppedMemberId}); ` +
      `${reflowLarger ? "all grew" : "did NOT grow"}; identity ${reflowIdentity ? "held" : "broke"}`,
  );

  console.log("");
  console.log(`Result        : ${results.join(", ")}`);
  if (failures.length === 0) {
    console.log("TILES PIXEL ORACLE: PASS");
  } else {
    console.log("TILES PIXEL ORACLE: FAIL");
    for (const f of failures) console.log(`  - ${f}`);
  }
} catch (error) {
  failures.push(error instanceof Error ? error.message : String(error));
  console.error("TILES PIXEL ORACLE: FAIL —", failures[failures.length - 1]);
  if (stderrTail.trim()) {
    console.error("--- core stderr (tail) ---");
    console.error(stderrTail.trim().split("\n").slice(-40).join("\n"));
  }
} finally {
  child.kill();
  if (!keepArtifacts) {
    for (const abs of artifacts) {
      try { if (existsSync(abs)) rmSync(abs); } catch { /* best-effort */ }
    }
  }
}
process.exit(failures.length === 0 ? 0 : 1);
