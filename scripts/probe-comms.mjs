/**
 * COMMS END TO END WITHOUT ZOOM (docs/reference/comms-talkback.md).
 *
 * Drives the real media core over its stdio protocol against the synthetic
 * Zoom engine (corevideo-zoom-engine-fake), exactly as the WinUI shell does:
 * comms-* requests in, "zoom-comms-state" facts out. Proves the core relay,
 * the engine command routing and the shared comms core (bank, auto-assign,
 * keys, latches, ALL CALL, leases, release-all, assign, tone, gain, devices,
 * web-client refusal) end to end. It does NOT prove real Zoom talkback.
 *
 * Port of CoreVideo Edge's scripts/probe-comms.mjs onto Pro's core protocol.
 * Usage: node ./scripts/probe-comms.mjs
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
  console.error(`missing ${nativeCore} or ${fakeEngine}; run npm run build:native-dev`);
  process.exit(1);
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
    // Fake roster: 101 mv1, 102 mv2, 103 Producer, 104 mv4 (the web client).
    COREVIDEO_FAKE_ENGINE_PARTICIPANTS: "4",
  },
  stdio: ["pipe", "pipe", "pipe"],
});
child.stderr.on("data", () => {});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? "PASS" : "FAIL"}  ${what}`);
  if (!ok) failures += 1;
  return ok;
};

let nextId = 1;
let buffer = "";
let handshake = false;
let comms;           // last zoom-comms-state comms object (null = dropped)
let commsFacts = 0;
const pending = new Map();
child.stdout.setEncoding("utf8");
child.stdout.on("data", (chunk) => {
  buffer += chunk;
  let nl;
  while ((nl = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, nl).trim();
    buffer = buffer.slice(nl + 1);
    if (!line) continue;
    let m;
    try { m = JSON.parse(line); } catch { continue; }
    if (m.type === "handshake" && m.ok === true) handshake = true;
    if (m.type === "zoom-comms-state" && m.id === undefined) { comms = m.comms; commsFacts += 1; }
    if (typeof m.id === "string" && pending.has(m.id)) {
      pending.get(m.id)(m);
      pending.delete(m.id);
    }
  }
});

function send(type, payload) {
  const id = `comms-${nextId++}`;
  return new Promise((res, rej) => {
    const timer = setTimeout(() => { pending.delete(id); rej(new Error(`${type} timed out`)); }, 15000);
    pending.set(id, (m) => { clearTimeout(timer); res(m); });
    const req = { id, type };
    if (payload !== undefined) req.payload = payload;
    child.stdin.write(`${JSON.stringify(req)}\n`);
  });
}
const ok = (r) => r.ok === true;
const err = (r) => r?.error?.message ?? "";

async function waitFor(pred, timeoutMs, what) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try { if (pred(comms)) return check(true, what); } catch { /* not there yet */ }
    await sleep(100);
  }
  return check(false, `${what} (timed out after ${timeoutMs / 1000}s; reason=${JSON.stringify(comms?.reason ?? null)})`);
}
const chan = (slot) => comms.channels[slot];
const person = (id) => comms.roster.find((p) => p.participantId === id);

try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!check(handshake, "core handshake")) throw new Error("no handshake");
  await sleep(500);
  check(comms === undefined, "no comms fact before any engine reports");
  let r = await send("comms-key", { slot: 0, on: true });
  check(!ok(r) && r.error?.code === "comms-refused", `comms-key refused outside a meeting ("${err(r)}")`);
  r = await send("comms-key", { slot: 0, on: false });
  check(ok(r), "releasing a key is never refused");

  r = await send("zoom-join", { meetingNumber: "1234567890", displayName: "comms-probe" });
  check(ok(r), `zoom-join accepted${ok(r) ? "" : ` (${err(r)})`}`);
  await waitFor((c) => c && c.available === true, 15000, "comms available (fake: host, talkback supported)");
  await waitFor((c) => c.channels.length === 16 && c.channels.every((x) => x.ready), 10000, "all 16 channels formed");
  check(comms.lamps.link && comms.lamps.meetingMic && comms.lamps.channel, "lamps: LINK, MTG MIC, CHANNEL on");

  await waitFor((c) => [101, 102, 103].every((id) => {
    const p = c.roster.find((x) => x.participantId === id);
    return p && p.talkback && p.channels.length === 1 && c.channels[p.channels[0]].listeners === 1;
  }), 15000, "each eligible guest is confirmed on their own channel");
  const slots = [101, 102, 103].map((id) => person(id).channels[0]);
  check(new Set(slots).size === 3, `three distinct channels (${slots.join(", ")})`);
  check(chan(person(101).channels[0]).label === "mv1", "a one-person channel wears its person's name");
  const web = person(104);
  check(web && web.talkback === false && web.channels.length === 0, "the web-client guest has talkback:false and no channel");
  r = await send("comms-assign", { slot: 9, participantId: 104, on: true });
  check(!ok(r) && /web client/.test(err(r)), `assigning the web client is refused ("${err(r)}")`);

  const s1 = person(101).channels[0];
  const before = { ...comms.stats };
  r = await send("comms-key", { slot: s1, on: true });
  check(ok(r), `comms-key slot ${s1} on`);
  await waitFor((c) => c.channels[s1].keyed && c.lamps.tx, 3000, "cell keyed and TX lamp on");
  await sleep(1500);
  const keyed = { ...comms.stats };
  check(keyed.channelSends - before.channelSends >= 40, `channelSends rose while keyed (+${keyed.channelSends - before.channelSends} in ~1.5 s)`);
  check(keyed.sendErrors === 0, "no send errors");
  r = await send("comms-key", { slot: s1, on: false });
  await waitFor((c) => !c.channels[s1].keyed && !c.lamps.tx, 3000, "released: not keyed, TX off");

  const target = [...Array(16).keys()].find((s) => !slots.includes(s) && comms.channels[s].listeners === 0 && s >= 8);
  r = await send("comms-assign", { slot: target, participantId: 101, on: true });
  check(ok(r), `assign mv1 -> slot ${target}`);
  await waitFor((c) => c.channels[target].listeners === 1, 15000, `mv1 confirmed on slot ${target}`);

  r = await send("comms-tone", { on: true });
  await waitFor((c) => c.mic.tone === true && c.mic.peakDbfs > -30, 3000, "TONE on moves the meter");
  r = await send("comms-tone", { on: false });
  await waitFor((c) => c.mic.tone === false, 3000, "TONE off");

  r = await send("comms-latch", { all: true, on: true });
  await waitFor((c) => c.channels.every((x) => x.keyed && x.latched), 3000, "LATCH ALL keys every channel");
  r = await send("comms-latch", { all: true, on: false });
  await waitFor((c) => c.channels.every((x) => !x.keyed), 3000, "LATCH ALL off releases every channel");
  r = await send("comms-key", { all: true, on: true, leaseMs: 1500 });
  await waitFor((c) => c.channels.every((x) => x.keyed && !x.latched), 3000, "ALL CALL keys every channel");
  r = await send("comms-key", { all: true, on: false });
  await waitFor((c) => c.channels.every((x) => !x.keyed), 3000, "ALL CALL released");

  // The shell's lease: a hold that is not renewed drops by itself.
  r = await send("comms-key", { slot: target, on: true, leaseMs: 800 });
  check(ok(r), `leased hold on slot ${target}`);
  await waitFor((c) => c.channels[target].keyed, 2000, "leased hold keys the channel");
  await waitFor((c) => !c.channels[target].keyed, 3000, "an unrenewed leased hold drops by itself");
  r = await send("comms-key", { slot: target, on: true });
  r = await send("comms-latch", { slot: s1, on: true });
  await waitFor((c) => c.channels[target].keyed && c.channels[s1].keyed, 2000, "a hold and a latch both keyed");
  r = await send("comms-release-all", {});
  check(ok(r), "comms-release-all accepted");
  await waitFor((c) => !c.channels[target].keyed && c.channels[s1].latched, 2000, "release-all drops the hold, keeps the latch");
  r = await send("comms-latch", { slot: s1, on: false });

  r = await send("comms-gain", { db: 6 });
  await waitFor((c) => c.mic.gainDb === 6, 3000, "gain +6 dB reported");
  r = await send("comms-gain", { db: 21 });
  check(!ok(r), `gain 21 refused ("${err(r)}")`);
  r = await send("comms-sidetone", { on: false });
  await waitFor((c) => c.mic.sidetone === false, 3000, "sidetone off reported");
  check(comms.mic.devices.length >= 2 && comms.mic.sidetoneDevices.length >= 2, "device lists published");
  r = await send("comms-mic", { device: "Headset" });
  await waitFor((c) => c.mic.device === "Fake Headset Mic", 3000, 'mic switched to "Fake Headset Mic"');

  // The fact repeats while held (the zoom pump lane may drop a line).
  const factsBefore = commsFacts;
  await sleep(2500);
  check(commsFacts - factsBefore >= 2, `comms fact repeats while nothing changes (+${commsFacts - factsBefore} in 2.5 s)`);

  r = await send("zoom-leave");
  check(ok(r), "zoom-leave accepted");
  await waitFor((c) => c === null, 5000, "comms dropped (comms:null) after leave");
} catch (e) {
  check(false, `probe threw: ${e.stack ?? e}`);
} finally {
  child.stdin.end();
  await sleep(2000);
  if (child.exitCode === null) child.kill();
}

console.log(failures === 0 ? "\nCOMMS PROBE PASSED" : `\nCOMMS PROBE FAILED (${failures})`);
process.exit(failures === 0 ? 0 : 1);
