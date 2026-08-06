/**
 * Display-name parsing: the OHG identity convention.
 * Panelists register in Mukana, receive a 4-digit PIN, and put it in their Zoom
 * display name (e.g. "J.J. Mc Kenna | 1383 | Santa Venetia, CA"). This module
 * recovers the PIN plus a best-effort display name and location, which is all
 * the identity an unregistered guest ever gets.
 */

import type { Identity } from "./contracts.js";

/** A standalone 4-digit token. Word boundaries keep "12345" and "123" from matching. */
const PIN_PATTERN = /\b\d{4}\b/;

/** Panelists separate name/location/PIN with either "|" or "/". */
const SEGMENT_SEPARATOR = /[/|]/;

/** Zoom permits pasted multi-line names; they break every downstream list. */
const NAME_NOISE = /[\n\r]+/g;

function sanitize(rawName: string): string {
  return rawName.replace(NAME_NOISE, "");
}

/** The first standalone 4-digit token in the name, or null when there is none. */
export function extractPin(rawName: string): string | null {
  const match = sanitize(rawName).match(PIN_PATTERN);
  return match === null ? null : match[0];
}

/**
 * Split a display name into name and location. The first segment is always the
 * name; the location is the first later segment that is not the PIN itself.
 */
export function splitDisplayName(
  rawName: string,
  pin: string | null
): { displayName: string; location: string } {
  const segments = sanitize(rawName)
    .split(SEGMENT_SEPARATOR)
    .map((segment) => segment.trim())
    .filter((segment) => segment.length > 0);

  const displayName = segments[0] ?? "";
  const location = segments.slice(1).find((segment) => segment !== pin) ?? "";
  return { displayName, location };
}

/** Full identity parse of a raw Zoom display name. */
export function identityFromName(rawName: string): Identity {
  const pin = extractPin(rawName);
  const { displayName, location } = splitDisplayName(rawName, pin);
  return { displayName, location, pin };
}
