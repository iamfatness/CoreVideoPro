/**
 * Typed show configuration for the OHG show engine.
 * Replaces the Isadora patch's `infraestructure-*.js` include files: every
 * external address, interval, and capacity lives here, validated at load so a
 * bad config fails loudly at startup instead of mid-show.
 */

import {
  DEFAULT_SKIP_ROLES,
  isPlateTone,
  isRole,
  PLATE_TONES,
  type LookDefinition,
  type Role
} from "./contracts.js";

export type MukanaConfig = {
  /** Base REST endpoint, e.g. https://host/phpsdk/php-panel-rest.php */
  baseUrl: string;
  /** The `event` query parameter, e.g. "officehours" */
  event: string;
  panelistsIntervalMs: number;
  handsIntervalMs: number;
  questionIntervalMs: number;
  /** Ceiling for exponential backoff after consecutive failures */
  maxBackoffMs: number;
};

export type ShowEngineConfig = {
  /** Number of concurrent participant slots the host can deliver */
  capacity: number;
  /**
   * PINs at or above this value denote utility participants (graphics bots,
   * playback machines) that are pinned to the tail slots rather than the
   * first free slot. `pin - utilityPinBase` is the offset from the last slot.
   */
  utilityPinBase: number;
  mukana: MukanaConfig;
  /** Absolute path of the persisted show-state JSON file */
  statePath: string;
  /** Roles automatically excluded from on-screen selection, e.g. the ASL interpreter. */
  skipRoles: Role[];
  /** Named on-screen arrangements available to this show. */
  looks: LookDefinition[];
};

const DEFAULT_UTILITY_PIN_BASE = 9000;
const DEFAULT_PANELISTS_INTERVAL_MS = 5000;
const DEFAULT_HANDS_INTERVAL_MS = 2000;
const DEFAULT_QUESTION_INTERVAL_MS = 2000;
const DEFAULT_MAX_BACKOFF_MS = 60000;
const MAX_LOOK_BOXES = 4;

function asRecord(value: unknown, field: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`show-engine ${field}: expected an object, got ${typeof value}`);
  }
  return value as Record<string, unknown>;
}

/** `label` is the dotted path used in error messages; `key` is the actual property. */
function requireString(source: Record<string, unknown>, key: string, label: string): string {
  const value = source[key];
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new Error(`show-engine ${label}: required non-empty string`);
  }
  return value;
}

function requirePositiveInt(
  source: Record<string, unknown>,
  key: string,
  label: string
): number {
  const value = source[key];
  if (typeof value !== "number" || !Number.isInteger(value) || value < 1) {
    throw new Error(`show-engine ${label}: required integer >= 1, got ${String(value)}`);
  }
  return value;
}

function optionalPositiveInt(
  source: Record<string, unknown>,
  key: string,
  label: string,
  fallback: number
): number {
  const value = source[key];
  if (value === undefined) return fallback;
  if (typeof value !== "number" || !Number.isInteger(value) || value < 1) {
    throw new Error(`show-engine ${label}: expected integer >= 1, got ${String(value)}`);
  }
  return value;
}

function requireBoolean(source: Record<string, unknown>, key: string, label: string): boolean {
  const value = source[key];
  if (typeof value !== "boolean") {
    throw new Error(`show-engine ${label}: required boolean, got ${String(value)}`);
  }
  return value;
}

function requireIntInRange(
  source: Record<string, unknown>,
  key: string,
  label: string,
  min: number,
  max: number
): number {
  const value = source[key];
  if (typeof value !== "number" || !Number.isInteger(value) || value < min || value > max) {
    throw new Error(
      `show-engine ${label}: expected integer in ${min}..${max}, got ${String(value)}`
    );
  }
  return value;
}

function optionalPlateTone(
  source: Record<string, unknown>,
  key: string,
  label: string
): LookDefinition["plateTone"] {
  const value = source[key];
  if (value === undefined) return "neutral";
  if (!isPlateTone(value)) {
    throw new Error(
      `show-engine ${label}: expected one of ${PLATE_TONES.join(", ")}, got ${String(value)}`
    );
  }
  return value;
}

/** Parse `root.skipRoles`, defaulting to the ASL interpreter. Rejects, never coerces, unknown roles. */
function parseSkipRoles(root: Record<string, unknown>): Role[] {
  const value = root.skipRoles;
  if (value === undefined) return [...DEFAULT_SKIP_ROLES];
  if (!Array.isArray(value)) {
    throw new Error(`show-engine config.skipRoles: expected an array, got ${typeof value}`);
  }
  return value.map((entry, index) => {
    if (!isRole(entry)) {
      throw new Error(
        `show-engine config.skipRoles[${index}]: unknown role ${JSON.stringify(entry)}`
      );
    }
    return entry;
  });
}

/** Parse a single look entry out of `root.looks[index]`. */
function parseLook(entry: unknown, index: number): LookDefinition {
  const label = `config.looks[${index}]`;
  const lookRaw = asRecord(entry, label);
  return {
    id: requireString(lookRaw, "id", `${label}.id`),
    label: requireString(lookRaw, "label", `${label}.label`),
    scenePreset: requireString(lookRaw, "scenePreset", `${label}.scenePreset`),
    boxes: requireIntInRange(lookRaw, "boxes", `${label}.boxes`, 0, MAX_LOOK_BOXES),
    includesHost: requireBoolean(lookRaw, "includesHost", `${label}.includesHost`),
    includesReader: requireBoolean(lookRaw, "includesReader", `${label}.includesReader`),
    plateTone: optionalPlateTone(lookRaw, "plateTone", `${label}.plateTone`)
  };
}

/** Parse `root.looks`, defaulting to empty. Rejects duplicate `id` values. */
function parseLooks(root: Record<string, unknown>): LookDefinition[] {
  const value = root.looks;
  if (value === undefined) return [];
  if (!Array.isArray(value)) {
    throw new Error(`show-engine config.looks: expected an array, got ${typeof value}`);
  }
  const looks = value.map((entry, index) => parseLook(entry, index));
  const seenIds = new Set<string>();
  for (const look of looks) {
    if (seenIds.has(look.id)) {
      throw new Error(`show-engine config.looks: duplicate look id ${JSON.stringify(look.id)}`);
    }
    seenIds.add(look.id);
  }
  return looks;
}

/** Validate raw JSON into a ShowEngineConfig, applying defaults. Throws on any invalid field. */
export function parseShowEngineConfig(raw: unknown): ShowEngineConfig {
  const root = asRecord(raw, "config");
  const mukanaRaw = asRecord(root.mukana, "config.mukana");

  return {
    capacity: requirePositiveInt(root, "capacity", "config.capacity"),
    utilityPinBase: optionalPositiveInt(
      root,
      "utilityPinBase",
      "config.utilityPinBase",
      DEFAULT_UTILITY_PIN_BASE
    ),
    statePath: requireString(root, "statePath", "config.statePath"),
    skipRoles: parseSkipRoles(root),
    looks: parseLooks(root),
    mukana: {
      baseUrl: requireString(mukanaRaw, "baseUrl", "config.mukana.baseUrl"),
      event: requireString(mukanaRaw, "event", "config.mukana.event"),
      panelistsIntervalMs: optionalPositiveInt(
        mukanaRaw,
        "panelistsIntervalMs",
        "config.mukana.panelistsIntervalMs",
        DEFAULT_PANELISTS_INTERVAL_MS
      ),
      handsIntervalMs: optionalPositiveInt(
        mukanaRaw,
        "handsIntervalMs",
        "config.mukana.handsIntervalMs",
        DEFAULT_HANDS_INTERVAL_MS
      ),
      questionIntervalMs: optionalPositiveInt(
        mukanaRaw,
        "questionIntervalMs",
        "config.mukana.questionIntervalMs",
        DEFAULT_QUESTION_INTERVAL_MS
      ),
      maxBackoffMs: optionalPositiveInt(
        mukanaRaw,
        "maxBackoffMs",
        "config.mukana.maxBackoffMs",
        DEFAULT_MAX_BACKOFF_MS
      )
    }
  };
}
