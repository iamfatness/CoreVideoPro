import { spawn } from "node:child_process";

const [, , label, runner, ...runnerArgs] = process.argv;

if (!label || !runner) {
  console.error("usage: node scripts/native-stdio-smoke.mjs <label> <runner> [...args]");
  process.exit(2);
}

const child = spawn(runner, runnerArgs, {
  stdio: ["pipe", "pipe", "pipe"],
});

let stdoutBuffer = "";
let stderrBuffer = "";
const pending = new Map();
let handshake;
let latestPreviewEvent;

const timeout = setTimeout(() => {
  fail(`${label} timed out waiting for stdio smoke response.`);
}, 10000);

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
    fail(`${label} exited before stdio smoke completed (code ${code ?? "unknown"}).`);
  }
});

try {
  child.stdin.write(`${JSON.stringify({ id: "native-smoke-handshake", type: "handshake" })}\n`);
  handshake = await waitForHandshake();
  if (handshake.ok !== true) {
    throw new Error(`${label} handshake failed.`);
  }

  const response = await send({
    id: "native-smoke-1",
    type: "media-core-sync",
    elapsedMs: 1000,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "preview-test",
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
        type: "start-program-output",
        destinations: ["recording"],
        isoParticipantIds: [],
      },
    ],
  });

  const preview = response.snapshot?.programFramePreview ?? (await waitForPreviewEvent())?.preview;
  if (!preview) {
    throw new Error(`${label} did not provide program-frame-preview via snapshot or event.`);
  }
  if (preview.pixelFormat !== "bgra") {
    throw new Error(`${label} preview pixelFormat must be bgra.`);
  }
  if (!preview.bgraBase64) {
    throw new Error(`${label} preview bgraBase64 must be non-empty.`);
  }
  if (preview.width > 320 || preview.height > 180) {
    throw new Error(`${label} preview exceeds 320x180 downscale cap.`);
  }

  clearTimeout(timeout);
  cleanup();
  console.log(`[test-native] ${label} program-frame-preview smoke ok (${preview.width}x${preview.height})`);
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
    fail(`${label} returned protocol error: ${message.error?.message ?? "unknown error"}`);
  }

  if (message.type === "handshake" && message.ok === true) {
    handshake = message;
  }
  if (message.type === "program-frame-preview") {
    latestPreviewEvent = message;
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

function waitForPreviewEvent() {
  if (latestPreviewEvent) {
    return Promise.resolve(latestPreviewEvent);
  }
  return new Promise((resolve) => {
    const startedAt = Date.now();
    const interval = setInterval(() => {
      if (latestPreviewEvent || Date.now() - startedAt > 5000) {
        clearInterval(interval);
        resolve(latestPreviewEvent);
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
