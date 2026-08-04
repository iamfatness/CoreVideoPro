/**
 * Mukana registry parsing and accumulation.
 * The Mukana REST backend is the show's panelist registry: people register,
 * receive a 4-digit PIN, and the engine joins them to Zoom participants on it.
 * Records arrive keyed by Firebase UID and are re-keyed by PIN here.
 *
 * Outside show hours the endpoint returns a status envelope rather than data;
 * that is a `dormant` outcome, never an error and never parsed as panelists.
 */

import { coerceRole, type MukanaDb, type MukanaRecord } from "./contracts.js";

export type MukanaOutcome =
  | { kind: "data"; db: MukanaDb }
  | { kind: "dormant"; detail: string }
  | { kind: "invalid"; reason: string };

function readString(source: Record<string, unknown>, key: string): string {
  const value = source[key];
  return typeof value === "string" ? value.trim() : "";
}

function readPin(source: Record<string, unknown>): string | null {
  const value = source.pin;
  if (typeof value === "number" && Number.isInteger(value)) return String(value);
  if (typeof value === "string" && value.trim().length > 0) return value.trim();
  return null;
}

/** Parse a raw panelists response body into a PIN-keyed registry. */
export function parseMukanaPanelists(body: string): MukanaOutcome {
  let parsed: unknown;
  try {
    parsed = JSON.parse(body);
  } catch {
    return { kind: "invalid", reason: "response body is not JSON" };
  }

  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    return { kind: "invalid", reason: "response body is not a JSON object" };
  }

  const root = parsed as Record<string, unknown>;
  if ("status" in root) {
    const detail = readString(root, "detail");
    return { kind: "dormant", detail: detail.length > 0 ? detail : "registry dormant" };
  }

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
 * Accumulates successive fetches. Records are never removed by a later fetch
 * that omits them — a panelist who drops out of the registry mid-show keeps
 * their identity until an explicit purge.
 */
export class MukanaRegistry {
  private db: MukanaDb = {};

  merge(incoming: MukanaDb): void {
    this.db = { ...this.db, ...incoming };
  }

  current(): MukanaDb {
    return { ...this.db };
  }

  purge(): void {
    this.db = {};
  }
}
