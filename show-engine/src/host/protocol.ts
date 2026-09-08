import type { ZoomEvent } from "../zoomIngest.js";
import type { ActionDefinition } from "../actions.js";
import type { ShowSnapshot } from "../showSnapshot.js";
import type { ControlFieldValue } from "../controlState.js";

export const PROTOCOL_VERSION = 1;

export type RequestType = "handshake" | "invoke" | "zoomEvent" | "activeSpeaker" | "capacity" | "ping" | "shutdown";

export type Request =
  | { id: string; type: "handshake" }
  | { id: string; type: "invoke"; action: string; args: readonly unknown[] }
  | { id: string; type: "zoomEvent"; event: ZoomEvent }
  | { id: string; type: "activeSpeaker"; participantId: string }
  | { id: string; type: "capacity"; capacity: number }
  | { id: string; type: "ping" }
  | { id: string; type: "shutdown" };

export type HandshakePayload = {
  protocolVersion: number;
  engineVersion: string;
  generation: number;
  actions: readonly ActionDefinition[];
  fieldTemplates: readonly string[];
  snapshot: ShowSnapshot;
  fields: Record<string, ControlFieldValue>;
};

export type Response =
  | { id: string | null; ok: true; [key: string]: unknown }
  | { id: string | null; ok: false; error: { message: string } };

export type HostCommandName =
  | "assignSlot" | "applyLook" | "setPreview" | "cut" | "auto" | "setGallery" | "setNameplates" | "setQuestion";

export type Event =
  | ({ event: "handshake" } & HandshakePayload)
  | { event: "snapshot"; generation: number; revision: number; snapshot: ShowSnapshot; fields: Record<string, ControlFieldValue> }
  | { event: "hostCommand"; generation: number; seq: number; name: HostCommandName; args: unknown[] }
  | { event: "log"; level: "info" | "warn" | "error"; message: string };

/** JSON.stringify replacer: turns any Map (at any depth) into a [key, value][] pairs array. */
function mapReplacer(_key: string, value: unknown): unknown {
  if (value instanceof Map) {
    return Array.from(value.entries());
  }
  return value;
}

/** Strip any raw newline/carriage-return that (only theoretically) survived serialization. */
function stripNewlines(line: string): string {
  return line.includes("\n") || line.includes("\r") ? line.replace(/[\r\n]/g, "\\n") : line;
}

/** Serialize one message to a single line WITHOUT the trailing newline. Never throws: a value that
 *  cannot be serialized (bigint, circular) becomes `{"id":null,"ok":false,"error":{"message":…}}`
 *  for responses, or a `log` event at level "error" for events. */
export function encodeLine(message: Request | Response | Event): string {
  try {
    const line = JSON.stringify(message, mapReplacer);
    if (typeof line !== "string") {
      throw new TypeError("JSON.stringify returned undefined (unsupported top-level value)");
    }
    return stripNewlines(line);
  } catch (err) {
    const errorMessage = err instanceof Error ? err.message : String(err);
    if (typeof message === "object" && message !== null && "event" in message) {
      return stripNewlines(JSON.stringify({ event: "log", level: "error", message: errorMessage }));
    }
    return stripNewlines(JSON.stringify({ id: null, ok: false, error: { message: errorMessage } }));
  }
}

/** Parse one line. Returns a discriminated result; never throws. */
export type DecodeResult =
  | { kind: "request"; request: Request }
  | { kind: "malformed"; id: string | null; reason: string };

function malformed(id: string | null, reason: string): DecodeResult {
  return { kind: "malformed", id, reason };
}

export function decodeRequest(line: string): DecodeResult {
  let parsed: unknown;
  try {
    parsed = JSON.parse(line);
  } catch (err) {
    const errorMessage = err instanceof Error ? err.message : String(err);
    return malformed(null, `invalid JSON: ${errorMessage}`);
  }

  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    return malformed(null, "request must be a JSON object");
  }

  const record = parsed as Record<string, unknown>;
  const rawId = record.id;
  if (typeof rawId !== "string") {
    return malformed(null, "request must have a string 'id'");
  }
  const id = rawId;

  const type = record.type;
  if (typeof type !== "string") {
    return malformed(id, "request must have a string 'type'");
  }

  switch (type) {
    case "handshake":
      return { kind: "request", request: { id, type: "handshake" } };
    case "ping":
      return { kind: "request", request: { id, type: "ping" } };
    case "shutdown":
      return { kind: "request", request: { id, type: "shutdown" } };
    case "invoke": {
      const action = record.action;
      const args = record.args;
      if (typeof action !== "string" || !Array.isArray(args)) {
        return malformed(id, "invoke requires a string 'action' and an array 'args'");
      }
      return { kind: "request", request: { id, type: "invoke", action, args } };
    }
    case "zoomEvent": {
      const event = record.event;
      if (
        typeof event !== "object" ||
        event === null ||
        Array.isArray(event) ||
        typeof (event as Record<string, unknown>).kind !== "string"
      ) {
        return malformed(id, "zoomEvent requires an object 'event' with a string 'kind'");
      }
      return { kind: "request", request: { id, type: "zoomEvent", event: event as ZoomEvent } };
    }
    case "activeSpeaker": {
      const participantId = record.participantId;
      if (typeof participantId !== "string") {
        return malformed(id, "activeSpeaker requires a string 'participantId'");
      }
      return { kind: "request", request: { id, type: "activeSpeaker", participantId } };
    }
    case "capacity": {
      const capacity = record.capacity;
      if (typeof capacity !== "number" || !Number.isFinite(capacity) || !Number.isInteger(capacity)) {
        return malformed(id, "capacity requires a finite integer 'capacity'");
      }
      return { kind: "request", request: { id, type: "capacity", capacity } };
    }
    default:
      return malformed(id, `unknown request type '${type}'`);
  }
}
