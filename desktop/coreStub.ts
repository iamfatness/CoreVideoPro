/**
 * Node media-core stub. Stands in for Track B's native binary so the whole
 * desktop pipe runs end-to-end with zero native code. Speaks the line-delimited
 * JSON-RPC in `coreProtocol.ts` over stdio.
 *
 * Runnable directly by `node desktop/coreStub.ts` (Node >= 22 type-stripping).
 */
import { createInterface } from "node:readline";
import { stdin, stdout } from "node:process";
import { Buffer } from "node:buffer";
import { handleCaptureDeviceRequest } from "./captureDeviceStub.ts";
import type { CoreCaptureBridgeRequest, CoreEvent, CoreRequest, CoreResponse } from "./coreProtocol.ts";
import { programFramePreviewEventFromSnapshot } from "./programFramePreview.ts";
import {
  SYNTHETIC_PROFILE,
  createSyntheticZoomCaptureState,
  synthesizeSnapshot,
  synthesizeSpineSnapshot,
  synthesizeZoomJoinSnapshot,
  synthesizeZoomLeaveSnapshot,
  synthesizeZoomSnapshot
} from "./syntheticMediaCore.ts";

let frameNumber = 0;
const zoomCaptureState = createSyntheticZoomCaptureState();

const ZOOM_THUMB_WIDTH = 160;
const ZOOM_THUMB_HEIGHT = 90;
const ZOOM_STUB_PARTICIPANTS = ["p1", "p2", "p3"] as const;

const ZOOM_PARTICIPANT_COLORS: Record<string, [number, number, number]> = {
  p1: [161, 193, 68],
  p2: [92, 168, 240],
  p3: [200, 140, 100]
};

function synthesizeZoomVideoFrameEvent(participantId: string, frameId: number) {
  const width = ZOOM_THUMB_WIDTH;
  const height = ZOOM_THUMB_HEIGHT;
  const bgra = Buffer.alloc(width * height * 4);
  const [b, g, r] = ZOOM_PARTICIPANT_COLORS[participantId] ?? [80, 60, 40];
  const pulse = 0.72 + 0.28 * Math.sin(frameId / 8);
  for (let index = 0; index < width * height; index += 1) {
    const offset = index * 4;
    bgra[offset] = Math.round(b * pulse);
    bgra[offset + 1] = Math.round(g * pulse);
    bgra[offset + 2] = Math.round(r * pulse);
    bgra[offset + 3] = 255;
  }

  return {
    type: "zoom-video-frame",
    frame: {
      participantId,
      width,
      height,
      frameId,
      bgraBase64: Buffer.from(bgra).toString("base64")
    }
  };
}

export type CoreStubDispatch = {
  response: CoreResponse;
  events?: CoreEvent[];
};

export function handleCoreRequest(raw: string): CoreResponse | null {
  let request: Partial<CoreRequest>;
  try {
    request = JSON.parse(raw) as Partial<CoreRequest>;
  } catch {
    return { id: "unknown", ok: false, error: { code: "invalid-request", message: "Unparseable request." } };
  }

  if (!request || typeof request.id !== "string" || typeof request.type !== "string") {
    return { id: "unknown", ok: false, error: { code: "invalid-request", message: "Request needs id and type." } };
  }

  switch (request.type) {
    case "handshake":
      return { id: request.id, ok: true, type: "handshake", profile: SYNTHETIC_PROFILE };
    case "ping":
      return { id: request.id, ok: true, type: "ping" };
    case "media-core-sync": {
      const sync = request as Extract<CoreRequest, { type: "media-core-sync" }>;
      if (!Array.isArray(sync.commands) || typeof sync.elapsedMs !== "number") {
        return { id: request.id, ok: false, error: { code: "invalid-request", message: "sync needs commands and elapsedMs." } };
      }
      frameNumber += 1;
      const snapshot = synthesizeSnapshot(sync.commands, sync.elapsedMs, frameNumber, zoomCaptureState);
      return {
        id: request.id,
        ok: true,
        type: "media-core-sync",
        snapshot
      };
    }
    case "zoom-join": {
      const join = request as Extract<CoreRequest, { type: "zoom-join" }>;
      if (!join.payload || typeof join.payload.displayName !== "string") {
        return { id: request.id, ok: false, error: { code: "invalid-request", message: "zoom-join needs a payload." } };
      }
      return { id: request.id, ok: true, type: "zoom-join", snapshot: synthesizeZoomJoinSnapshot(zoomCaptureState, join.payload) };
    }
    case "zoom-leave":
      return { id: request.id, ok: true, type: "zoom-leave", snapshot: synthesizeZoomLeaveSnapshot(zoomCaptureState) };
    case "zoom-snapshot":
      return { id: request.id, ok: true, type: "zoom-snapshot", snapshot: synthesizeZoomSnapshot(zoomCaptureState) };
    case "zoom-media-spine-sync": {
      const spine = request as Extract<CoreRequest, { type: "zoom-media-spine-sync" }>;
      if (!spine.spinePayload || typeof spine.elapsedMs !== "number") {
        return { id: request.id, ok: false, error: { code: "invalid-request", message: "zoom-media-spine-sync needs spinePayload and elapsedMs." } };
      }
      return {
        id: request.id,
        ok: true,
        type: "zoom-media-spine-sync",
        spineSnapshot: synthesizeSpineSnapshot(spine.spinePayload, spine.elapsedMs)
      };
    }
    case "list-capture-devices":
    case "select-capture-input":
    case "set-capture-audio-sync-offset":
    case "connect-capture-device":
      return handleCaptureDeviceRequest(request as CoreCaptureBridgeRequest);
    case "__crash":
      // Intentionally take the process down to exercise supervisor restart.
      stdout.write(`${JSON.stringify({ id: request.id, ok: false, error: { code: "media-core-failed", message: "crashing" } })}\n`);
      process.exit(1);
      return null;
    default:
      return { id: request.id, ok: false, error: { code: "invalid-request", message: `Unsupported type ${request.type}.` } };
  }
}

export function dispatchCoreRequest(raw: string): CoreStubDispatch | null {
  const response = handleCoreRequest(raw);
  if (!response) {
    return null;
  }
  if (response.ok && response.type === "media-core-sync" && "snapshot" in response) {
    const events = ZOOM_STUB_PARTICIPANTS.map((participantId) =>
      synthesizeZoomVideoFrameEvent(participantId, response.snapshot.programFrame?.frameNumber ?? frameNumber)
    ) as unknown as CoreEvent[];
    const previewEvent = programFramePreviewEventFromSnapshot(response.snapshot);
    if (previewEvent) {
      events.push(previewEvent);
    }
    return events.length > 0 ? { response, events } : { response };
  }
  return { response };
}

/** Start the stdio loop. Exposed so tests can opt out of auto-start. */
export function startCoreStubLoop(): void {
  const lines = createInterface({ input: stdin });
  lines.on("line", (line: string) => {
    if (line.trim().length === 0) {
      return;
    }
    const dispatch = dispatchCoreRequest(line);
    if (!dispatch) {
      return;
    }
    stdout.write(`${JSON.stringify(dispatch.response)}\n`);
    for (const event of dispatch.events ?? []) {
      stdout.write(`${JSON.stringify(event)}\n`);
    }
  });
}

const entry = process.argv[1] ?? "";
if (entry.endsWith("coreStub.ts") || entry.endsWith("coreStub.js") || entry.endsWith("coreStub.cjs")) {
  startCoreStubLoop();
}
