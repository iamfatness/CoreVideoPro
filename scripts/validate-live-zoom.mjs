/**
 * Live Zoom validation harness for Sprint 1 / B2.
 *
 * This is a dev-machine gate for the real native Zoom path. It launches the
 * native media core over stdio, joins a real Zoom meeting, subscribes to live
 * participant feeds through zoom-media-spine-sync, and prints a pass/fail
 * report with roster, active speaker, and first-frame evidence.
 *
 * Usage:
 *   node ./scripts/validate-live-zoom.mjs --meeting-url "https://zoom.us/j/..."
 *
 * Optional:
 *   --passcode "..." --min-participants 3 --min-video-feeds 3
 *   --timeout-ms 120000 --max-first-frame-ms 1000 --allow-no-active-speaker
 */
import { spawn } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";

const args = parseArgs(process.argv.slice(2));
const meetingUrl = args["meeting-url"] ?? process.env.COREVIDEO_TEST_MEETING_URL ?? process.env.COREVIDEO_DEFAULT_MEETING_URL;
const passcode = args.passcode ?? process.env.COREVIDEO_ZOOM_MEETING_PASSCODE;
const displayName = args["display-name"] ?? "CoreVideo Live Validator";
const timeoutMs = numberArg(args["timeout-ms"], 120000);
const joinWaitMs = numberArg(args["join-wait-ms"], Math.min(timeoutMs, 90000));
const pollMs = numberArg(args["poll-ms"], 500);
const minParticipants = numberArg(args["min-participants"], 3);
const minVideoFeeds = numberArg(args["min-video-feeds"], minParticipants);
const maxVideoFeeds = numberArg(args["max-video-feeds"], Math.max(minVideoFeeds, 6));
const maxFirstFrameMs = numberArg(args["max-first-frame-ms"], 1000);
const requireActiveSpeaker = !booleanArg(args["allow-no-active-speaker"]);
const nativeCore = resolve(args["native-core"] ?? join(buildDir, `corevideo-native${exeSuffix}`));
const zoomEngine = resolve(args["zoom-engine"] ?? process.env.COREVIDEO_ZOOM_ENGINE_PATH ?? join(buildDir, `corevideo-zoom-engine${exeSuffix}`));

if (!meetingUrl) {
  failBeforeSpawn("Missing --meeting-url or COREVIDEO_TEST_MEETING_URL.");
}
if (!existsSync(nativeCore)) {
  failBeforeSpawn(`Missing native core at ${nativeCore}. Run ./scripts/build-native-dev.ps1 first.`);
}
if (!existsSync(zoomEngine)) {
  failBeforeSpawn(`Missing Zoom engine at ${zoomEngine}. Run ./scripts/build-native-dev.ps1 first.`);
}

const embeddedKey = readEmbeddedPublicAppKey();
const child = spawn(nativeCore, [], {
  cwd: dirname(nativeCore),
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: zoomEngine,
    COREVIDEO_ZOOM_PUBLIC_APP_KEY: process.env.COREVIDEO_ZOOM_PUBLIC_APP_KEY ?? embeddedKey,
    COREVIDEO_ZOOM_JOIN_WAIT_MS: String(joinWaitMs),
  },
  stdio: ["pipe", "pipe", "pipe"],
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
let joinSnapshot;
let latestSnapshot;
let latestSpineSnapshot;
let firstFrameAtMs;
let firstFrameParticipantId;
let firstFrameDimensions;
let firstFrameSource = "none";
let activeSpeakerSeen = false;
let maxParticipantCount = 0;
const participantsById = new Map();
const frameCountsByParticipant = new Map();
const audioPacketsByParticipant = new Map();
const rawAudioStatusByParticipant = new Map();
const pending = new Map();
const warnings = [];
const stderrLines = [];

child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString();
  let newlineIndex;
  while ((newlineIndex = stdoutBuffer.indexOf("\n")) >= 0) {
    const line = stdoutBuffer.slice(0, newlineIndex).trim();
    stdoutBuffer = stdoutBuffer.slice(newlineIndex + 1);
    if (line) {
      onLine(line);
    }
  }
});

child.stderr.on("data", (chunk) => {
  const text = chunk.toString();
  process.stderr.write(text);
  stderrLines.push(...text.split(/\r?\n/).filter(Boolean).slice(-20));
});

child.once("exit", (code) => {
  if (pending.size === 0) {
    return;
  }
  for (const { reject, timer } of pending.values()) {
    clearTimeout(timer);
    reject(new Error(`native core exited with code ${code ?? "unknown"}`));
  }
  pending.clear();
});

try {
  console.log(`Native core : ${nativeCore}`);
  console.log(`Zoom engine : ${zoomEngine}`);
  console.log(`Meeting URL : ${redactMeetingUrl(meetingUrl)}`);
  console.log(`Criteria    : participants>=${minParticipants}, videoFeeds>=${minVideoFeeds}, firstFrame<=${maxFirstFrameMs}ms`);

  handshake = await waitForHandshake();
  console.log(`Handshake   : ${handshake.profile?.name ?? "unknown"} (${(handshake.profile?.capabilities ?? []).join(", ")})`);

  joinSnapshot = await send("zoom-join", {
    payload: {
      meetingUrl,
      displayName,
      webinar: booleanArg(args.webinar),
      ...(passcode ? { passcode } : {}),
    },
  });
  latestSnapshot = joinSnapshot.snapshot;
  recordSnapshot(latestSnapshot);
  console.log(`Join state  : ${normalizeMeetingState(latestSnapshot?.meetingState)} with ${participantCount(latestSnapshot)} participant(s)`);

  await validationLoop();
  const report = buildReport("passed");
  printReport(report);
  cleanup(0);
} catch (error) {
  const report = buildReport("failed", error instanceof Error ? error.message : String(error));
  printReport(report);
  cleanup(1);
}

async function validationLoop() {
  const deadline = startedAt + timeoutMs;
  let lastSpineSyncAt = 0;

  while (Date.now() < deadline) {
    const snapshotResponse = await send("zoom-snapshot");
    latestSnapshot = snapshotResponse.snapshot;
    recordSnapshot(latestSnapshot);

    const state = normalizeMeetingState(latestSnapshot?.meetingState);
    if (state === "error") {
      throw new Error(`Zoom meeting entered error state: ${collectWarnings().join("; ") || "unknown error"}`);
    }

    const participants = usableParticipants(latestSnapshot);
    if (state === "in-meeting" && participants.length > 0 && Date.now() - lastSpineSyncAt >= pollMs) {
      latestSpineSnapshot = await send("zoom-media-spine-sync", {
        spinePayload: buildSpinePayload(participants),
        elapsedMs: Date.now() - startedAt,
      }).then((response) => response.spineSnapshot);
      recordSpineSnapshot(latestSpineSnapshot);
      lastSpineSyncAt = Date.now();
    }

    await send("ping").catch(() => undefined);
    if (criteriaMet()) {
      return;
    }
    await sleep(pollMs);
  }

  throw new Error("Timed out before live Zoom criteria were met.");
}

function onLine(line) {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }

  if (message.type === "zoom-video-frame" && message.frame) {
    const frame = message.frame;
    const id = String(frame.participantId ?? "");
    const hasPixels = hasZoomFramePixels(frame);
    if (id && hasPixels) {
      frameCountsByParticipant.set(id, (frameCountsByParticipant.get(id) ?? 0) + 1);
      recordFirstFrame({
        participantId: id,
        width: frame.width,
        height: frame.height,
        source: "zoom-video-frame",
        observedAtMs: numberArg(frame.observedAtMs, Date.now() - startedAt),
      });
    }
    return;
  }

  if (message.type === "handshake" && message.ok === true) {
    handshake = message;
  }

  if (typeof message.id === "string") {
    const item = pending.get(message.id);
    if (item) {
      pending.delete(message.id);
      clearTimeout(item.timer);
      item.resolve(message);
    }
  }
}

function send(type, payload = {}) {
  const id = `live-zoom-${nextId++}`;
  const request = { id, type, ...payload };
  return new Promise((resolvePromise, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`request ${type} timed out`));
    }, Math.max(5000, joinWaitMs + 5000));
    pending.set(id, { resolve: resolvePromise, reject, timer });
    child.stdin.write(`${JSON.stringify(request)}\n`);
  }).then((response) => {
    if (response.ok === false) {
      throw new Error(`${type} failed: ${response.error?.message ?? "unknown error"}`);
    }
    return response;
  });
}

function waitForHandshake() {
  if (handshake) {
    return Promise.resolve(handshake);
  }
  return new Promise((resolvePromise, reject) => {
    const timer = setTimeout(() => reject(new Error("Timed out waiting for native core handshake.")), 10000);
    const interval = setInterval(() => {
      if (handshake) {
        clearTimeout(timer);
        clearInterval(interval);
        resolvePromise(handshake);
      }
    }, 25);
  });
}

function recordSnapshot(snapshot) {
  if (!snapshot) {
    return;
  }
  const participants = usableParticipants(snapshot);
  maxParticipantCount = Math.max(maxParticipantCount, participants.length);
  for (const participant of participants) {
    participantsById.set(participant.id, participant);
  }
  if (snapshot.activeSpeakerId || participants.some((participant) => participant.talking)) {
    activeSpeakerSeen = true;
  }
  for (const warning of snapshot.warnings ?? []) {
    warnings.push(warning);
  }
}

function recordSpineSnapshot(snapshot) {
  if (!snapshot) {
    return;
  }
  maxParticipantCount = Math.max(maxParticipantCount, snapshot.participantCount ?? 0);
  if (snapshot.activeSpeakerId || snapshot.participants?.some((participant) => participant.talking)) {
    activeSpeakerSeen = true;
  }
  for (const participant of snapshot.participants ?? []) {
    participantsById.set(participant.sdkUserId, {
      id: participant.sdkUserId,
      displayName: participant.displayName,
      videoOn: participant.videoOn,
      muted: participant.muted,
      talking: participant.talking,
    });
  }
  for (const subscription of snapshot.subscriptions ?? []) {
    if (subscription.kind === "participant-video" && subscription.framesReceived > 0) {
      frameCountsByParticipant.set(
        subscription.participantId,
        Math.max(frameCountsByParticipant.get(subscription.participantId) ?? 0, subscription.framesReceived)
      );
      if (firstFrameAtMs === undefined) {
        const observedAtMs =
          typeof subscription.firstFrameAtMs === "number" && subscription.firstFrameAtMs >= 0
            ? subscription.firstFrameAtMs
            : Date.now() - startedAt;
        recordFirstFrame({
          participantId: subscription.participantId,
          width: subscription.deliveredWidth,
          height: subscription.deliveredHeight,
          source: "zoom-media-spine-sync",
          observedAtMs,
        });
      }
    }
    if (subscription.kind === "participant-audio") {
      audioPacketsByParticipant.set(
        subscription.participantId,
        Math.max(audioPacketsByParticipant.get(subscription.participantId) ?? 0, subscription.audioPacketsReceived ?? 0)
      );
      rawAudioStatusByParticipant.set(subscription.participantId, {
        status: subscription.status ?? "failed",
        lastResultCode: subscription.lastResultCode ?? "unknown",
        warning: subscription.warning,
      });
      if (subscription.lastResultCode === "raw-audio-unavailable") {
        warnings.push(
          subscription.warning ??
            "Raw mixed and isolated audio callbacks are disabled; enable raw audio in the Zoom SDK helper before live audio validation."
        );
      }
    }
  }
  for (const evidence of snapshot.recording?.evidence?.rawAudioByParticipant ?? []) {
    audioPacketsByParticipant.set(
      evidence.participantId,
      Math.max(audioPacketsByParticipant.get(evidence.participantId) ?? 0, evidence.audioPacketsReceived ?? 0)
    );
    rawAudioStatusByParticipant.set(evidence.participantId, {
      status: evidence.status ?? "failed",
      lastResultCode: evidence.lastResultCode ?? "unknown",
      warning: evidence.warning,
    });
    if (evidence.lastResultCode === "raw-audio-unavailable") {
      warnings.push(
        evidence.warning ??
          "Raw mixed and isolated audio callbacks are disabled; enable raw audio in the Zoom SDK helper before live audio validation."
      );
    }
  }
  for (const warning of snapshot.warnings ?? []) {
    warnings.push(warning);
  }
}

function buildSpinePayload(participants) {
  const selected = participants.slice(0, maxVideoFeeds);
  const subscriptions = [
    ...selected.map((participant, index) => ({
      participantId: participant.id,
      kind: "participant-video",
      purpose: index === 0 ? "active-speaker" : "program",
      priority: 10 + index,
    })),
    ...participants.map((participant, index) => ({
      participantId: participant.id,
      kind: "participant-audio",
      purpose: "mix",
      priority: 40 + index,
    })),
  ];

  return {
    readiness: {
      status: "ready",
      platform: process.platform === "darwin" ? "macos" : "windows",
      sdkVersion: "zoom-engine",
      checks: [
        { id: "sdk-runtime", status: "ready", label: "Zoom Meeting SDK runtime", detail: "Runtime launched by native validation harness." },
        { id: "app-key", status: "ready", label: "Meeting SDK app key", detail: "Meeting SDK app key configured." },
        { id: "oauth", status: "ready", label: "OAuth broker", detail: "Live validation uses the native join credential path." },
        { id: "jwt-broker", status: "ready", label: "SDK JWT broker", detail: "Live validation uses configured SDK credentials." },
        { id: "raw-video", status: "ready", label: "Raw participant video", detail: "Raw participant video requested." },
        { id: "raw-audio", status: "ready", label: "Raw participant audio", detail: "Raw participant audio requested." },
        { id: "raw-share", status: "ready", label: "Raw screen share", detail: "Raw screen-share path available." },
      ],
      blockers: [],
      warnings: [],
      summary: "Zoom SDK media path ready for live validation.",
    },
    participants: participants.map((participant) => ({
      sdkUserId: participant.id,
      displayName: participant.displayName,
      role: "guest",
      videoOn: participant.videoOn,
      muted: participant.muted,
      talking: participant.talking,
      sharingScreen: participant.sharingScreen,
      audioLevel: participant.talking ? 70 : 0,
      networkQuality: "good",
    })),
    subscriptions,
    blocked: false,
    warnings: [],
    summary: `${participants.length} real Zoom participant(s), ${subscriptions.length} raw subscriptions requested.`,
  };
}

function criteriaMet() {
  const videoFeedCount = participantsWithFrames().length;
  return (
    normalizeMeetingState(latestSnapshot?.meetingState) === "in-meeting" &&
    maxParticipantCount >= minParticipants &&
    videoFeedCount >= minVideoFeeds &&
    firstFrameAtMs !== undefined &&
    firstFrameAtMs <= maxFirstFrameMs &&
    (!requireActiveSpeaker || activeSpeakerSeen)
  );
}

function buildCriteriaResults() {
  const videoFeedCount = participantsWithFrames().length;
  const meetingInProgress = normalizeMeetingState(latestSnapshot?.meetingState) === "in-meeting";
  const firstFrameWithinBudget =
    firstFrameAtMs !== undefined && firstFrameAtMs <= maxFirstFrameMs;

  return {
    meetingInMeeting: {
      required: true,
      met: meetingInProgress,
      actual: normalizeMeetingState(latestSnapshot?.meetingState),
      detail: meetingInProgress
        ? "Zoom meeting reached in-meeting state."
        : "Zoom meeting never reached in-meeting state.",
    },
    participantCount: {
      required: true,
      met: maxParticipantCount >= minParticipants,
      actual: maxParticipantCount,
      expected: `>=${minParticipants}`,
      detail: `Observed ${maxParticipantCount} participant(s); need at least ${minParticipants}.`,
    },
    videoFeeds: {
      required: true,
      met: videoFeedCount >= minVideoFeeds,
      actual: videoFeedCount,
      expected: `>=${minVideoFeeds}`,
      detail: `Observed ${videoFeedCount} participant video feed(s); need at least ${minVideoFeeds}.`,
    },
    firstFrameTiming: {
      required: true,
      met: firstFrameWithinBudget,
      actualMs: firstFrameAtMs ?? null,
      expectedMs: `<=${maxFirstFrameMs}`,
      source: firstFrameSource,
      participantId: firstFrameParticipantId ?? null,
      dimensions: firstFrameDimensions ?? null,
      detail:
        firstFrameAtMs === undefined
          ? "No first zoom video frame was observed before timeout."
          : `First frame arrived in ${firstFrameAtMs}ms via ${firstFrameSource}${firstFrameParticipantId ? ` from participant ${firstFrameParticipantId}` : ""}${firstFrameDimensions ? ` (${firstFrameDimensions})` : ""}; budget is ${maxFirstFrameMs}ms.`,
    },
    activeSpeaker: {
      required: requireActiveSpeaker,
      met: !requireActiveSpeaker || activeSpeakerSeen,
      actual: activeSpeakerSeen,
      detail: activeSpeakerSeen
        ? "Active speaker or talking participant was observed."
        : "No active speaker or talking participant was observed.",
    },
  };
}

function buildReport(status, failureReason) {
  const criteriaResults = buildCriteriaResults();
  const failedChecks = Object.entries(criteriaResults)
    .filter(([, result]) => result.required && !result.met)
    .map(([name]) => name);
  const participantRows = [...participantsById.values()].map((participant) => ({
    id: participant.id,
    displayName: participant.displayName,
    videoOn: participant.videoOn,
    muted: participant.muted,
    talking: participant.talking,
    frames: frameCountsByParticipant.get(participant.id) ?? 0,
    audioPackets: audioPacketsByParticipant.get(participant.id) ?? 0,
    rawAudio: rawAudioStatusByParticipant.get(participant.id) ?? {
      status: "not-observed",
      lastResultCode: "no-participant-audio-subscription",
    },
  }));

  return {
    status,
    result: status === "passed" ? "PASS" : "FAIL",
    failureReason,
    failedChecks,
    elapsedMs: Date.now() - startedAt,
    meetingState: normalizeMeetingState(latestSnapshot?.meetingState),
    maxParticipantCount,
    activeSpeakerSeen,
    firstFrameMs: firstFrameAtMs ?? null,
    firstFrameSource,
    firstFrameParticipantId: firstFrameParticipantId ?? null,
    firstFrameDimensions: firstFrameDimensions ?? null,
    videoParticipantsWithFrames: participantsWithFrames(),
    audioParticipantsWithPackets: participantsWithAudioPackets(),
    criteria: {
      minParticipants,
      minVideoFeeds,
      maxFirstFrameMs,
      requireActiveSpeaker,
    },
    criteriaResults,
    participants: participantRows,
    subscriptions: latestSpineSnapshot?.subscriptions ?? [],
    warnings: collectWarnings(),
    recentStderr: stderrLines.slice(-10),
  };
}

function printReport(report) {
  console.log("");
  console.log("=".repeat(72));
  console.log(`Live Zoom validation: ${report.result}`);
  console.log("=".repeat(72));
  console.log(`Status        : ${report.status}`);
  console.log(`Elapsed       : ${report.elapsedMs}ms`);
  console.log(`Meeting state : ${report.meetingState}`);
  console.log(
    `First frame   : ${
      report.firstFrameMs === null
        ? "not observed"
        : `${report.firstFrameMs}ms via ${report.firstFrameSource}${report.firstFrameParticipantId ? ` (participant ${report.firstFrameParticipantId})` : ""}${report.firstFrameDimensions ? ` ${report.firstFrameDimensions}` : ""}`
    }`
  );
  if (report.failureReason) {
    console.log(`Failure       : ${report.failureReason}`);
  }
  if (report.failedChecks?.length) {
    console.log(`Failed checks : ${report.failedChecks.join(", ")}`);
  }

  console.log("");
  console.log("Criteria");
  for (const [name, result] of Object.entries(report.criteriaResults ?? {})) {
    const label = result.required ? "required" : "optional";
    const verdict = result.met ? "PASS" : "FAIL";
    console.log(`- ${name}: ${verdict} (${label}) ${result.detail}`);
  }

  console.log("");
  console.log("Participants");
  for (const participant of report.participants ?? []) {
    console.log(
      `- ${participant.id} ${participant.displayName}: video=${participant.videoOn ? "on" : "off"}, frames=${participant.frames}, audioPackets=${participant.audioPackets}, talking=${participant.talking ? "yes" : "no"}`
    );
  }

  if (report.warnings?.length) {
    console.log("");
    console.log("Warnings");
    for (const warning of report.warnings) {
      console.log(`- ${warning}`);
    }
  }

  console.log("");
  console.log("JSON report");
  console.log(JSON.stringify(report, null, 2));
}

function recordFirstFrame({ participantId, width, height, source, observedAtMs }) {
  if (firstFrameAtMs !== undefined) {
    return;
  }

  firstFrameAtMs = observedAtMs;
  firstFrameParticipantId = participantId;
  firstFrameDimensions =
    width > 0 && height > 0 ? `${width}x${height}` : undefined;
  firstFrameSource = source;
  console.log(
    `First frame : ${firstFrameAtMs}ms via ${source} from participant ${participantId}${
      firstFrameDimensions ? ` (${firstFrameDimensions})` : ""
    }`
  );
}

function hasZoomFramePixels(frame) {
  if (!frame || frame.width <= 0 || frame.height <= 0 || frame.frameId <= 0) {
    return false;
  }

  const expectedBytes = frame.width * frame.height * 4;
  if (typeof frame.bgraBase64 === "string" && frame.bgraBase64.length > 0) {
    return true;
  }

  if (Array.isArray(frame.rgba) && frame.rgba.length === expectedBytes) {
    return true;
  }

  if (Array.isArray(frame.bgra) && frame.bgra.length === expectedBytes) {
    return true;
  }

  return false;
}

function usableParticipants(snapshot) {
  return (snapshot?.participants ?? [])
    .map((participant) => ({
      id: String(participant.userId ?? participant.sdkUserId ?? ""),
      displayName: participant.displayName ?? "Zoom participant",
      videoOn: participant.videoOn !== false,
      muted: Boolean(participant.muted),
      talking: Boolean(participant.talking),
      sharingScreen: Boolean(participant.sharingScreen),
    }))
    .filter((participant) => participant.id);
}

function participantsWithFrames() {
  return [...frameCountsByParticipant.entries()].filter(([, count]) => count > 0).map(([participantId]) => participantId);
}

function participantsWithAudioPackets() {
  return [...audioPacketsByParticipant.entries()].filter(([, count]) => count > 0).map(([participantId]) => participantId);
}

function participantCount(snapshot) {
  return usableParticipants(snapshot).length;
}

function normalizeMeetingState(state) {
  if (state === "in_meeting") {
    return "in-meeting";
  }
  return state ?? "unknown";
}

function collectWarnings() {
  return [...new Set(warnings.filter(Boolean))];
}

function readEmbeddedPublicAppKey() {
  const configPath = join(repoRoot, "src", "config", "zoomMeetingSdk.json");
  if (!existsSync(configPath)) {
    return "";
  }
  return JSON.parse(readFileSync(configPath, "utf8")).publicAppKey ?? "";
}

function parseArgs(values) {
  const result = {};
  for (let index = 0; index < values.length; index += 1) {
    const value = values[index];
    if (!value.startsWith("--")) {
      continue;
    }
    const key = value.slice(2);
    const next = values[index + 1];
    if (!next || next.startsWith("--")) {
      result[key] = "true";
      continue;
    }
    result[key] = next;
    index += 1;
  }
  return result;
}

function numberArg(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

function booleanArg(value) {
  return value === true || value === "true" || value === "1" || value === "yes";
}

function redactMeetingUrl(value) {
  return value.replace(/pwd=[^&]+/i, "pwd=REDACTED");
}

function sleep(ms) {
  return new Promise((resolvePromise) => setTimeout(resolvePromise, ms));
}

function cleanup(code) {
  for (const { reject, timer } of pending.values()) {
    clearTimeout(timer);
    reject(new Error("validation harness exiting"));
  }
  pending.clear();
  child.kill();
  process.exit(code);
}

function failBeforeSpawn(message) {
  console.error(message);
  process.exit(1);
}
