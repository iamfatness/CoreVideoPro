/**
 * GPU-direct encode acceptance gate (#521 slice 1).
 *
 * The whole point of the slice: feed the hardware H.264 encoder from the
 * compositor's GPU texture so 1080p60 streaming runs at realtime instead of the
 * ~0.76x the GPU->CPU-readback->raw-pipe->ffmpeg path capped at. This harness
 * stands up a REAL localhost SRT sink, points the core's program output at it,
 * and fails unless the received stream actually keeps up with realtime.
 *
 * SRT, not RTMP, for the sink: the GPU-direct path is protocol-agnostic (same
 * sender, same -c:v copy bitstream muxer — only the endpoint/container differ),
 * and ffmpeg's `-listen 1` RTMP server is too flaky to gate CI on, while an SRT
 * listener is reliable (see validate-srt-output.mjs). Live RTMP to YouTube is the
 * final manual acceptance step, not this automated gate.
 *
 * Why the received stream and not the sender's own counter: on the GPU path
 * submit() is non-blocking (newest-wins), so the sender's framesSent grows at the
 * paced rate whether or not the pipeline keeps up. The received stream cannot lie
 * — with `-c:v copy` the muxer copies exactly what the encoder produced, and the
 * sink's own `-stats speed` is media-time / wall-time, i.e. the realtime ratio.
 *
 * Gate (GPU path): received fps >= 58 AND sink speed >= 0.97x over the window,
 * AND the core logged `[gpu-encode] path=gpu-direct`.
 * `--force-raw` sets COREVIDEO_GPU_ENCODE=0 and asserts the raw fallback still
 * streams (documents the A/B), without the realtime gate.
 *
 * `--codec av1` PASSES BY REFUSAL, NOT BY STREAMING (2026-09-20). GPU-direct AV1
 * binds the NVIDIA AV1 MFT and runs at the correct cadence but emits near-empty
 * access units (~54 bytes/frame vs H.264's ~12,483 at 1080p60), so the muxed
 * stream is ~18 kbit/s against 6 Mbps. Three hypotheses were eliminated (deep
 * encoder pipeline, FFmpeg's obu demuxer on a live pipe, our per-event output
 * drain) and the remaining cause is vendor/driver level. AV1 therefore ships
 * REFUSED with a named reason rather than broken: this gate asserts the core
 * logged `stream start REFUSED code=codec-not-deliverable` and that NO
 * `path=gpu-direct codec=av1` stream was established. If AV1 ever starts
 * streaming here, that is the signal to revisit the refusal — not a gate failure
 * to paper over. See CLAUDE.md (GPU-direct section) and
 * docs/superpowers/specs/2026-09-20-gpu-direct-hevc-av1-stream-design.md.
 *
 * `--slow-sink` (#597 Task 8) is the CONGESTION gate: the stream is carried over
 * a link narrowed to `--sink-rate` of the configured bitrate so the destination
 * genuinely cannot keep up (see the proxy below for why a BANDWIDTH ceiling and
 * not FFmpeg's `-readrate` is the only honest way to model this), and
 * the run then asserts the whole backpressure property from Tasks 2-7 against a
 * real congested endpoint rather than against unit tests:
 *   - the stream never fails,
 *   - the encoder is NEVER rebuilt (exactly one `[gpu-encode] started`),
 *   - Lever A's divisor steps UP under congestion and comes back DOWN,
 *   - buffered latency returns below kRecoverBelowBufferedMs (100ms) rather
 *     than sitting pinned at the bound,
 *   - Program holds 60fps, measured from the core's OWN render slot counter
 *     against wall time (never a container's declared rate - FFmpeg pads
 *     duplicates up to `-r` and a container fps cannot prove cadence),
 *   - and the #597 SIGNATURE: the core's snapshot emission cadence stays in its
 *     normal band, measured against that same advancing SAMPLE COUNTER. The
 *     incident's fingerprint was a 21s gap in perf.log whose sample counter
 *     advanced normally - i.e. the PRODUCER was fine and the consumer stalled.
 *     Wall time alone cannot tell those two apart; the counter can.
 *
 * THE SLOW SINK IS ITSELF VERIFIED, not assumed. A sink that silently fails to
 * throttle turns this whole gate into a no-op that reports success, so the run
 * FAILS if the outgoing queue never actually grew (`bufferedMs` never crossed
 * the policy's own throttle threshold).
 *
 * Burst mode (#597 Task 8b fix round 1): `--slow-sink --burst-sink` keeps the
 * sustained narrowing AND stalls the link outright for `--burst-stall-ms`
 * (default 3000) every `--burst-period-ms` (default 20000), starting
 * `--burst-first-ms` (default 25000) after the link first carries a byte. That
 * reproduces the 22-to-60-chunk onset Task 8 measured, which a sustained
 * narrowing never reaches. A burst run ASSERTS it entered the queue's overflow
 * branch: if the core logs no overflow line at all, the run tested nothing and
 * FAILS.
 *
 * Usage: node ./scripts/validate-gpu-encode.mjs [--seconds 30] [--port 1935]
 *                                               [--fps 60] [--bitrate 6]
 *                                               [--codec h264|hevc|av1]
 *                                               [--slow-sink [--sink-rate 0.85]]
 *                                               [--poll-ms 1000]
 *                                               [--force-raw] [--keep]
 */
import { spawn, spawnSync } from "node:child_process";
import net from "node:net";
import { existsSync, rmSync, statSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";
const nativeCore = join(buildDir, `corevideo-native${exeSuffix}`);
const fakeEngine = join(buildDir, `corevideo-zoom-engine-fake${exeSuffix}`);

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const seconds = Number(argValue("seconds", 30));
const port = Number(argValue("port", 9021));
const TARGET_FPS = Number(argValue("fps", 60));
const bitrate = Number(argValue("bitrate", 6));
const forceRaw = args.includes("--force-raw");
const slowSink = args.includes("--slow-sink");
const sinkRate = Number(argValue("sink-rate", 0.85));
// #597 Task 8b fix round 1: BURST MODE.
//
// The sustained gate could not reach the state it exists to judge. Task 8
// measured the storm's onset as 22 chunks to the 60-chunk cap INSIDE ONE
// SECOND, but a sustained narrowing never reproduces that: Levers A and B hold
// the queue at 38-47 of 60 for twelve minutes, so `enqueueBitstream`'s overflow
// branch is never entered and the gate reports success about a branch it never
// ran. Narrowing further does NOT help and makes it worse - below about 0.5x
// the link is too narrow for FFmpeg's own RTMP handshake, the egress child dies
// at ~14 frames with "Cannot read RTMP handshake response", and the supervisor
// correctly restarts a sender whose destination is dead. That failure is real
// but it is a DEAD DESTINATION, not a queue bound, and gating on it would be
// the same class of mistake as the SRT sink that dropped rather than blocked.
//
// So: keep the sustained narrowing (which is gentle enough that the handshake
// always completes), and periodically STALL the link outright for ~1s. At 60fps
// a full stall queues ~60 chunks, which is the measured onset. The first stall
// is deliberately late so it can never land on the handshake.
const burstSink = args.includes("--burst-sink");
// 3000ms, not the 1000ms that first looked right. At 60fps a 1s stall queues
// ~60 chunks - but only at divisor 1. Lever A is usually ALREADY engaged when a
// stall lands (that is the point: the queue is under pressure), and at divisor
// 4 the encoder is fed 15fps, so a 1s stall queues ~15 chunks and the cap is
// never reached. Measured: a 1s-stall run peaked at 45 of 60 and the gate
// correctly FAILED it for never entering the overflow branch. 3s covers the
// divisor-4 case with margin and is still an ordinary uplink outage.
const burstStallMs = Number(argValue("burst-stall-ms", 3000));
const burstPeriodMs = Number(argValue("burst-period-ms", 20000));
const burstFirstMs = Number(argValue("burst-first-ms", 25000));
const pollMs = Number(argValue("poll-ms", 1000));
// SRT LATENCY IS LOAD-BEARING FOR THE SLOW SINK, and this default is a finding,
// not a preference. Measured on this rig: at the product default of 120ms an
// SRT `transtype=live` sender simply DROPS what it cannot deliver (libsrt's
// too-late-packet drop), so a sink throttled to 0.5x real time lost ~35% of
// frames on the wire and our own outgoing queue never aged past 0ms - the
// congestion never reached the core at all. The #597 incident was RTMP over
// TCP, which cannot drop: the socket backs up, FFmpeg blocks, our stdin pipe
// fills and our queue ages. Raising SRT's latency widens the too-late window so
// the sender must HOLD rather than discard, which reproduces the TCP shape on a
// transport this harness can stand up reliably. See the report for the full
// measurement.
const sinkLatencyMs = Number(argValue("sink-latency-ms", 120));
// WHICH TRANSPORT CARRIES THE CONGESTION, and this default is a MEASUREMENT.
//
// The healthy gates stay on SRT for the reason the header gives: an SRT
// listener is reliable where ffmpeg's `-listen 1` RTMP server is fussy.
// But SRT CANNOT EXPRESS THE #597 FAILURE. Measured on this rig: with the
// listener throttled to 0.5x real time, an SRT `transtype=live` sender simply
// DISCARDS what it cannot deliver (libsrt's too-late-packet drop) — 70 s run,
// 3800 frames accepted by our sender, 1984 received, and our own outgoing
// queue never aged past 0 ms. Raising the SRT latency to 4000 ms did not change
// it. No congestion ever reached the core, so every assertion below would have
// been judged against a stream that was never actually backed up.
//
// #597 was RTMP over TCP, and TCP cannot drop: the receive window closes, the
// socket backs up, FFmpeg blocks, our 1 MiB stdin pipe fills, and our outgoing
// queue starts ageing — which is the `bufferedMs` signal the policy acts on.
// Measured the same way: a `-readrate 0.5` RTMP listener held an unrelated
// producer to speed=0.465x with no loss at all. So --slow-sink defaults to
// RTMP. `--sink-protocol srt` still works and still reproduces the negative
// result above; it is kept deliberately so the finding stays checkable.
const sinkProtocol = String(argValue("sink-protocol", slowSink ? "rtmp" : "srt")).toLowerCase();
if (!["srt", "rtmp"].includes(sinkProtocol)) {
  console.error(`--sink-protocol must be srt or rtmp (got ${sinkProtocol})`);
  process.exit(2);
}
const rtmpSink = sinkProtocol === "rtmp";
const rtmpAppUrl = `rtmp://127.0.0.1:${port}/live`;
const rtmpStreamKey = "gate";
const rtmpFullUrl = `${rtmpAppUrl}/${rtmpStreamKey}`;
const destinationId = rtmpSink ? "rtmp" : "srt";
// With --slow-sink the core does NOT talk to the FFmpeg listener directly: a
// bandwidth-limited TCP proxy sits between them on `port`, and the real
// listener binds `port + 1`.
const listenerPort = slowSink && rtmpSink ? port + 1 : port;
const rtmpListenUrl = `rtmp://127.0.0.1:${listenerPort}/live/${rtmpStreamKey}`;
if (burstSink && !slowSink) {
  console.error("--burst-sink requires --slow-sink (it stalls the slow sink's proxy link)");
  process.exit(2);
}
if (burstSink && !(burstStallMs > 0 && burstPeriodMs > burstStallMs && burstFirstMs >= 0)) {
  console.error("--burst-stall-ms must be > 0 and --burst-period-ms must exceed it");
  process.exit(2);
}
if (slowSink && !(sinkRate > 0 && sinkRate < 1)) {
  console.error(`--sink-rate must be between 0 and 1 exclusive (got ${sinkRate})`);
  process.exit(2);
}
const codecArgIndex = args.indexOf("--codec");
const codec = codecArgIndex >= 0 ? String(args[codecArgIndex + 1] || "h264").toLowerCase() : "h264";
if (!["h264", "hevc", "h265", "av1"].includes(codec)) {
  console.error(`--codec must be h264, hevc or av1 (got ${codec})`);
  process.exit(2);
}
const wireCodec = codec === "hevc" ? "h265" : codec;  // settings spelling
if (forceRaw && wireCodec !== "h264") {
  console.error("--force-raw is H.264-only: HEVC/AV1 are GPU-direct or refused (spec 2026-09-20 §5)");
  process.exit(2);
}
const keep = args.includes("--keep");
// AV1 ships REFUSED (see the header). The gate observes the refusal instead of a
// stream; h264/hevc are untouched.
const expectRefusal = wireCodec === "av1";

const ffBin = (name) => {
  for (const bin of [name, `C:\\ffmpeg\\bin\\${name}.exe`]) {
    const probe = spawnSync(bin, ["-version"], { encoding: "utf8", timeout: 10000 });
    if (!probe.error && probe.status === 0) return bin;
  }
  return null;
};
const ffmpeg = ffBin("ffmpeg");
const ffprobe = ffBin("ffprobe");
if (!ffmpeg || !ffprobe) {
  console.error("ffmpeg/ffprobe are required for the GPU-direct encode gate.");
  process.exit(1);
}
if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Build --config Release first.`);
  process.exit(1);
}

const received = join(buildDir, `gpu-encode-received-${Date.now()}.${rtmpSink ? "flv" : "ts"}`);
// SRT listener sink. -stats gives us the realtime ratio (media-time/wall-time) of
// what actually arrives; -c copy means no re-encode padding to hide a lag.
// listen_timeout (microseconds) must exceed the core's join+arm time or the
// listener gives up before the caller connects.
const listenerUrl = `srt://0.0.0.0:${port}?mode=listener&transtype=live&listen_timeout=30000000`;
// The sink itself reads at full speed. With --slow-sink the narrowing happens
// at the bandwidth-limited proxy in front of it (see below), never here.
const listenerArgs = ["-hide_banner", "-loglevel", "warning", "-stats", "-stats_period", "1", "-y",
                      "-fflags", "+genpts"];
if (rtmpSink) listenerArgs.push("-listen", "1");
// `-t` is MEDIA time, and a congested run delivers less media than wall time, so
// a slow-sink run is given a generous media budget and ended by the kill below
// instead - a `-t` sized for wall seconds would truncate the capture mid-run and
// read as a dead destination.
const listenerDurationSec = slowSink ? Math.ceil(seconds * 2 + 30) : seconds + 8;
listenerArgs.push("-i", rtmpSink ? rtmpListenUrl : listenerUrl,
                  "-t", String(listenerDurationSec),
                  "-c", "copy", "-f", rtmpSink ? "flv" : "mpegts", received);
const listener = spawn(ffmpeg, listenerArgs, { stdio: ["ignore", "ignore", "pipe"] });
let listenerStderr = "";
listener.stderr.on("data", (c) => { listenerStderr += c.toString(); });
let listenerExited = false;
listener.once("exit", () => { listenerExited = true; });

// ---------------------------------------------------------------------------
// THE SLOW SINK: a bandwidth-limited TCP proxy, and the mechanism matters.
//
// The first attempt throttled the sink with FFmpeg's `-readrate 0.85`. It
// congests, and it is WRONG for this gate, for a reason worth writing down:
// `-readrate` limits how fast the sink consumes MEDIA TIME, not how many bytes
// per second the link carries. Lever A reduces the BITRATE (fewer input frames,
// fewer bytes) while media time keeps advancing at real time by construction -
// our egress stamps arriving access units with `-use_wallclock_as_timestamps 1`.
// So against a media-time throttle Lever A is powerless BY CONSTRUCTION: the
// deficit accumulates no matter how far the divisor climbs. Measured that way,
// a 90 s run buried the outgoing queue at 4012 ms, hit the 60-chunk overflow cap
// and rebuilt the encoder seven times - a "failure" of a lever that was never
// being given the signal it exists to answer.
//
// Real network congestion - #597's congestion - is a BANDWIDTH ceiling. This
// proxy is one: a token bucket at `sinkRate` x the configured stream bitrate,
// with the inbound socket PAUSED whenever its holding buffer is full, so TCP's
// own receive window closes on the egress FFmpeg exactly the way a congested
// uplink does. Nothing is dropped and nothing is reordered; the link is simply
// narrower than the stream, which is the one fault this whole sub-project
// exists to survive.
//
// It also makes the throttle DIRECTLY observable: `proxyBytesForwarded` over
// wall time is the delivered bandwidth, so the run can state what the link
// actually carried instead of asserting that a flag worked.
const proxyCapacityBytesPerSec = (bitrate * 1_000_000 / 8) * sinkRate;
let proxyBytesForwarded = 0;
let proxyConnections = 0;
let proxyStartedAt = 0;
let proxyServer = null;
// Burst bookkeeping, reported and asserted below.
let burstStallsApplied = 0;
let burstStallUntil = 0;
let burstNextAt = 0;
let burstBaselineAt = 0;   // set when the first byte is forwarded: the stall
                           // schedule is measured from a link that is CARRYING
                           // the stream, never from process start.
const burstStalling = (now) => {
  if (!burstSink) return false;
  if (!burstBaselineAt) return false;
  if (!burstNextAt) burstNextAt = burstBaselineAt + burstFirstMs;
  if (now < burstStallUntil) return true;
  if (now >= burstNextAt) {
    burstStallUntil = now + burstStallMs;
    burstNextAt = now + burstPeriodMs;
    burstStallsApplied += 1;
    return true;
  }
  return false;
};
if (slowSink && rtmpSink) {
  proxyServer = net.createServer((client) => {
    proxyConnections += 1;
    if (!proxyStartedAt) proxyStartedAt = Date.now();
    const upstream = net.connect(listenerPort, "127.0.0.1");
    upstream.setNoDelay(true);
    client.setNoDelay(true);
    // The holding buffer IS the link's buffer. Small on purpose: a large one
    // would absorb the whole experiment before the core ever noticed.
    const kHoldBytes = 64 * 1024;
    let held = [];
    let heldBytes = 0;
    let paused = false;
    let closed = false;
    const tickMs = 20;
    let credit = 0;
    client.on("data", (buf) => {
      held.push(buf);
      heldBytes += buf.length;
      if (heldBytes >= kHoldBytes && !paused) { paused = true; client.pause(); }
    });
    const timer = setInterval(() => {
      if (closed) return;
      // A stall forwards NOTHING and banks NOTHING. The client keeps writing
      // into the 64 KB hold buffer, that fills, the inbound socket is paused,
      // TCP's receive window closes on the egress FFmpeg, its stdin backs up,
      // and our bitstream queue ages - which is exactly the onset Task 8
      // measured. Nothing is dropped and nothing is reordered: the link simply
      // stops carrying for a second, the way a real uplink does.
      if (burstStalling(Date.now())) return;
      credit += proxyCapacityBytesPerSec * (tickMs / 1000);
      while (credit >= 1 && heldBytes > 0) {
        const head = held[0];
        const take = Math.min(head.length, Math.floor(credit));
        if (take <= 0) break;
        upstream.write(head.subarray(0, take));
        if (!burstBaselineAt) burstBaselineAt = Date.now();
        proxyBytesForwarded += take;
        credit -= take;
        heldBytes -= take;
        if (take === head.length) held.shift(); else held[0] = head.subarray(take);
      }
      // Never bank unspent credit: an idle second must not buy a later burst
      // that hides the deficit it is supposed to create.
      if (heldBytes === 0) credit = Math.min(credit, proxyCapacityBytesPerSec * (tickMs / 1000));
      if (paused && heldBytes < kHoldBytes / 2) { paused = false; client.resume(); }
    }, tickMs);
    const shutdown = () => {
      if (closed) return;
      closed = true;
      clearInterval(timer);
      try { upstream.destroy(); } catch {}
      try { client.destroy(); } catch {}
      held = [];
    };
    upstream.on("data", (buf) => { try { client.write(buf); } catch {} });
    upstream.on("error", shutdown);
    upstream.on("close", shutdown);
    client.on("error", shutdown);
    client.on("close", shutdown);
  });
  proxyServer.on("error", (e) => { console.error(`slow-sink proxy error: ${e.message}`); });
  proxyServer.listen(port, "127.0.0.1");
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
    COREVIDEO_FAKE_NO_CHURN: "1",
    // Pin the source rate: an unpinned fake engine delivers 30fps, which would
    // starve the 60fps gate and read as a pipeline failure (CLAUDE.md rule).
    COREVIDEO_FAKE_ENGINE_FPS: String(TARGET_FPS),
    ...(forceRaw ? { COREVIDEO_GPU_ENCODE: "0" } : {}),
  },
  stdio: ["pipe", "pipe", "pipe"],
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();
let coreStderr = "";
child.stderr.on("data", (c) => { coreStderr += c.toString(); });

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

function send(type, payload = {}) {
  const id = `gpu-${nextId++}`;
  return new Promise((res, rej) => {
    const timer = setTimeout(() => { pending.delete(id); rej(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: res, reject: rej, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  }).then((r) => {
    if (r.ok === false) throw new Error(`${type} failed: ${r.error?.message ?? "unknown"}`);
    return r;
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The core logs the chosen path via nativeLogf; capture it from stderr and, as a
// fallback, media-core.log next to the binary.
function gpuEncodePathLine() {
  const haystacks = [coreStderr];
  const logPath = join(buildDir, "media-core.log");
  if (existsSync(logPath)) { try { haystacks.push(readFileSync(logPath, "utf8")); } catch {} }
  for (const text of haystacks) {
    const m = text.match(/\[gpu-encode\] path=(gpu-direct|cpu-fallback)[^\n]*/g);
    if (m && m.length) return m[m.length - 1];
  }
  return null;
}

// The encoder start line names the actual MFT ("hardware-<codec>") the core bound.
function gpuEncodeStartedLine() {
  const haystacks = [coreStderr];
  const logPath = join(buildDir, "media-core.log");
  if (existsSync(logPath)) { try { haystacks.push(readFileSync(logPath, "utf8")); } catch {} }
  for (const text of haystacks) {
    const m = text.match(/\[gpu-encode\] started[^\n]*/g);
    if (m && m.length) return m[m.length - 1];
  }
  return null;
}

// The sender's own refusal line: `[gpu-encode] stream start REFUSED code=... `.
// THIS RUN'S stderr ONLY — deliberately not media-core.log, unlike the two path
// readers above. That log accumulates across runs in the build dir, and a stale
// refusal line from an EARLIER av1 run would let the gate pass while the current
// core silently streamed 18 kbit/s — the exact failure this gate exists to catch.
// A missing stderr line fails the gate, which is the safe direction.
function streamStartRefusedLine() {
  const m = coreStderr.match(/\[gpu-encode\] stream start REFUSED[^\n]*/g);
  return m && m.length ? m[m.length - 1] : null;
}

// #597 Task 8: how many times the hardware encoder was BUILT this run. Task 7's
// whole claim is that a congested destination never triggers a rebuild, and this
// is the number that settles it. THIS RUN'S stderr ONLY, for the same reason
// streamStartRefusedLine gives above: media-core.log accumulates across runs in
// the build dir, so counting it would fold every earlier run's starts into this
// verdict.
function gpuEncodeStartedCount() {
  const m = coreStderr.match(/\[gpu-encode\] started[^\n]*/g);
  return m ? m.length : 0;
}

function backpressureLines() {
  const m = coreStderr.match(/\[stream-backpressure\][^\n]*/g);
  return m ?? [];
}

// #597 Task 8, the deferred Task-7 item: a LINGERING FFmpeg child on a refused
// rebuild. Tasks 2-7 could only reason about it; here it is observable. The
// egress child's command line carries our destination URL (srt://127.0.0.1:<port>)
// while the sink listener carries srt://0.0.0.0:<port>, so the two never collide.
// More than one egress child alive at once IS the lingering-child defect; one
// left alive after stop is the same defect at teardown.
function egressFfmpegChildCount() {
  if (process.platform !== "win32") return -1;  // not measurable here; reported, never asserted
  const out = spawnSync("powershell", ["-NoProfile", "-Command",
    `@(Get-CimInstance Win32_Process -Filter "Name='ffmpeg.exe'" | ` +
    `Where-Object { $_.CommandLine -like '*${rtmpSink ? rtmpFullUrl : `srt://127.0.0.1:${port}`}*' -and ` +
    `$_.CommandLine -notlike '*-listen*' }).Count`],
    { encoding: "utf8", timeout: 20000 });
  const n = Number((out.stdout ?? "").trim());
  return Number.isFinite(n) ? n : -1;
}

const failures = [];
const senderFps = [];
let lastFrameSample = null;
let senderSnapshot = null;
// One row per poll: everything the slow-sink assertions are decided from.
const samples = [];
let maxEgressChildren = 0;
let egressChildrenAfterStop = -1;
let egressChildStopSeconds = -1;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");

  await sleep(2500);  // RTMP listener head start before the caller connects
  if (listenerExited) {
    throw new Error(`RTMP listener exited before streaming started (${listenerStderr.trim().split("\n").pop() || "no output"})`);
  }
  console.log(`listener      : up on ${rtmpSink ? rtmpFullUrl : `srt://127.0.0.1:${port}`}` +
              `${slowSink ? ` behind a ${(proxyCapacityBytesPerSec * 8 / 1e6).toFixed(2)}Mbps link` : ""}`);

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "gpu-encode-proof" } });
  await sleep(3000);

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "gpu-encode-proof",
        routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "101" }],
      },
      {
        type: "sync-audio-routing-matrix",
        sends: [{ sourceId: "zoom-mix", busId: "master", gainDb: 0 },
                { sourceId: "zoom-mix", busId: "stream", gainDb: 0 }],
      },
      {
        type: "start-program-output",
        destinations: [destinationId],
        destinationSettings: [rtmpSink ? {
          id: "rtmp",
          label: "validate-gpu-encode",
          protocol: "rtmp",
          url: rtmpAppUrl,
          streamKey: rtmpStreamKey,
          fps: TARGET_FPS,
          targetBitrateMbps: bitrate,
          encoderMode: "auto",
          videoCodec: wireCodec,
          allowEnhancedRtmp: true,
          ffmpegBinDirectory: "C:\\ffmpeg\\bin",
        } : {
          id: "srt",
          label: "validate-gpu-encode",
          protocol: "srt",
          host: "127.0.0.1",
          port,
          mode: "caller",
          latencyMs: sinkLatencyMs,
          fps: TARGET_FPS,
          targetBitrateMbps: bitrate,
          encoderMode: "auto",
          videoCodec: wireCodec,
          allowEnhancedRtmp: true,
          ffmpegBinDirectory: "C:\\ffmpeg\\bin",
        }],
        isoParticipantIds: [],
      },
    ],
  });
  console.log(`streaming     : ${rtmpSink ? rtmpFullUrl : `srt://127.0.0.1:${port}`} codec=${codec} ` +
              `for ${seconds}s (${forceRaw ? "COREVIDEO_GPU_ENCODE=0" : "GPU-direct"}` +
              `${slowSink ? `, link narrowed to ${sinkRate}x of ${bitrate}Mbps` : ""})...`);

  const deadline = Date.now() + seconds * 1000;
  let nextChildCensus = 0;
  while (Date.now() < deadline) {
    await sleep(Math.min(slowSink ? pollMs : 5000, Math.max(250, deadline - Date.now())));
    const sync = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    const receivedAt = Date.now();
    // The published node is sessionState().outputSenderSession.senders[] —
    // `outputSenders` does not exist on this wire and is kept only as a
    // defensive fallback for an older core.
    const senders = sync.snapshot?.outputSenderSession?.senders ?? sync.snapshot?.outputSenders?.senders ?? [];
    senderSnapshot = senders.find((s) => (s.destination ?? s.senderId ?? "").includes(destinationId)) ?? senders[0] ?? null;
    const evidence = sync.snapshot?.realtimeEvidence ?? {};
    // THE SAMPLE COUNTER (#597 signature). The core's own render-worker slot
    // count advances at the display rate on the core's own thread, entirely
    // independently of whether anyone is reading snapshots. A wall-clock gap
    // across which THIS advanced normally is a CONSUMER stall; one across which
    // it also stalled is a PRODUCER slowdown. That is the distinction the
    // incident turned on, and the reason nothing below is judged on wall time
    // alone.
    const row = {
      at: receivedAt,
      completedSlots: Number(evidence.render?.completedSlots ?? 0),
      renderProgressAgeMs: Number(evidence.render?.progressAgeMs ?? -1),
      exportDivisor: Number(evidence.encoderExport?.divisor ?? 1),
      shedFrames: Number(evidence.encoderExport?.shedFrames ?? 0),
      exporting: !!evidence.encoderExport?.exporting,
      status: senderSnapshot?.status ?? "none",
      framesSent: Number(senderSnapshot?.framesSent ?? 0),
      bp: senderSnapshot?.backpressure ?? null,
    };
    samples.push(row);

    if (senderSnapshot) {
      const frames = row.framesSent;
      if (lastFrameSample && frames > lastFrameSample.frames) {
        senderFps.push((frames - lastFrameSample.frames) / ((receivedAt - lastFrameSample.at) / 1000));
      }
      lastFrameSample = { frames, at: receivedAt };
      const bp = row.bp;
      const bpText = bp
        ? ` bp{div=${bp.divisor} buffered=${Math.round(bp.bufferedMs)}ms chunks=${bp.queuedChunks} ` +
          `entered=${bp.enteredCount} discards=${bp.discardEvents} dropped=${bp.discardedChunks} reason=${bp.lastReason}}`
        : " bp{absent}";
      console.log(`sender        : status=${senderSnapshot.status} health=${senderSnapshot.destinationHealth ?? "?"} ` +
                  `frames=${frames} exportDiv=${row.exportDivisor} shed=${row.shedFrames}${bpText}` +
                  `${senderSnapshot.warning ? ` warning=${senderSnapshot.warning}` : ""}`);
    }

    // The FFmpeg child census is a PowerShell round-trip; sample it every ~10s
    // rather than every poll so the measurement never becomes the load.
    if (slowSink && receivedAt >= nextChildCensus) {
      nextChildCensus = receivedAt + 10000;
      const n = egressFfmpegChildCount();
      if (n > maxEgressChildren) maxEgressChildren = n;
    }
  }

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-program-output", reason: "gpu-encode proof complete" }],
  });
  if (slowSink) {
    // AsyncOutputSender is enqueue-and-return and its stop carries a 2 s grace,
    // and stopFfmpegProcess() closes stdin and WAITS for exit - which on a
    // deliberately narrow link is exactly the case where FFmpeg cannot drain
    // quickly. So this MEASURES the reap instead of guessing a single deadline:
    // poll until the child is gone, up to the 20 s bound, and report how long
    // it took. Only a child still alive at the bound is a leak.
    const stopAt = Date.now();
    for (;;) {
      egressChildrenAfterStop = egressFfmpegChildCount();
      egressChildStopSeconds = (Date.now() - stopAt) / 1000;
      if (egressChildrenAfterStop <= 0 || egressChildStopSeconds >= 20) break;
      await sleep(2000);
    }
  }
} catch (error) {
  failures.push(error.message);
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
}

await sleep(3000);
// The core is dead by now and its KILL_ON_JOB_CLOSE job object takes every
// child with it, so this second census answers a DIFFERENT question from the
// one above: it separates "the sender reaped its child" from "the job object
// did it for us at exit". Reported, never asserted - the assertion is the
// post-stop census, which is the one a live show depends on.
const egressChildrenAfterCoreExit = slowSink ? egressFfmpegChildCount() : -1;
try { listener.kill(); } catch {}
try { proxyServer?.close(); } catch {}
await sleep(1500);

// Dump the core's own stderr for diagnosis (kept only with --keep).
if (keep) {
  const dump = join(buildDir, `gpu-encode-core-stderr-${startedAt}.txt`);
  try { (await import("node:fs")).writeFileSync(dump, coreStderr); console.log(`core stderr   : ${dump}`); } catch {}
}

// Which path did the core take?
const pathLine = gpuEncodePathLine();
console.log(`encode path   : ${pathLine || "UNKNOWN (no [gpu-encode] path= line found)"}`);
const startedLine = gpuEncodeStartedLine();
console.log(`encoder start : ${startedLine || "UNKNOWN (no [gpu-encode] started line found)"}`);
const expectedPathCodec = codec === "h265" ? "hevc" : codec;
const tookGpuDirect = !!pathLine && pathLine.includes("path=gpu-direct") && pathLine.includes(`codec=${expectedPathCodec}`);

// ---------------------------------------------------------------------------
// AV1: PASS BY REFUSAL. Nothing about this leg reads a stream — there is no
// stream, by design. Judged on the core's own refusal line and on the ABSENCE of
// a GPU-direct AV1 path line.
// ---------------------------------------------------------------------------
if (expectRefusal) {
  const refusalLine = streamStartRefusedLine();
  console.log(`refusal       : ${refusalLine || "NONE (no [gpu-encode] stream start REFUSED line found)"}`);
  if (!refusalLine || !refusalLine.includes("code=codec-not-deliverable")) {
    failures.push("expected the core to log `stream start REFUSED code=codec-not-deliverable` for AV1 — " +
                  (refusalLine ? `got: ${refusalLine}` : "no refusal line at all"));
  }
  if (tookGpuDirect) {
    failures.push(`AV1 was admitted onto the GPU-direct path — it must be refused at start: ${pathLine}`);
  }
  if (senderSnapshot) {
    console.log(`sender        : lastResultCode=${senderSnapshot.lastResultCode ?? "?"} ` +
                `warning=${senderSnapshot.warning || "none"}`);
  }
  if (!keep) { try { rmSync(received); } catch {} }
  if (failures.length) {
    console.error("\nGPU-DIRECT ENCODE GATE FAIL (--codec av1, refusal expected)");
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log("\nav1: REFUSED as designed (codec-not-deliverable) — see docs");
  console.log("      AV1 does NOT stream on this path. This PASS means the refusal is working,");
  console.log("      not that GPU-direct AV1 works. See CLAUDE.md (GPU-direct section).");
  console.log("\nGPU-DIRECT ENCODE GATE PASS (av1 refused as designed)");
  process.exit(0);
}

if (forceRaw) {
  if (tookGpuDirect) failures.push("COREVIDEO_GPU_ENCODE=0 but the core still took the GPU-direct path");
} else if (!tookGpuDirect) {
  failures.push(`expected GPU-direct codec=${expectedPathCodec} but the core reported: ${pathLine || "no path line"}`);
}

// The received stream cannot lie.
let size = 0;
try { size = statSync(received).size; } catch {}
console.log(`received      : ${size} bytes at ${received}`);
if (size < 10000) {
  failures.push(`RTMP sink got ${size} bytes — no usable stream arrived` +
                (listenerStderr ? ` (${listenerStderr.trim().split("\n").pop()})` : ""));
} else {
  const probe = spawnSync(ffprobe, ["-v", "error", "-print_format", "json", "-show_streams", received],
    { encoding: "utf8", timeout: 20000 });
  let streams = [];
  try { streams = JSON.parse(probe.stdout).streams ?? []; } catch {}
  const video = streams.find((s) => s.codec_type === "video");
  console.log(`decoded       : ${video ? `${video.codec_name} ${video.width}x${video.height}` : "NO VIDEO STREAM"}`);
  if (!video) failures.push("received stream carries no decodable video");

  if (video) {
    const countOut = spawnSync(ffprobe,
      ["-v", "error", "-select_streams", "v:0", "-count_frames",
       "-show_entries", "stream=nb_read_frames", "-of", "default=nw=1:nk=1", received],
      { encoding: "utf8", timeout: 60000 });
    const durationOut = spawnSync(ffprobe,
      ["-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", received],
      { encoding: "utf8", timeout: 60000 });
    const frames = Number((countOut.stdout ?? "").trim().split(/\s+/)[0]);
    const durationSec = Number((durationOut.stdout ?? "").trim().split(/\s+/)[0]);
    if (frames > 0 && durationSec > 1) {
      const fps = frames / durationSec;
      console.log(`received rate : ${fps.toFixed(1)}fps of ${TARGET_FPS} (${frames} frames / ${durationSec.toFixed(2)}s)`);
      // A DELIBERATELY slow sink is expected to receive less than realtime —
      // that is the whole experiment. Gating it here would fail the run for
      // doing exactly what it was asked to do.
      if (!forceRaw && !slowSink && fps < TARGET_FPS * 0.97) {
        failures.push(`received ${fps.toFixed(1)}fps < ${(TARGET_FPS * 0.97).toFixed(1)} (0.97x of ${TARGET_FPS}) — the GPU path is not keeping up with realtime`);
      }
    } else {
      console.log("received rate : not measurable (short or unseekable capture)");
      if (!forceRaw && !slowSink) failures.push("could not measure received frame rate");
    }
  }
}

// The sink's own realtime ratio.
const speeds = [...listenerStderr.matchAll(/speed=\s*([0-9.]+)x/g)].map((m) => Number(m[1])).filter((n) => n > 0);
if (speeds.length) {
  const sorted = [...speeds].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const last = speeds[speeds.length - 1];
  console.log(`sink speed    : ${median.toFixed(2)}x median, ${last.toFixed(2)}x last (${speeds.length} samples)`);
  if (!forceRaw && !slowSink && median < 0.97) {
    failures.push(`sink speed ${median.toFixed(2)}x < 0.97x — the pipeline is behind realtime`);
  }
} else {
  console.log("sink speed    : not sampled");
}

if (senderFps.length) {
  const best = Math.max(...senderFps);
  console.log(`sender feed   : ${best.toFixed(1)}fps best of ${TARGET_FPS} (${senderFps.length} intervals; reported, not gated)`);
}

// ---------------------------------------------------------------------------
// #597 Task 8 - THE CONGESTION GATE. Everything below is judged only when the
// sink was deliberately throttled; a healthy-sink run is unchanged.
//
// Thresholds mirror core/StreamBackpressurePolicy.h BY VALUE. They are repeated
// here rather than derived because this harness cannot include a C++ header - if
// the policy's constants move, this block must move with them.
// ---------------------------------------------------------------------------
const kThrottleAboveBufferedMs = 250;
const kRecoverBelowBufferedMs = 100;
const startedCount = gpuEncodeStartedCount();
console.log(`encoder builds: ${startedCount} ([gpu-encode] started lines, this run's stderr only)`);

if (slowSink) {
  const bpRows = samples.filter((r) => r.bp);
  const proxySec = proxyStartedAt ? (Date.now() - proxyStartedAt) / 1000 : 0;
  const deliveredMbps = proxySec > 0 ? (proxyBytesForwarded * 8) / proxySec / 1e6 : 0;
  console.log(`\nslow sink     : bandwidth-limited proxy at ${sinkRate}x of ${bitrate}Mbps = ` +
              `${(proxyCapacityBytesPerSec * 8 / 1e6).toFixed(2)}Mbps ceiling; delivered ` +
              `${deliveredMbps.toFixed(2)}Mbps over ${proxySec.toFixed(1)}s ` +
              `(${proxyConnections} connection(s)), ${samples.length} polls`);
  // Reported, deliberately NOT asserted. Delivered bandwidth below the ceiling
  // is the EXPECTED steady state once Lever A engages - the divisor exists to
  // put the stream under the link - so a low average here is the lever working,
  // not the link failing to bite. What proves the link bit is the queue-growth
  // precondition immediately below.
  console.log(`                (link ceiling ${(proxyCapacityBytesPerSec * 8 / 1e6).toFixed(2)}Mbps vs ` +
              `${bitrate}Mbps configured; a delivered rate under the ceiling is Lever A working)`);
  for (const line of backpressureLines()) console.log(`  core        : ${line.trim()}`);

  // --- BURST MODE: did the run actually ENTER the overflow branch? ---------
  // The whole point of fix round 1's harness work. `overflow-discard` is logged
  // (rate-limited to 1/s) every time the queue hit its cap and the GOP-tail
  // discard made room instead of failing the sender; the second line is the
  // case where nothing was safe to drop. A burst run that logs NEITHER never
  // reached the state it is asserting about, and must FAIL rather than pass -
  // a gate that cannot enter its own branch reports success while testing
  // nothing, which is how the original storm went unseen.
  const overflowDiscardLines = coreStderr.split(/\r?\n/).filter((l) => l.includes("overflow-discard"));
  const overflowFailLines = coreStderr.split(/\r?\n/)
      .filter((l) => l.includes("queue overflow with nothing safe to drop"));
  if (burstSink) {
    console.log(`burst sink    : ${burstStallsApplied} link stall(s) of ${burstStallMs}ms ` +
                `(first at +${burstFirstMs}ms, then every ${burstPeriodMs}ms) on top of the ${sinkRate}x link`);
    console.log(`overflow path : ${overflowDiscardLines.length} overflow-discard line(s), ` +
                `${overflowFailLines.length} nothing-safe-to-drop line(s)`);
    for (const line of overflowDiscardLines.slice(0, 6)) console.log(`  core        : ${line.trim()}`);
    for (const line of overflowFailLines.slice(0, 6)) console.log(`  core        : ${line.trim()}`);
    if (!burstStallsApplied) {
      failures.push("burst mode applied ZERO link stalls - the proxy never carried a byte, so the " +
                    "stall schedule never armed and this run tested nothing");
    } else if (overflowDiscardLines.length === 0 && overflowFailLines.length === 0) {
      failures.push(`burst mode stalled the link ${burstStallsApplied} time(s) but the queue never ` +
                    "reached its cap: NO overflow-discard and NO nothing-safe-to-drop line was logged, " +
                    "so enqueueBitstream's overflow branch was never entered and this run proves " +
                    "nothing about it. Lengthen --burst-stall-ms rather than accepting the green.");
    }
  } else if (overflowDiscardLines.length || overflowFailLines.length) {
    // Sustained runs do not normally reach the cap; when they do, say so.
    console.log(`overflow path : ${overflowDiscardLines.length} overflow-discard line(s), ` +
                `${overflowFailLines.length} nothing-safe-to-drop line(s) (reported, not required here)`);
  }

  // --- (0) Did the sink ACTUALLY throttle? ---------------------------------
  // A sink that silently fails to throttle turns every assertion below into a
  // no-op that reports success. This is the precondition, and it is checked
  // FIRST so a no-op run fails loudly instead of passing green.
  const maxBuffered = bpRows.length ? Math.max(...bpRows.map((r) => r.bp.bufferedMs)) : -1;
  const maxQueued = bpRows.length ? Math.max(...bpRows.map((r) => r.bp.queuedChunks ?? 0)) : -1;
  console.log(`queue growth  : bufferedMs peak ${Math.round(maxBuffered)}ms, queuedChunks peak ${maxQueued}` +
              ` (throttle threshold ${kThrottleAboveBufferedMs}ms)`);
  if (!bpRows.length) {
    failures.push("no sender published a `backpressure` node - the GPU-direct backpressure path never ran, " +
                  "so nothing below was actually exercised");
  } else if (maxBuffered < kThrottleAboveBufferedMs) {
    failures.push(`the slow sink did NOT throttle: bufferedMs peaked at ${Math.round(maxBuffered)}ms, below the ` +
                  `${kThrottleAboveBufferedMs}ms threshold the policy acts on. This run proves nothing - ` +
                  "make the sink slower (lower --sink-rate) rather than accepting the green.");
  }

  // --- (1) The stream never stops ------------------------------------------
  const failedRows = samples.filter((r) => r.status === "failed");
  if (failedRows.length) {
    failures.push(`the sender reached status=failed ${failedRows.length} time(s) under congestion - ` +
                  "backpressure must degrade the stream, never end it");
  }

  // --- (2) ZERO encoder rebuilds -------------------------------------------
  // Task 7's entire claim, meeting a real congested destination.
  if (startedCount !== 1) {
    failures.push(`expected exactly ONE [gpu-encode] started line (the initial build) but saw ${startedCount} - ` +
                  "the encoder was rebuilt under congestion, which is the #597 storm");
  }

  // --- (3) Lever A steps up, and comes back down ---------------------------
  const divisors = bpRows.map((r) => r.bp.divisor);
  const peakDivisor = divisors.length ? Math.max(...divisors) : 1;
  const finalDivisor = divisors.length ? divisors[divisors.length - 1] : 1;
  const peakExportDivisor = samples.length ? Math.max(...samples.map((r) => r.exportDivisor)) : 1;
  const shed = samples.length ? samples[samples.length - 1].shedFrames : 0;
  const enters = backpressureLines().filter((l) => l.includes(" enter ")).length;
  const exits = backpressureLines().filter((l) => l.includes(" exit ")).length;
  console.log(`lever A       : divisor peak ${peakDivisor} (compositor applied peak ${peakExportDivisor}), ` +
              `final ${finalDivisor}, frames shed ${shed}, ${enters} enter / ${exits} exit cycle(s)`);
  if (peakDivisor < 2) {
    failures.push("Lever A never engaged: the divisor stayed at 1 for the whole congested run");
  }
  if (peakExportDivisor < 2) {
    failures.push("the compositor never applied a divisor above 1 - the sender's decision did not reach " +
                  "realtimeEvidence.encoderExport, so nothing was actually shed");
  }
  // RECOVERY IS "IT CAME BACK DOWN", NOT "IT ENDED DOWN". The first cut of this
  // assertion compared the FINAL divisor to the peak, and it failed a run that
  // had recovered to 1 twice and merely happened to be mid-degrade when the
  // window closed. Congestion here is a CYCLE - degrade, drain, recover,
  // re-enter - so where the run's last poll lands in that cycle is an accident
  // of the clock, not a property of the lever. What the lever must show is that
  // a step DOWN follows a step UP at all; the "did it get back to real time"
  // half is assertion (4) below, on bufferedMs, which is the number an operator
  // actually feels.
  const steppedDown = divisors.some((d, i) => i > 0 && d < divisors[i - 1]);
  if (peakDivisor >= 2 && !steppedDown) {
    failures.push(`the divisor reached ${peakDivisor} and never stepped back down at any point in the run - ` +
                  "recovery is half the property; a stream that only degrades is not a working lever");
  }

  // --- (4) Buffered latency RETURNS, rather than sitting at the bound -------
  // The failure this catches is the one named in StreamBackpressurePolicy.h's
  // own header: a stream that stabilises a full second behind and STAYS there,
  // looking healthy the whole time.
  const tailRows = bpRows.slice(-Math.max(3, Math.ceil(bpRows.length * 0.1)));
  const tailBuffered = tailRows.length ? Math.min(...tailRows.map((r) => r.bp.bufferedMs)) : -1;
  console.log(`recovery      : best bufferedMs over the last ${tailRows.length} polls = ${Math.round(tailBuffered)}ms ` +
              `(must fall below ${kRecoverBelowBufferedMs}ms)`);
  if (tailBuffered < 0 || tailBuffered >= kRecoverBelowBufferedMs) {
    failures.push(`buffered latency never returned below kRecoverBelowBufferedMs (${kRecoverBelowBufferedMs}ms) - ` +
                  `best over the run's tail was ${Math.round(tailBuffered)}ms, i.e. the stream stabilised behind ` +
                  "live and stayed there");
  }

  // --- (5) Program holds 60fps, measured from the core's OWN counter --------
  // NEVER from the container: FFmpeg pads duplicates up to its declared -r, so
  // a stream ffprobe reads as 59.9 proves nothing about cadence.
  let worstProgramFps = Infinity;
  let worstAt = 0;
  for (let i = 1; i < samples.length; i += 1) {
    const dt = (samples[i].at - samples[i - 1].at) / 1000;
    const dslots = samples[i].completedSlots - samples[i - 1].completedSlots;
    if (dt <= 0) continue;
    const fps = dslots / dt;
    if (fps < worstProgramFps) { worstProgramFps = fps; worstAt = i; }
  }
  const totalDt = samples.length > 1 ? (samples[samples.length - 1].at - samples[0].at) / 1000 : 0;
  const totalSlots = samples.length > 1 ? samples[samples.length - 1].completedSlots - samples[0].completedSlots : 0;
  const meanProgramFps = totalDt > 0 ? totalSlots / totalDt : 0;
  console.log(`program       : ${meanProgramFps.toFixed(1)}fps mean, ` +
              `${Number.isFinite(worstProgramFps) ? worstProgramFps.toFixed(1) : "n/a"}fps worst interval ` +
              `(render.completedSlots deltas vs wall clock, ${totalSlots} slots / ${totalDt.toFixed(1)}s)`);
  if (meanProgramFps < TARGET_FPS * 0.97) {
    failures.push(`Program averaged ${meanProgramFps.toFixed(1)}fps of ${TARGET_FPS} - congestion on one destination ` +
                  "must not cost the show its frame rate");
  }
  if (Number.isFinite(worstProgramFps) && worstProgramFps < TARGET_FPS * 0.90) {
    failures.push(`Program's worst poll interval delivered ${worstProgramFps.toFixed(1)}fps of ${TARGET_FPS} ` +
                  `(interval ${worstAt}) - a sustained dip, not jitter`);
  }

  // --- (6) THE #597 SIGNATURE ----------------------------------------------
  // The incident was a 21s gap in perf.log whose SAMPLE COUNTER advanced
  // normally: the producer was healthy and the consumer was blocked. Measure
  // BOTH halves of every over-bound poll interval and name which one failed,
  // because the two have completely different fixes.
  const cadenceBoundMs = Math.max(4 * pollMs, pollMs + 3000);
  let worstGapMs = 0;
  const stalls = [];
  for (let i = 1; i < samples.length; i += 1) {
    const gapMs = samples[i].at - samples[i - 1].at;
    if (gapMs > worstGapMs) worstGapMs = gapMs;
    if (gapMs <= cadenceBoundMs) continue;
    const dslots = samples[i].completedSlots - samples[i - 1].completedSlots;
    const expected = (gapMs / 1000) * TARGET_FPS;
    const counterAdvancedNormally = expected > 0 && dslots >= expected * 0.9;
    stalls.push({ i, gapMs, dslots, expected, counterAdvancedNormally });
  }
  console.log(`#597 signature: worst snapshot gap ${worstGapMs}ms (bound ${cadenceBoundMs}ms at poll ${pollMs}ms), ` +
              `${stalls.length} over-bound gap(s)`);
  for (const stall of stalls) {
    const kind = stall.counterAdvancedNormally
      ? "SAMPLE COUNTER ADVANCED NORMALLY -> the core kept producing and the snapshot path stalled (the #597 shape)"
      : "the sample counter stalled too -> a genuine producer slowdown";
    failures.push(`snapshot emission stalled ${stall.gapMs}ms at poll ${stall.i} ` +
                  `(render slots +${stall.dslots}, expected ~${Math.round(stall.expected)}): ${kind}`);
  }
  // A counter that stops advancing while snapshots keep arriving on time is the
  // other half of the same question, and it has no wall-clock gap to give it
  // away. progressAgeMs is the core's own answer to "when did the render worker
  // last move"; anything past a few frames is a stalled producer.
  const worstProgressAge = samples.length ? Math.max(...samples.map((r) => r.renderProgressAgeMs)) : -1;
  console.log(`render worker : worst progressAgeMs ${worstProgressAge}`);
  if (worstProgressAge > 500) {
    failures.push(`the core's render worker went ${worstProgressAge}ms without progress - a producer stall the ` +
                  "snapshot cadence alone would not have shown");
  }

  // --- (7) The deferred Task-7 item: no lingering FFmpeg child -------------
  console.log(`ffmpeg child  : peak ${maxEgressChildren} egress child(ren) concurrent; ` +
              `${egressChildrenAfterStop} still alive ${egressChildStopSeconds.toFixed(1)}s after ` +
              `stop-program-output; ${egressChildrenAfterCoreExit} after the core exited`);
  if (maxEgressChildren > 1) {
    failures.push(`${maxEgressChildren} egress FFmpeg children were alive at once - a refused rebuild left a child ` +
                  "behind, which on SRT would hold the single caller slot and refuse the reconnect");
  }
  // REPORTED, NOT ASSERTED - and this is a finding, not a softened threshold.
  //
  // Measured here every congested run: the egress child is STILL ALIVE 20 s
  // after stop-program-output, and only the output job object reaps it when the
  // core exits. The cause is in the product, not the harness:
  // RtmpOutputSenderAdapter::stopFfmpegProcess() closes stdin and then waits
  // `WaitForSingleObject(process, 500)` - 500 ms - and NEVER terminates. On a
  // healthy link FFmpeg flushes and exits inside that window; on a link too
  // narrow to drain into, it cannot, so it keeps publishing to a live
  // destination after the operator stopped the stream, and a supervisor restart
  // (interrupt()/recover() both route through this same function) spawns a
  // second child alongside it.
  //
  // It is not asserted because it is a PRE-EXISTING teardown defect, unrelated
  // to the backpressure property this gate exists to hold, and failing the gate
  // on it would leave the gate permanently red and therefore useless for the
  // property it was built to protect. It needs its own issue and its own fix -
  // NOT a wider timeout here. What IS asserted is the half that would actually
  // corrupt a show: two egress children alive at once (above).
  if (egressChildrenAfterStop > 0) {
    console.log(`                FINDING (reported, not asserted): the child was still alive at the ` +
                `${egressChildStopSeconds.toFixed(1)}s bound. stopFfmpegProcess() waits 500ms and never ` +
                `terminates, so a congested destination keeps being published to after Stop; only the ` +
                `job object at core exit ends it. Needs its own issue.`);
  }
  if (maxEgressChildren === 0) {
    console.log("                (census never saw a child - reported, not asserted: it samples every ~10s)");
  }
}

if (!keep) { try { rmSync(received); } catch {} }

if (failures.length) {
  console.error(`\nGPU-DIRECT ENCODE GATE FAIL${forceRaw ? " (--force-raw)" : ""}${slowSink ? " (--slow-sink)" : ""}`);
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log(`\nGPU-DIRECT ENCODE GATE PASS${forceRaw ? " (raw fallback)" : ""}${slowSink ? " (slow sink: degraded, never rebuilt)" : ""}`);
