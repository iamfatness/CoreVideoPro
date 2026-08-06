/**
 * Mukana payload parsing and accumulation.
 * The Mukana REST backend serves two endpoints this module understands: the
 * panelist registry (people register, receive a 4-digit PIN, and the engine
 * joins them to Zoom participants on it) and the single "up" audience
 * question shown as a lower third. Both share the same off-hours behavior:
 * outside show hours the endpoint returns a status envelope rather than
 * data, which is a `dormant` outcome, never an error and never parsed as
 * either panelists or a question.
 */

import { coerceRole, type MukanaDb, type MukanaQuestion, type MukanaRecord } from "./contracts.js";

/** The shared off-hours envelope, common to every endpoint's dormant arm. */
export type DormantOutcome = { kind: "dormant"; detail: string };

export type MukanaOutcome =
  | { kind: "data"; db: MukanaDb }
  | DormantOutcome
  | { kind: "invalid"; reason: string };

export type QuestionOutcome =
  | { kind: "data"; question: MukanaQuestion | null }
  | DormantOutcome
  | { kind: "invalid"; reason: string };

function readString(source: Record<string, unknown>, key: string): string {
  const value = source[key];
  return typeof value === "string" ? value.trim() : "";
}

function readNumber(source: Record<string, unknown>, key: string): number {
  const value = source[key];
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

function readPin(source: Record<string, unknown>): string | null {
  const value = source.pin;
  if (typeof value === "number" && Number.isInteger(value)) return String(value);
  if (typeof value === "string" && value.trim().length > 0) return value.trim();
  return null;
}

/** Decode a raw response body into a JSON object, rejecting anything else. */
function decodeJsonObject(
  body: string
): { kind: "ok"; root: Record<string, unknown> } | { kind: "invalid"; reason: string } {
  let parsed: unknown;
  try {
    parsed = JSON.parse(body);
  } catch {
    return { kind: "invalid", reason: "response body is not JSON" };
  }

  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    return { kind: "invalid", reason: "response body is not a JSON object" };
  }

  return { kind: "ok", root: parsed as Record<string, unknown> };
}

/**
 * The off-hours status envelope, shared by every endpoint. Returns null when
 * the body carries no `status` key, i.e. it is not the dormant envelope.
 */
function dormantGate(root: Record<string, unknown>): DormantOutcome | null {
  if (!("status" in root)) return null;
  const detail = readString(root, "detail");
  return { kind: "dormant", detail: detail.length > 0 ? detail : "mukana endpoint dormant" };
}

/**
 * Detect the shared off-hours envelope directly from a raw response body.
 * This is the single place that recognizes "status" as the off-hours
 * signal: `parseMukanaPanelists` and `parseMukanaQuestion` call `dormantGate`
 * on the JSON object they have already decoded, and this wraps the same
 * check with its own decode step for callers — like the hands endpoint's
 * legacy three-line text parser — whose own outcome type has no `dormant`
 * arm to report it through. Returns null both when the body isn't JSON and
 * when it is JSON but not the dormant envelope; either way that just means
 * "not dormant," not an error.
 */
export function detectDormantEnvelope(body: string): DormantOutcome | null {
  const decoded = decodeJsonObject(body);
  if (decoded.kind === "invalid") return null;
  return dormantGate(decoded.root);
}

/** Parse a raw panelists response body into a PIN-keyed registry. */
export function parseMukanaPanelists(body: string): MukanaOutcome {
  const decoded = decodeJsonObject(body);
  if (decoded.kind === "invalid") return decoded;

  const root = decoded.root;
  const dormant = dormantGate(root);
  if (dormant) return dormant;

  const db: MukanaDb = {};
  for (const value of Object.values(root)) {
    if (typeof value !== "object" || value === null || Array.isArray(value)) continue;
    const record = value as Record<string, unknown>;

    const pin = readPin(record);
    if (pin === null) continue;

    const entry: MukanaRecord = {
      pin,
      displayName: readString(record, "displayName"),
      location: readString(record, "loc"),
      role: coerceRole(record.role),
      online: record.online === true
    };
    db[pin] = entry;
  }

  return { kind: "data", db };
}

/**
 * Parse a raw question response body into the single "up" audience
 * question, or `null` when no question is currently up — a normal show
 * state, not an error. Only the `q` node is this parser's business; a
 * sibling `hands` node in the same body belongs to a separate parser.
 */
export function parseMukanaQuestion(body: string): QuestionOutcome {
  const decoded = decodeJsonObject(body);
  if (decoded.kind === "invalid") return decoded;

  const root = decoded.root;
  const dormant = dormantGate(root);
  if (dormant) return dormant;

  const qValue = root.q;
  if (typeof qValue !== "object" || qValue === null || Array.isArray(qValue)) {
    return { kind: "data", question: null };
  }

  const q = qValue as Record<string, unknown>;
  const rawText = q.q;
  const text = typeof rawText === "string" ? rawText.replace(/[\n\r]+/g, " ").trim() : "";

  const question: MukanaQuestion = {
    key: readString(q, "key"),
    askerName: readString(q, "n"),
    text,
    tag: readString(q, "tag"),
    votes: readNumber(q, "v"),
    timestampMs: readNumber(q, "ts")
  };

  return { kind: "data", question };
}

/**
 * Accumulates successive fetches. Records are never removed by a later fetch
 * that omits them — a panelist who drops out of the registry mid-show keeps
 * their identity until an explicit purge.
 */
export class MukanaRegistry {
  private db: MukanaDb = {};

  merge(incoming: MukanaDb): void {
    // Clone each incoming record to prevent external mutations
    const cloned = Object.fromEntries(
      Object.entries(incoming).map(([pin, record]) => [pin, { ...record }])
    );
    this.db = { ...this.db, ...cloned };
  }

  current(): MukanaDb {
    // Clone each record to prevent external mutations
    return Object.fromEntries(
      Object.entries(this.db).map(([pin, record]) => [pin, { ...record }])
    );
  }

  purge(): void {
    this.db = {};
  }
}
