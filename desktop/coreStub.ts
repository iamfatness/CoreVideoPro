/**
 * Node media-core stub. Stands in for Track B's native binary so the whole
 * desktop pipe runs end-to-end with zero native code. Speaks the line-delimited
 * JSON-RPC in `coreProtocol.ts` over stdio.
 *
 * Runnable directly by `node desktop/coreStub.ts` (Node >= 22 type-stripping).
 */
import { createInterface } from "node:readline";
import { stdin, stdout } from "node:process";
import type { CoreRequest, CoreResponse } from "./coreProtocol.ts";
import { SYNTHETIC_PROFILE, synthesizeSnapshot, synthesizeSpineSnapshot } from "./syntheticMediaCore.ts";

let frameNumber = 0;

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
      return {
        id: request.id,
        ok: true,
        type: "media-core-sync",
        snapshot: synthesizeSnapshot(sync.commands, sync.elapsedMs, frameNumber)
      };
    }
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
    case "__crash":
      // Intentionally take the process down to exercise supervisor restart.
      stdout.write(`${JSON.stringify({ id: request.id, ok: false, error: { code: "media-core-failed", message: "crashing" } })}\n`);
      process.exit(1);
      return null;
    default:
      return { id: request.id, ok: false, error: { code: "invalid-request", message: `Unsupported type ${request.type}.` } };
  }
}

/** Start the stdio loop. Exposed so tests can opt out of auto-start. */
export function startCoreStubLoop(): void {
  const lines = createInterface({ input: stdin });
  lines.on("line", (line: string) => {
    if (line.trim().length === 0) {
      return;
    }
    const response = handleCoreRequest(line);
    if (response) {
      stdout.write(`${JSON.stringify(response)}\n`);
    }
  });
}

const entry = process.argv[1] ?? "";
if (entry.endsWith("coreStub.ts") || entry.endsWith("coreStub.js")) {
  startCoreStubLoop();
}
