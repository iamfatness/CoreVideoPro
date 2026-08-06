/**
 * Shared data contracts for the OHG show engine.
 * These are the normative shapes from the design spec §5. Note the deliberate
 * divergences from the Isadora patch: an absent PIN is `null` (never the string
 * `" "` or `"####"`), and slots are keyed by plain integers.
 */

import type { PersonKey } from "./personKey.js";

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

/** Roles automatically excluded from on-screen selection, e.g. the ASL interpreter. */
export const DEFAULT_SKIP_ROLES: readonly Role[] = ["aslinterpreter"];

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
    /** Stable identity key computed once by `resolvePersonKey`; never re-derive it downstream. */
    personKey: PersonKey;
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

/** The single audience question currently "up" on screen. */
export type MukanaQuestion = {
  key: string;
  askerName: string;
  text: string;
  tag: string;
  votes: number;
  timestampMs: number;
};

/**
 * Visual weight applied to a lower-third plate. Mirrors `PlateTone` in the
 * app's `src/engine/lowerThird.ts` (a different package) so the host adapter
 * maps these 1:1 onto CVP's lower-third renderer. Kept as its own copy here
 * rather than an import across the package boundary.
 */
export type PlateTone = "neutral" | "accent" | "guest" | "breaking";

export const PLATE_TONES: readonly PlateTone[] = ["neutral", "accent", "guest", "breaking"];

export function isPlateTone(value: unknown): value is PlateTone {
  return typeof value === "string" && (PLATE_TONES as readonly string[]).includes(value);
}

/**
 * Which slots a look's tally should credit as on air. `"boxes"` (the
 * default) is the ordinary case: host, reader, and guest boxes are all
 * live. `"activeSpeaker"` exists for looks like the legacy "Panel Checks"
 * state, where the look's own boxes are not on air and only the active
 * speaker, shown full-frame, is.
 */
export type TallySource = "boxes" | "activeSpeaker";

export const TALLY_SOURCES: readonly TallySource[] = ["boxes", "activeSpeaker"];

export function isTallySource(value: unknown): value is TallySource {
  return typeof value === "string" && (TALLY_SOURCES as readonly string[]).includes(value);
}

/**
 * How a look's guest boxes get filled with panelists. `"queue"` (the
 * default) pulls from the producer's speaking-order queue automatically;
 * `"manual"` leaves box assignment to an operator, e.g. for looks that need
 * specific panelists in specific boxes.
 */
export type BoxFill = "queue" | "manual";

export const BOX_FILLS: readonly BoxFill[] = ["queue", "manual"];

export function isBoxFill(value: unknown): value is BoxFill {
  return typeof value === "string" && (BOX_FILLS as readonly string[]).includes(value);
}

/** A named on-screen arrangement, e.g. "Host + Reader with two guests". */
export type LookDefinition = {
  id: string;
  label: string;
  scenePreset: string;
  boxes: number;
  includesHost: boolean;
  includesReader: boolean;
  plateTone: PlateTone;
  tallySource: TallySource;
  boxFill: BoxFill;
};

/** One position in the on-screen gallery grid. `slot: 0` means blank. */
export type GalleryCell = { cell: number; slot: number };

/** The producer's queued speaking order. All entries are 4-digit PIN strings. */
export type QueueState = {
  previous: string[];
  current: string | null;
  upcoming: string[];
};

/** What is currently feeding program video. */
export type ProgramSource =
  | { kind: "look"; lookId: string }
  | { kind: "gallery" }
  | { kind: "slot"; slot: number }
  | { kind: "activeSpeaker" }
  | { kind: "black" };

/** Structural equality for `ProgramSource`, comparing discriminant and payload. */
export function programSourcesEqual(a: ProgramSource, b: ProgramSource): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind === "look" && b.kind === "look") return a.lookId === b.lookId;
  if (a.kind === "slot" && b.kind === "slot") return a.slot === b.slot;
  return true;
}

/**
 * Whether an optional integration is currently usable. `"available"` means
 * the show is configured for it and it is responding; `"unavailable"` means
 * it is configured but not currently reachable (e.g. lost mid-broadcast);
 * `"disabled"` means the show was never configured to use it.
 */
export type CapabilityState = "available" | "unavailable" | "disabled";

/** The resolved status of one optional integration, with a human-readable reason. */
export type Capability = { state: CapabilityState; detail: string | null };

/** The resolved status of every optional integration this show may use. */
export type ShowCapabilities = {
  registry: Capability;
  handsQueue: Capability;
  questionFeed: Capability;
};
