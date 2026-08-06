/**
 * Shared data contracts for the OHG show engine.
 * These are the normative shapes from the design spec §5. Note the deliberate
 * divergences from the Isadora patch: an absent PIN is `null` (never the string
 * `" "` or `"####"`), and slots are keyed by plain integers.
 */

export type Role = "panelist" | "host" | "reader" | "aslpanelist" | "aslinterpreter";

export const ROLES: readonly Role[] = [
  "panelist",
  "host",
  "reader",
  "aslpanelist",
  "aslinterpreter"
];

/** Roles that may be held by at most one person at a time. */
export const EXCLUSIVE_ROLES: readonly Role[] = ["host", "reader"];

export function isRole(value: unknown): value is Role {
  return typeof value === "string" && (ROLES as readonly string[]).includes(value);
}

/** Narrow an untrusted value to a Role, defaulting to "panelist". */
export function coerceRole(value: unknown): Role {
  return isRole(value) ? value : "panelist";
}

/** A participant as reported by the host's Zoom engine. */
export type Participant = {
  participantId: string;
  rawName: string;
  online: boolean;
  videoOn: boolean;
  audioOn: boolean;
  handRaised: boolean;
  /** The host's numeric Zoom role. Display only — never the editorial role. */
  zoomRole: number;
};

/** Display identity parsed out of a raw Zoom display name. */
export type Identity = {
  displayName: string;
  location: string;
  /** 4-digit Mukana PIN, or null when the name carries none. */
  pin: string | null;
};

/** A participant joined against the Mukana registry and operator overrides. */
export type Panelist = Participant &
  Identity & {
    hasMukana: boolean;
    role: Role;
  };

/** One position in the fixed-capacity live roster. `panelist === null` is an empty hole. */
export type Slot = {
  slot: number;
  panelist: Panelist | null;
};

/** A Mukana registry record, re-keyed by PIN. */
export type MukanaRecord = {
  pin: string;
  displayName: string;
  location: string;
  role: Role;
  online: boolean;
};

/** The Mukana registry, keyed by 4-digit PIN. */
export type MukanaDb = Record<string, MukanaRecord>;
