import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";

const repoRoot = process.cwd();
const explicitRunner = process.argv[2];
const candidates = explicitRunner
  ? [explicitRunner]
  : [
      join(repoRoot, "native", "build", "corevideo-native.exe"),
      join(repoRoot, "native", "build", "Release", "corevideo-native.exe"),
      join(repoRoot, "native", "build-dev", "corevideo-native.exe"),
      join(repoRoot, "native", "build-dev", "Release", "corevideo-native.exe"),
    ];

const runner = candidates.find((candidate) => existsSync(candidate));
if (!runner) {
  console.error(
    "native recording proof requires corevideo-native.exe. Run npm run test:native-media-core or npm run build:native-dev.",
  );
  process.exit(2);
}

const child = spawn(runner, {
  cwd: repoRoot,
  stdio: ["pipe", "pipe", "pipe"],
});

let stdoutBuffer = "";
let stderrBuffer = "";
const pending = new Map();
let handshake;

const timeout = setTimeout(() => {
  fail("native recording proof timed out.");
}, 15000);

child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString("utf8");
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
  stderrBuffer += chunk.toString("utf8");
});

child.once("exit", (code) => {
  if (pending.size > 0) {
    fail(`native recording proof exited before completion (code ${code ?? "unknown"}).`);
  }
});

try {
  handshake = await waitForHandshake();
  if (handshake.ok !== true) {
    throw new Error("native core handshake failed.");
  }

  const response = await send({
    id: "recording-proof-1",
    type: "media-core-sync",
    elapsedMs: 1000,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "recording-proof",
        routes: [
          {
            routeId: "program",
            mode: "fixed",
            audioRole: "mix",
            participantId: "speaker-1",
          },
        ],
      },
      {
        type: "sync-participant-audio-mix",
        channels: [
          {
            participantId: "speaker-1",
            inputLevel: 72,
            muted: false,
            noiseSuppression: true,
          },
        ],
      },
      {
        type: "start-program-output",
        destinations: ["recording"],
        isoParticipantIds: ["speaker-1"],
      },
      {
        type: "set-recording-targets",
        targetFolder: "Recordings/CoreVideo Pro/native-proof",
        filenamePrefix: "alpha-proof",
        format: "mp4",
        quality: "high",
        isoParticipantIds: ["speaker-1"],
      },
      {
        type: "start-recording-session",
        sessionId: "alpha-proof-speaker-1",
        startedAtMs: 1000,
      },
    ],
  });

  const snapshot = response.snapshot;
  const recording = snapshot?.recording;
  const proof = recording?.proof;
  if (!recording?.active || recording.status !== "recording") {
    throw new Error(`expected active recording snapshot, got ${recording?.status ?? "missing"}.`);
  }
  if (!recording.programPath?.endsWith("alpha-proof-program-0.mp4")) {
    throw new Error(`unexpected program path: ${recording.programPath ?? "missing"}.`);
  }
  if (!Array.isArray(recording.streams) || recording.streams.length < 2) {
    throw new Error("expected program plus at least one ISO stream.");
  }
  if (!proof || proof.programFrameCount < 1 || proof.isoFrameCount < 1) {
    throw new Error(
      `recording proof did not observe program and ISO frames (proof=${JSON.stringify(proof ?? null)}).`,
    );
  }
  if (proof.durationMs < 33) {
    throw new Error(`recording proof durationMs too low: ${proof.durationMs}.`);
  }
  if (proof.audioPacketsObserved < 1 || proof.audioPresent !== true) {
    throw new Error("recording proof did not observe audio packets.");
  }
  if (proof.metadataValid !== true || proof.containerFormat !== "mp4") {
    throw new Error(
      `recording proof metadata is not valid mp4 (metadataValid=${proof.metadataValid}, container=${proof.containerFormat}).`,
    );
  }

  clearTimeout(timeout);
  cleanup();
  console.log("[recording-proof] native recording proof passed");
  console.log(`  runner: ${runner}`);
  console.log(`  program: ${recording.programPath}`);
  console.log(`  streams: ${recording.streams.length}`);
  console.log(`  frames: program=${proof.programFrameCount} iso=${proof.isoFrameCount} durationMs=${proof.durationMs}`);
} catch (error) {
  fail(error instanceof Error ? error.message : String(error));
}

function onLine(line) {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }

  if (message.ok === false) {
    fail(`native core returned protocol error: ${message.error?.message ?? "unknown error"}`);
  }

  if (message.type === "handshake" && message.ok === true) {
    handshake = message;
  }

  if (typeof message.id === "string") {
    const resolver = pending.get(message.id);
    if (resolver) {
      pending.delete(message.id);
      resolver(message);
    }
  }
}

function waitForHandshake() {
  if (handshake) {
    return Promise.resolve(handshake);
  }
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
    console.error(stderrBuffer.trim());
  }
  console.error(message);
  process.exit(1);
}

function cleanup() {
  pending.clear();
  if (!child.killed) {
    child.kill();
  }
}
