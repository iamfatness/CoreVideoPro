/**
 * Typed show configuration for the OHG show engine.
 * Replaces the Isadora patch's `infraestructure-*.js` include files: every
 * external address, interval, and capacity lives here, validated at load so a
 * bad config fails loudly at startup instead of mid-show.
 */

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
};

const DEFAULT_UTILITY_PIN_BASE = 9000;
const DEFAULT_PANELISTS_INTERVAL_MS = 5000;
const DEFAULT_HANDS_INTERVAL_MS = 2000;
const DEFAULT_QUESTION_INTERVAL_MS = 2000;
const DEFAULT_MAX_BACKOFF_MS = 60000;

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
