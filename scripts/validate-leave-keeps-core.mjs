/**
 * LEAVE MUST NOT KILL THE STUDIO.
 *
 * 2026-08-09, live meeting: the shell's leave-meeting path kill-treed the whole
 * media core (a sledgehammer from the era when the Zoom engine could not be
 * trusted to die). The studio then sat "unstable" — endless deferred syncs, no
 * render, no capture — until an app restart. This proves the CORE-side
 * contract the fix relies on: zoom-leave is handled, the core keeps answering
 * and keeps rendering afterward, and a REJOIN works on the same core.
 *
 * Usage: node ./scripts/validate-leave-keeps-core.mjs
 */
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";
const nativeCore = join(buildDir, `corevideo-native${exeSuffix}`);
const fakeEngine = join(buildDir, `corevideo-zoom-engine-fake${exeSuffix}`);

if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error("missing core or fake engine build");
  process.exit(1);
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: { ...process.env, COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine },
  stdio: ["pipe", "pipe", "pipe"],
});

let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();
let renderTicksSeen = 0;
child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString();
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
  if (chunk.toString().includes("displayTick")) renderTicksSeen += 1;
});

function send(type, payload = {}) {
  const id = `leave-${nextId++}`;
  return new Promise((res, rej) => {
    const timer = setTimeout(() => { pending.delete(id); rej(new Error(`${type} TIMED OUT — core frozen?`)); }, 15000);
    pending.set(id, { resolve: res, reject: rej, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const failures = [];
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no handshake");

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "leave-proof" } });
  await sleep(2500);
  const joined = await send("zoom-snapshot");
  console.log(`joined        : meetingState=${joined.snapshot?.meetingState}`);

  // THE MOMENT UNDER TEST.
  const left = await send("zoom-leave");
  console.log(`left          : responded ok=${left.ok !== false}`);

  // The core must still ANSWER and still RENDER after the leave.
  const before = renderTicksSeen;
  for (let i = 0; i < 6; i += 1) {
    await sleep(500);
    await send("media-core-sync", { commands: [] });
  }
  const ticked = renderTicksSeen > before;
  console.log(`after leave   : sync answered 6/6, render ticking=${ticked}`);
  if (!ticked) failures.push("render thread stopped after zoom-leave — the leave still stalls the core");

  // And a REJOIN must work on the SAME core process.
  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "leave-proof-2" } });
  await sleep(2500);
  const rejoined = await send("zoom-snapshot");
  console.log(`rejoined      : meetingState=${rejoined.snapshot?.meetingState}`);
  if (rejoined.snapshot?.meetingState !== "in_meeting") {
    failures.push(`rejoin after leave did not reach in_meeting (got ${rejoined.snapshot?.meetingState})`);
  }
} catch (error) {
  failures.push(error.message);
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
}

if (failures.length) {
  console.error("\nLEAVE-KEEPS-CORE VALIDATION FAIL");
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log("\nLEAVE-KEEPS-CORE VALIDATION PASS");
