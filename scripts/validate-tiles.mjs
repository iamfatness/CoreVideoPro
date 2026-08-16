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
 *     routes, never the tiles wall/solver) via a REJECTING classifier — a
 *     match is only accepted within a max distance AND a minimum margin over
 *     the runner-up, so background/placeholder colour can never misclassify
 *     as a participant (see identifyParticipant).
 *   - separately proves GEOMETRY, not just identity: `assertEdgeTransition`
 *     walks a scanline across a published tile boundary and asserts the real
 *     content->background pixel transition lands within a few px of the rect
 *     edge the snapshot claims — this is what catches a tile that is the
 *     right shape/colour but the wrong SIZE or POSITION (a uniformly-shrunk
 *     or offset wall), which sampling only tile centres and gutter centres
 *     can never see.
 *
 * This is why the deliberate-break in Step 3 (reversing `admitted` right
 * before the rect-assignment loop) is detectable at all: the reversal keeps
 * `sourceId`/`rect`/actual-drawn-frame mutually consistent (self-referential),
 * but it breaks the relationship between MY sent order and the geometric
 * position — which is exactly what this oracle checks.
 *
 * Scope (T1 only — see task-6-brief.md): implements per-tile identity,
 * background colour (at both a generous test fixture spacing AND the real
 * shipping default spacing), tile-boundary geometry, fill-not-letterbox (all
 * four edges), and reflow after a genuine staleness departure (a participant's
 * video subscription is dropped while they REMAIN in tiles.members, so the
 * admission gate's frameId-freshness tracker — not membership editing — is
 * what has to fire). Glow is T2 (not built yet, nothing to sample). Byte-
 * identical-at-rest is skipped: T1 has no animation toggle to hold constant
 * against, so there is nothing to freeze and compare.
 *
 * Usage: node scripts/validate-tiles.mjs [--participants 3] [--build-dir DIR]
 *          [--phase-seconds 3] [--keep-artifacts]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, rmSync } from "node:fs";
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

// Placeholders — replaced with the RECORDED file's real dimensions (via
// ffprobe) once it exists, never trusted as a hardcoded assumption for pixel
// math. See probeVideoDims() below.
let canvasWidth = 1920;
let canvasHeight = 1080;
const backgroundColor = "#101418";
const bgRgb = { r: 0x10, g: 0x14, b: 0x18 };
const targetFolder = "Recordings/CoreVideoPro/validate-tiles";
// Mirrors compositor::kTilesStaleFrameMs (native/src/compositor/TilesMembership.h).
// Not re-implemented logic — just the budget the reflow test has to wait past.
const kTilesStaleFrameMs = 1500;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ── Ground-truth participant colour model (from fake-engine.cpp, NOT the wall
// solver under test — this is a completely separate subsystem: the synthetic
// video generator's per-participant chroma formula). Held constant across the
// WHOLE frame per participant (memset), so we never need to predict luma.
// Logged for comparison only — see the fingerprint pass for what's actually used. ──
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
// `chromaOf(pid)` is a lookup into the empirical fingerprint table (built
// from phase 0 — each participant at an EXPLICIT rect, never the tiles wall)
// rather than the closed-form formula: the real encode/decode round trip
// shifts absolute chroma enough that a formula-only prediction left
// nearest-match margins uncomfortably close to noise when this was first
// measured against the real pipeline.
//
// This is a REJECTING classifier, not a bare nearest-neighbour: a match is
// only accepted if it is within `maxDist` of its fingerprint AND beats the
// runner-up by at least `minMargin`. Without a reject option, `nearestParticipant`
// always returns SOME candidate — converting the wall background colour
// through this same recovery lands ~15 units from a real participant's
// fingerprint, well inside "plausible" for a bare nearest-neighbour with no
// ceiling. maxDist/minMargin are set from this script's own measured healthy
// separations (correct matches: dist 0.0-3.2; cross-participant separation:
// 18.1-37.2) with real margin to spare on both sides.
function identifyParticipant(sampleUv, candidatePids, chromaOf, { maxDist = 8, minMargin = 6 } = {}) {
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
  const margin = secondDist - bestDist;
  const accepted = best !== null && bestDist <= maxDist && margin >= minMargin;
  return { pid: accepted ? best : null, rawNearest: best, dist: bestDist, margin, accepted };
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
  const buf = ffmpegRawCrop(videoPath, tSeconds, x0, y0, w, h);
  const n = w * h;
  const rs = new Array(n), gs = new Array(n), bs = new Array(n);
  for (let i = 0; i < n; i += 1) {
    rs[i] = buf[i * 3];
    gs[i] = buf[i * 3 + 1];
    bs[i] = buf[i * 3 + 2];
  }
  return { r: median(rs), g: median(gs), b: median(bs) };
}

function median(arr) {
  const s = [...arr].sort((a, b) => a - b);
  return s[Math.floor(s.length / 2)];
}

function ffmpegRawCrop(videoPath, tSeconds, x0, y0, w, h) {
  const result = spawnSync(
    ffmpegBin,
    [
      "-y", "-ss", tSeconds.toFixed(3), "-i", videoPath,
      "-vframes", "1",
      "-vf", `crop=${w}:${h}:${Math.max(0, x0)}:${Math.max(0, y0)}`,
      "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1",
    ],
    { encoding: "buffer", maxBuffer: 64 * 1024 * 1024, timeout: 20000 },
  );
  if (result.status !== 0 || !result.stdout || result.stdout.length < w * h * 3) {
    throw new Error(
      `ffmpeg crop failed at t=${tSeconds} (${w}x${h}+${x0}+${y0}): ${
        (result.stderr || Buffer.alloc(0)).toString("utf8").slice(-400)
      }`,
    );
  }
  return result.stdout;
}

// Per-COLUMN (vertical-boundary scan) or per-ROW (horizontal-boundary scan)
// medians across a thin strip — used by assertEdgeTransition to find exactly
// where a tile's real pixel content stops and the background starts, so it
// can be compared to the rect edge the snapshot claims.
function sampleColumnMedians(videoPath, tSeconds, x0, y0, w, h) {
  const buf = ffmpegRawCrop(videoPath, tSeconds, x0, y0, w, h);
  const cols = [];
  for (let c = 0; c < w; c += 1) {
    const rs = [], gs = [], bs = [];
    for (let r = 0; r < h; r += 1) {
      const idx = (r * w + c) * 3;
      rs.push(buf[idx]); gs.push(buf[idx + 1]); bs.push(buf[idx + 2]);
    }
    cols.push({ r: median(rs), g: median(gs), b: median(bs) });
  }
  return cols;
}
function sampleRowMedians(videoPath, tSeconds, x0, y0, w, h) {
  const buf = ffmpegRawCrop(videoPath, tSeconds, x0, y0, w, h);
  const rows = [];
  for (let r = 0; r < h; r += 1) {
    const rs = [], gs = [], bs = [];
    for (let c = 0; c < w; c += 1) {
      const idx = (r * w + c) * 3;
      rs.push(buf[idx]); gs.push(buf[idx + 1]); bs.push(buf[idx + 2]);
    }
    rows.push({ r: median(rs), g: median(gs), b: median(bs) });
  }
  return rows;
}

function colorClose(a, b, tol) {
  return Math.abs(a.r - b.r) <= tol && Math.abs(a.g - b.g) <= tol && Math.abs(a.b - b.b) <= tol;
}

// Finds the sample index (as a half-integer between two samples) where
// classification flips background<->content, searching outward from the
// middle of the strip (the expected edge is centred in the scan window).
function findTransitionIndex(samples, tol) {
  const isBg = samples.map((s) => colorClose(s, bgRgb, tol));
  const mid = Math.floor(samples.length / 2);
  for (let d = 0; d <= samples.length; d += 1) {
    for (const i of [mid - d, mid + d]) {
      if (i > 0 && i < samples.length && isBg[i] !== isBg[i - 1]) return i - 0.5;
    }
  }
  return null;
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

function probeVideoDims(absPath) {
  const probe = spawnSync(
    ffprobeBin,
    ["-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=s=x:p=0", absPath],
    { encoding: "utf8", timeout: 10000 },
  );
  if (probe.status === 0) {
    const m = /^(\d+)x(\d+)/.exec((probe.stdout || "").trim());
    if (m) return { width: Number(m[1]), height: Number(m[2]) };
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

// Sample point: tile centre.
//
// An off-centre (25%/25%) sample point was tried first, on the theory that
// the fake engine's luma discontinuity on the diagonal through the centre
// (base + ((xterm+yterm)&0xFF)/2, which clips some pids to Y=255 exactly at
// centre) would corrupt recovered chroma there. Measured against THIS
// script's actual empirical-fingerprint design, moving off-centre made
// separation WORSE, not better (dist rose from 0.0-3.2 to 9.8-26.5) — because
// the fingerprint routes (full-height 1/N-wide columns, phase 0) and a
// solved tile rect (roughly square) have different aspect ratios, so "25%
// inset" lands at a different relative position in the SOURCE frame for the
// two passes, and the chroma-recovery error introduced by lossy h264 +
// inverse-YUV rounding turns out to be position-dependent enough that this
// geometry mismatch matters more than the centre-diagonal clipping does.
// The reviewer's concern targeted a CLOSED-FORM formula predictor (which
// really does need to dodge Y-clipping, since it predicts luma from
// nothing); the empirical fingerprint sidesteps that concern by construction
// — chroma recovery is Y-independent — so centre sampling, MEASURED, is the
// better choice for this specific design. Keeping this decision (and the
// measurement behind it) here rather than silently reverting without a trace.
function tileSamplePoint(rect) {
  return { x: (rect.x + rect.width * 0.5) * canvasWidth, y: (rect.y + rect.height * 0.5) * canvasHeight };
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

// Finds the widest inter-tile gutter and returns BOTH a simple midpoint
// sample (for the background-colour check) and the two boundary edges +
// scan axis (for assertEdgeTransition's geometry check). Returns null only
// when tiles genuinely touch/overlap with no gap at all on either axis —
// callers decide whether that is itself a failure for their layout.
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
        const cand = { axis: "x", gap, midX, midY, edgeNear: a.x + a.width, edgeFar: b.x, perp: midY };
        if (!best || cand.gap > best.gap) best = cand;
      }
      const overlapX = Math.min(a.x + a.width, b.x + b.width) - Math.max(a.x, b.x);
      if (overlapX > 0.3 * Math.min(a.width, b.width) && b.y > a.y + a.height) {
        const gap = b.y - (a.y + a.height);
        const midX = Math.max(a.x, b.x) + overlapX / 2;
        const midY = a.y + a.height + gap / 2;
        const cand = { axis: "y", gap, midX, midY, edgeNear: a.y + a.height, edgeFar: b.y, perp: midX };
        if (!best || cand.gap > best.gap) best = cand;
      }
    }
  }
  return best;
}

// Walks a scanline across BOTH edges of a gutter pair (the near tile's far
// edge, and the far tile's near edge) and asserts the real content<->background
// pixel transition lands within `tolPx` of the rect edge the snapshot
// published. This is the assertion that catches a tile drawn the right
// colour in the right general area but the WRONG SIZE/POSITION — sampling
// only tile centres (identity) and gutter centres (background colour) can
// never see a uniformly-shrunk or offset wall, because a shrunk tile's
// centre is still deep inside real content and a shrunk gutter's centre is
// still deep inside real background.
function measureEdgeTransition(videoPath, tSeconds, gutter) {
  const stripThicknessPx = 6;
  const axisSpanPx = gutter.axis === "x" ? canvasWidth : canvasHeight;
  const windowPx = Math.max(14, Math.min(30, Math.floor(gutter.gap * axisSpanPx * 0.4)));
  const measureOne = (edgeFrac) => {
    if (gutter.axis === "x") {
      const edgePx = edgeFrac * canvasWidth;
      const yPx = Math.max(0, Math.round(gutter.perp * canvasHeight - stripThicknessPx / 2));
      const x0 = Math.round(edgePx - windowPx);
      const cols = sampleColumnMedians(videoPath, tSeconds, x0, yPx, windowPx * 2, stripThicknessPx);
      const idx = findTransitionIndex(cols, 24);
      return { edgePx, foundPx: idx == null ? null : x0 + idx };
    }
    const edgePx = edgeFrac * canvasHeight;
    const xPx = Math.max(0, Math.round(gutter.perp * canvasWidth - stripThicknessPx / 2));
    const y0 = Math.round(edgePx - windowPx);
    const rows = sampleRowMedians(videoPath, tSeconds, xPx, y0, stripThicknessPx, windowPx * 2);
    const idx = findTransitionIndex(rows, 24);
    return { edgePx, foundPx: idx == null ? null : y0 + idx };
  };
  return { near: measureOne(gutter.edgeNear), far: measureOne(gutter.edgeFar) };
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

// AUTOSUBSCRIBE IS OFF. With it on, the fake engine keeps an independent
// per-participant video stream alive regardless of what THIS harness
// subscribes/unsubscribes (it auto-creates "participant-video-<id>-auto"
// targets for every video-on roster participant once, at join, and
// COREVIDEO_FAKE_NO_CHURN=1 stops the periodic re-sync that would otherwise
// keep re-adding them) — so an explicit unsubscribe from the core would stop
// only ITS OWN target while the auto stream kept feeding frames for the same
// participantId, and the wall would never see a stale/frozen feed at all.
// Turning autosubscribe off makes this harness's own explicit
// zoom-media-spine-sync subscriptions the ONLY source of frames, so dropping
// one is a genuine, controllable way to freeze a participant's feed without
// touching tiles.members — see the reflow-after-departure phase below.
const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
    COREVIDEO_FAKE_NO_CHURN: "1",
    COREVIDEO_FAKE_ENGINE_PARTICIPANTS: String(participantCount),
    COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE: "0",
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

// `excludeVideoId`, when set, omits that participant's participant-video
// subscription while KEEPING them in the `participants` roster array — a
// guest who is still in the call but whose camera feed has stopped, not a
// guest who left. That is what drives the wall's genuine staleness path
// (frameId stops advancing) rather than a membership edit.
function buildSpinePayload(participants, excludeVideoId = null) {
  const subscriptions = [
    { participantId: participants[0].id, kind: "meeting-audio", purpose: "program", priority: 0 },
    ...participants
      .filter((p) => p.id !== excludeVideoId)
      .map((p, i) => ({ participantId: p.id, kind: "participant-video", purpose: i === 0 ? "active-speaker" : "program", priority: 10 + i })),
  ];
  return {
    readiness: { status: "ready", platform: process.platform === "darwin" ? "macos" : "windows", sdkVersion: "zoom-engine", checks: [], blockers: [], warnings: [], summary: "tiles pixel oracle" },
    participants: participants.map((p) => ({ sdkUserId: p.id, displayName: p.name, role: "guest", videoOn: true, muted: false, talking: true, audioLevel: 60, networkQuality: "good" })),
    subscriptions,
    startCapture: true,
    blocked: false,
    warnings: [],
    summary: `${participants.length} participants, ${subscriptions.length} subscriptions${excludeVideoId ? ` (video withheld from ${excludeVideoId})` : ""}`,
  };
}

// styleOverrides defaults to a generous 6% gutter/margin (~65px at 1080p) —
// the wall ships at 0.741% (~8px), too thin to sample reliably against h264
// compression bleed at tile boundaries. This does not change what "correct"
// means for any assertion, it just gives the oracle room to sample. Pass {}
// to get the REAL shipping defaults (used by the dedicated default-spacing
// phase below, so the thin-gutter case is exercised too, not just the fixture).
function tilesWallCommand(sceneId, memberIds, tileAspect, styleOverrides = { gutterPercent: 6, marginPercent: 6 }) {
  return {
    type: "load-scene-graph",
    sceneId,
    routes: [],
    tiles: {
      layerId: "wall",
      order: 0,
      members: memberIds,
      style: { tileAspect, backgroundColor, ...styleOverrides },
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
  console.log(`Participants  : ${participants.map((p) => `${p.id}(${p.name})`).join(", ")} (autosubscribe OFF — explicit subscriptions only)`);
  // Let frames actually flow a beat before anything is admitted onto the wall.
  await sleep(2000);

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

  // ── Phase A: 16:9 tiles, all N members. Used for per-tile identity,
  // background colour, and the edge-transition geometry check. ──
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [tilesWallCommand("tiles-validate-a", memberIds, "16:9")] });
  await sleep(phaseSeconds * 1000);
  let snap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
  const phaseA = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25, tiles: tilesNodeOf(snap) };
  console.log(`Phase A tiles : ${phaseA.tiles.length} member(s) @ t=${phaseA.tOffsetSec.toFixed(2)}s`);

  // ── Phase B: SAME members, tileAspect switched to 1:1 (mismatched vs the
  // 16:9 source) — fill-not-letterbox check, all four edges. ──
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [tilesWallCommand("tiles-validate-b", memberIds, "1:1")] });
  await sleep(phaseSeconds * 1000);
  snap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
  const phaseB = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25, tiles: tilesNodeOf(snap) };
  console.log(`Phase B tiles : ${phaseB.tiles.length} member(s) (1:1) @ t=${phaseB.tOffsetSec.toFixed(2)}s`);

  // ── Phase C: reflow after a GENUINE STALENESS departure. Back to 16:9,
  // ALL N members STILL listed in tiles.members (never edited) — instead the
  // last participant's video SUBSCRIPTION is dropped, which (autosubscribe
  // off) genuinely stops new frames for them. The wall's own admission gate
  // (compositor::admitTilesMembers + the frameId-freshness tracker at
  // MediaCore.cpp ~4829-4911) has to notice the frozen feed and exclude them
  // after kTilesStaleFrameMs — this is the path that previously could never
  // fire at all (see CLAUDE.md's frame-freshness history). ──
  const droppedMemberId = memberIds[memberIds.length - 1];
  const droppedParticipant = participants[participants.length - 1];
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [tilesWallCommand("tiles-validate-c", memberIds, "16:9")] });
  await sleep(500);
  const unsubscribeSentAtMs = Date.now();
  await send("zoom-media-spine-sync", { spinePayload: buildSpinePayload(participants, droppedParticipant.id), elapsedMs: elapsed() });
  console.log(`Staleness     : withheld video subscription for ${droppedMemberId} at t=${((unsubscribeSentAtMs - recordStartMs) / 1000).toFixed(2)}s, polling for reflow...`);

  let staleDropObservedAtMs = null;
  const staleDeadlineMs = unsubscribeSentAtMs + Math.max(6000, kTilesStaleFrameMs * 3);
  while (Date.now() < staleDeadlineMs) {
    await sleep(200);
    const pollSnap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
    const count = tilesNodeOf(pollSnap).length;
    if (count === participantCount - 1) {
      staleDropObservedAtMs = Date.now();
      break;
    }
  }
  const staleElapsedMs = staleDropObservedAtMs == null ? null : staleDropObservedAtMs - unsubscribeSentAtMs;
  if (staleElapsedMs == null) {
    console.log(`Staleness     : tiles.members never dropped to ${participantCount - 1} within ${(staleDeadlineMs - unsubscribeSentAtMs)}ms`);
  } else {
    console.log(`Staleness     : tiles.members dropped ${participantCount}->${participantCount - 1} after ${staleElapsedMs}ms (kTilesStaleFrameMs=${kTilesStaleFrameMs})`);
  }
  // A little settle time past the observed transition so the sampled frame
  // is comfortably inside the post-reflow state, not right at the edge.
  await sleep(800);
  snap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
  const phaseC = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25, tiles: tilesNodeOf(snap) };
  console.log(`Phase C tiles : ${phaseC.tiles.length} member(s) (${droppedMemberId} stale) @ t=${phaseC.tOffsetSec.toFixed(2)}s`);

  // ── Phase D: SAME staleness state (member never re-subscribed), but at the
  // REAL SHIPPING DEFAULT gutter/margin (0.741%, ~8px at 1080p) instead of
  // this script's generous 6% test fixture — proves the background/gutter
  // colour is right at the spacing the product actually ships, not just at
  // a spacing chosen for sampling convenience. ──
  await send("media-core-sync", { elapsedMs: elapsed(), commands: [tilesWallCommand("tiles-validate-d", memberIds, "16:9", {})] });
  await sleep(phaseSeconds * 1000);
  snap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [] })).snapshot;
  const phaseD = { tOffsetSec: (Date.now() - recordStartMs) / 1000 - 0.25, tiles: tilesNodeOf(snap) };
  console.log(`Phase D tiles : ${phaseD.tiles.length} member(s) (default spacing) @ t=${phaseD.tOffsetSec.toFixed(2)}s`);

  const stopSnap = (await send("media-core-sync", { elapsedMs: elapsed(), commands: [{ type: "stop-recording-session", reason: "tiles pixel oracle complete" }] })).snapshot;
  const programPath = stopSnap?.recording?.programPath ??
    (stopSnap?.recording?.streams ?? []).find((s) => s.kind === "program")?.path ?? null;
  if (!programPath) throw new Error("recording snapshot carries no program path");
  const programAbs = isAbsolute(programPath) ? programPath : resolve(buildDir, programPath);
  artifacts.push(programAbs);

  const duration = await waitForReadableDuration(programAbs);
  if (duration == null) throw new Error(`ffprobe never read a duration for ${programAbs} (moov never landed?)`);
  console.log(`Recorded      : ${programAbs} (${duration.toFixed(2)}s, ffprobe-readable)`);

  const dims = probeVideoDims(programAbs);
  if (dims) {
    canvasWidth = dims.width;
    canvasHeight = dims.height;
  }
  console.log(`Canvas dims   : ${canvasWidth}x${canvasHeight} (${dims ? "from ffprobe" : "ffprobe unavailable, using default"})`);

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
    fingerprints.set(pid, { rgb, uv });
    const formulaUv = predictedChroma(pid);
    console.log(
      `  fingerprint[${pid}] measured rgb=(${rgb.r},${rgb.g},${rgb.b}) uv=(${uv.u.toFixed(1)},${uv.v.toFixed(1)}) ` +
      `vs formula uv=(${formulaUv.u},${formulaUv.v}) [logged for comparison only, not used]`,
    );
  }
  const chromaOf = (pid) => fingerprints.get(pid).uv;

  // ================= Precondition: fingerprint sanity =================
  // A degenerate phase 0 (fingerprints too close together, or one reading as
  // background) would otherwise surface downstream as a confusing identity
  // mismatch with no obvious cause. Check it explicitly and fail LOUD here.
  {
    const problems = [];
    for (const pid of pids) {
      const fp = fingerprints.get(pid);
      if (colorClose(fp.rgb, bgRgb, 24)) {
        problems.push(`fingerprint[${pid}] reads as background: rgb=(${fp.rgb.r},${fp.rgb.g},${fp.rgb.b})`);
      }
    }
    const minSeparation = 12;
    for (let i = 0; i < pids.length; i += 1) {
      for (let j = i + 1; j < pids.length; j += 1) {
        const a = fingerprints.get(pids[i]).uv, b = fingerprints.get(pids[j]).uv;
        const d = Math.hypot(a.u - b.u, a.v - b.v);
        if (d < minSeparation) problems.push(`fingerprint[${pids[i]}] vs [${pids[j]}] too close: ${d.toFixed(1)} < ${minSeparation}`);
      }
    }
    assertTrue("fingerprint sanity", problems.length === 0, problems.length === 0 ? `${pids.length} fingerprints, all distinct and non-background` : problems.join("; "));
    if (problems.length > 0) {
      throw new Error("fingerprint sanity failed — every downstream identity assertion would be unreliable, stopping here");
    }
  }

  // ================= Assertion: per-tile identity =================
  const countOkA = phaseA.tiles.length === participantCount;
  const orderedA = sortReadingOrder(phaseA.tiles);
  const identitySamplesA = [];
  if (countOkA) {
    for (let k = 0; k < orderedA.length; k += 1) {
      const rect = orderedA[k].rect;
      const { x: cx, y: cy } = tileSamplePoint(rect);
      const patch = Math.max(16, Math.min(48, Math.floor(Math.min(rect.width * canvasWidth, rect.height * canvasHeight) * 0.25)));
      const rgb = samplePatchMedianRgb(programAbs, phaseA.tOffsetSec, cx, cy, patch);
      const uv = rgbToYuv(rgb);
      const match = identifyParticipant(uv, pids, chromaOf);
      const expectedPid = pids[k];
      const isBackground = colorClose(rgb, bgRgb, 24);
      identitySamplesA.push({ k, expectedPid, match, rgb, uv, isBackground, publishedSourceId: orderedA[k].sourceId });
      console.log(
        `  posA[${k}] expect=${expectedPid} publishedSourceId=${orderedA[k].sourceId} ` +
        `rgb=(${rgb.r},${rgb.g},${rgb.b}) uv=(${uv.u.toFixed(1)},${uv.v.toFixed(1)}) ` +
        `identified=${match.pid ?? "REJECTED"} (nearest=${match.rawNearest} dist=${match.dist.toFixed(1)} margin=${match.margin.toFixed(1)})`,
      );
    }
  }
  const identityCorrect = identitySamplesA.every((s) => s.match.accepted && s.match.pid === s.expectedPid);
  const identityDistinct = new Set(identitySamplesA.map((s) => s.match.pid)).size === identitySamplesA.length;
  const noneBackground = identitySamplesA.every((s) => !s.isBackground);
  assertTrue(
    "per-tile identity",
    countOkA && identityCorrect && identityDistinct && noneBackground,
    !countOkA
      ? `expected ${participantCount} published tiles, got ${phaseA.tiles.length} — no samples taken`
      : (identityCorrect && identityDistinct && noneBackground
        ? "every rect's pixel content matched its expected participant (confident, non-background), all distinct"
        : [
            !identityCorrect ? `mismatch/rejected: ${identitySamplesA.filter((s) => !(s.match.accepted && s.match.pid === s.expectedPid)).map((s) => `pos${s.k} expected ${s.expectedPid} got ${s.match.pid ?? "REJECTED(nearest=" + s.match.rawNearest + ")"}`).join("; ")}` : null,
            !identityDistinct ? "two slots resolved to the same participant" : null,
            !noneBackground ? "a tile centre read as background colour" : null,
          ].filter(Boolean).join("; ")),
  );

  // ================= Assertion: background colour (6% fixture) =================
  const rectsA = phaseA.tiles.map((m) => m.rect);
  const marginA = countOkA ? marginSample(rectsA) : null;
  const gutterA = countOkA ? gutterSample(rectsA) : null;
  let bgOk = countOkA;
  const bgDetails = [];
  if (!countOkA) bgDetails.push(`skipped — phase A tile count was ${phaseA.tiles.length}, expected ${participantCount}`);
  if (marginA && marginA.size > 0.005) {
    const patch = Math.max(4, Math.min(20, Math.floor(marginA.size * canvasHeight * 0.5)));
    const rgb = samplePatchMedianRgb(programAbs, phaseA.tOffsetSec, marginA.x * canvasWidth, marginA.y * canvasHeight, patch);
    const ok = colorClose(rgb, bgRgb, 24);
    bgOk = bgOk && ok;
    bgDetails.push(`margin(${marginA.side})=rgb(${rgb.r},${rgb.g},${rgb.b}) vs bg(${bgRgb.r},${bgRgb.g},${bgRgb.b}) -> ${ok ? "match" : "MISMATCH"}`);
  } else if (countOkA) {
    bgOk = false;
    bgDetails.push("no usable margin band found");
  }
  // A gutter is REQUIRED here, not optional: this fixture's 6% spacing with
  // participantCount tiles solves to a multi-row/column grid (verified for
  // N=3: 2x2), so a missing gutter means tiles are packed edge-to-edge or
  // overlapping — a real defect, not a benign "single row" layout. A wall
  // shaped so no gutter can ever exist would need a different check; this
  // fixture is not that shape.
  if (gutterA && gutterA.gap > 0.01) {
    const patch = Math.max(4, Math.min(20, Math.floor(Math.min(gutterA.gap * canvasWidth, gutterA.gap * canvasHeight) * 0.5)));
    const rgb = samplePatchMedianRgb(programAbs, phaseA.tOffsetSec, gutterA.midX * canvasWidth, gutterA.midY * canvasHeight, patch);
    const ok = colorClose(rgb, bgRgb, 24);
    bgOk = bgOk && ok;
    bgDetails.push(`gutter=rgb(${rgb.r},${rgb.g},${rgb.b}) vs bg(${bgRgb.r},${bgRgb.g},${bgRgb.b}) -> ${ok ? "match" : "MISMATCH"}`);
  } else if (countOkA) {
    bgOk = false;
    bgDetails.push("no usable inter-tile gutter found — expected one at this fixture's spacing/count (tiles touching or overlapping?)");
  }
  assertTrue("background colour (6% fixture)", bgOk, bgDetails.join("; "));

  // ================= Assertion: tile boundary matches published rect =================
  // Geometry check, not identity or colour: walks a scanline across the SAME
  // gutter pair used above and asserts the content<->background transition
  // is within tolerance of the rect edges the snapshot published. Catches a
  // tile that is the right colour in the right place but the wrong SIZE.
  if (countOkA && gutterA && gutterA.gap > 0.01) {
    const tolPx = 10;
    const edgeMeasure = measureEdgeTransition(programAbs, phaseA.tOffsetSec, gutterA);
    const nearErr = edgeMeasure.near.foundPx == null ? Infinity : Math.abs(edgeMeasure.near.foundPx - edgeMeasure.near.edgePx);
    const farErr = edgeMeasure.far.foundPx == null ? Infinity : Math.abs(edgeMeasure.far.foundPx - edgeMeasure.far.edgePx);
    assertTrue(
      "tile boundary matches published rect",
      nearErr <= tolPx && farErr <= tolPx,
      `axis=${gutterA.axis} near: published=${edgeMeasure.near.edgePx.toFixed(1)}px found=${edgeMeasure.near.foundPx == null ? "none" : edgeMeasure.near.foundPx.toFixed(1) + "px"} err=${nearErr.toFixed(1)}px; ` +
        `far: published=${edgeMeasure.far.edgePx.toFixed(1)}px found=${edgeMeasure.far.foundPx == null ? "none" : edgeMeasure.far.foundPx.toFixed(1) + "px"} err=${farErr.toFixed(1)}px (tol ${tolPx}px)`,
    );
  } else {
    assertTrue("tile boundary matches published rect", false, "no gutter pair available to scan (see background-colour assertion)");
  }

  // ================= Assertion: fill, not letterbox (all four edges) =================
  // My brief said "left/right" — that is wrong for this direction of
  // mismatch. The fake engine emits 16:9; a 16:9 source is WIDER than a 1:1
  // tile, and under `fill` (CompositorLayout.h computeSourceFraming, the
  // non-fit branch) a wider source is scaled to the tile's HEIGHT and
  // overflows WIDTH, so it crops left/right and matches top/bottom exactly —
  // meaning under BOTH `fill` (correct) and a regressed `fit` (letterbox top/
  // bottom, but still full width) the left/right edges show content. A `fit`
  // regression would only show up as bars top/bottom. Sampling all four
  // edges is direction-agnostic and catches a fit regression no matter which
  // way a future aspect combination points.
  const rectsB = phaseB.tiles.map((m) => m.rect);
  if (rectsB.length === 0) throw new Error("phase B published no tiles");
  const probeRect = sortReadingOrder(phaseB.tiles)[0].rect;
  const inset = 4;
  const cxB = (probeRect.x + probeRect.width / 2) * canvasWidth;
  const cyB = (probeRect.y + probeRect.height / 2) * canvasHeight;
  const leftX = probeRect.x * canvasWidth + inset;
  const rightX = (probeRect.x + probeRect.width) * canvasWidth - inset;
  const topY = probeRect.y * canvasHeight + inset;
  const bottomY = (probeRect.y + probeRect.height) * canvasHeight - inset;
  const edgeSamples = {
    left: samplePatchMedianRgb(programAbs, phaseB.tOffsetSec, leftX, cyB, 6),
    right: samplePatchMedianRgb(programAbs, phaseB.tOffsetSec, rightX, cyB, 6),
    top: samplePatchMedianRgb(programAbs, phaseB.tOffsetSec, cxB, topY, 6),
    bottom: samplePatchMedianRgb(programAbs, phaseB.tOffsetSec, cxB, bottomY, 6),
  };
  const edgeIsBg = Object.fromEntries(Object.entries(edgeSamples).map(([k, v]) => [k, colorClose(v, bgRgb, 20)]));
  const anyBar = Object.values(edgeIsBg).some(Boolean);
  assertTrue(
    "fill, not letterbox (all four edges)",
    !anyBar,
    `1:1 tile vs 16:9 source: ${Object.entries(edgeSamples).map(([k, v]) => `${k}=rgb(${v.r},${v.g},${v.b})${edgeIsBg[k] ? " BAR" : ""}`).join(", ")} (bg~=(${bgRgb.r},${bgRgb.g},${bgRgb.b}))`,
  );

  // ================= Assertion: reflow after (genuine) staleness =================
  const expectedRemaining = participantCount - 1;
  const countOkC = phaseC.tiles.length === expectedRemaining;
  const orderedC = sortReadingOrder(phaseC.tiles);
  const remainingPids = pids.slice(0, -1);
  let reflowLarger = countOkC;
  let reflowIdentity = countOkC;
  const reflowDetails = [];
  if (countOkC) {
    for (let k = 0; k < orderedC.length && k < orderedA.length; k += 1) {
      const beforeArea = orderedA[k].rect.width * orderedA[k].rect.height;
      const afterArea = orderedC[k].rect.width * orderedC[k].rect.height;
      const larger = afterArea > beforeArea * 1.02; // must be MEANINGFULLY larger, not float noise
      reflowLarger = reflowLarger && larger;
      const rect = orderedC[k].rect;
      const { x: cx, y: cy } = tileSamplePoint(rect);
      const patch = Math.max(16, Math.min(48, Math.floor(Math.min(rect.width * canvasWidth, rect.height * canvasHeight) * 0.25)));
      const rgb = samplePatchMedianRgb(programAbs, phaseC.tOffsetSec, cx, cy, patch);
      const uv = rgbToYuv(rgb);
      const match = identifyParticipant(uv, pids, chromaOf);
      const expectedPid = remainingPids[k];
      const identOk = match.accepted && match.pid === expectedPid;
      reflowIdentity = reflowIdentity && identOk;
      reflowDetails.push(
        `posC[${k}] area ${beforeArea.toFixed(4)}->${afterArea.toFixed(4)} (${larger ? "larger" : "NOT larger"}), ` +
        `identity expected=${expectedPid} got=${match.pid ?? "REJECTED"} (${identOk ? "ok" : "MISMATCH"})`,
      );
    }
    console.log(`  ${reflowDetails.join("\n  ")}`);
  }
  assertTrue(
    "reflow after departure (genuine staleness)",
    countOkC && reflowLarger && reflowIdentity && staleElapsedMs != null,
    countOkC
      ? `${orderedC.length} tiles remain (${droppedMemberId} went stale, never removed from tiles.members); ` +
        `dropped after ${staleElapsedMs ?? "never"}ms (budget ${kTilesStaleFrameMs}ms); ` +
        `${reflowLarger ? "all grew" : "did NOT grow"}; identity ${reflowIdentity ? "held" : "broke"}`
      : `expected ${expectedRemaining} tiles after staleness, got ${phaseC.tiles.length} (staleElapsedMs=${staleElapsedMs})`,
  );

  // ================= Assertion: background colour at shipping-default spacing =================
  const rectsD = phaseD.tiles.map((m) => m.rect);
  const countOkD = phaseD.tiles.length === expectedRemaining;
  const marginD = countOkD ? marginSample(rectsD) : null;
  const gutterD = countOkD ? gutterSample(rectsD) : null;
  let bgOkD = countOkD;
  const bgDetailsD = [];
  if (!countOkD) bgDetailsD.push(`skipped — phase D tile count was ${phaseD.tiles.length}, expected ${expectedRemaining}`);
  if (marginD) {
    const patch = Math.max(3, Math.min(10, Math.floor(marginD.size * canvasHeight * 0.5)));
    const rgb = samplePatchMedianRgb(programAbs, phaseD.tOffsetSec, marginD.x * canvasWidth, marginD.y * canvasHeight, patch);
    const ok = colorClose(rgb, bgRgb, 24);
    bgOkD = bgOkD && ok;
    bgDetailsD.push(`margin(${marginD.side}, ${(marginD.size * canvasHeight).toFixed(1)}px)=rgb(${rgb.r},${rgb.g},${rgb.b}) -> ${ok ? "match" : "MISMATCH"}`);
  } else if (countOkD) {
    bgOkD = false;
    bgDetailsD.push("no usable margin band found at default spacing");
  }
  if (gutterD && gutterD.gap > 0) {
    const gapPx = gutterD.gap * (gutterD.axis === "x" ? canvasWidth : canvasHeight);
    const patch = Math.max(3, Math.min(10, Math.floor(gapPx * 0.5)));
    const rgb = samplePatchMedianRgb(programAbs, phaseD.tOffsetSec, gutterD.midX * canvasWidth, gutterD.midY * canvasHeight, patch);
    const ok = colorClose(rgb, bgRgb, 24);
    bgOkD = bgOkD && ok;
    bgDetailsD.push(`gutter(${gapPx.toFixed(1)}px)=rgb(${rgb.r},${rgb.g},${rgb.b}) -> ${ok ? "match" : "MISMATCH"}`);
  } else if (countOkD) {
    bgOkD = false;
    bgDetailsD.push("no usable inter-tile gutter found at default spacing");
  }
  assertTrue("background colour (shipping default spacing)", bgOkD, bgDetailsD.join("; "));

  console.log("");
  console.log("[validate-tiles] SKIP glow (T2 — not built yet, no glow layer exists to sample)");
  console.log("[validate-tiles] SKIP at-rest byte-identical (T1 has no animation toggle; nothing to freeze and compare)");
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
  // Keep evidence on failure (you can only pass --keep-artifacts in advance
  // if you already knew it would fail); delete on success unless explicitly
  // asked to keep.
  const keep = keepArtifacts || failures.length > 0;
  if (!keep) {
    for (const abs of artifacts) {
      try { if (existsSync(abs)) rmSync(abs); } catch { /* best-effort */ }
    }
  } else if (failures.length > 0 && artifacts.length) {
    console.error(`[validate-tiles] keeping artifact(s) for inspection: ${artifacts.join(", ")}`);
  }
}
process.exit(failures.length === 0 ? 0 : 1);
