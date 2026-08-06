/**
 * Person key resolution: a stable identity key for editorial role assignment.
 * Roles (host, reader) must survive a Zoom reconnect, where the participant id
 * changes but the person does not. This module picks the most durable
 * identifier available for a participant — PIN, then normalized display name,
 * then participant id — and prefixes it by tier so the three resolution paths
 * can never collide with one another, even when their raw values coincide.
 */

import type { Participant } from "./contracts.js";
import { identityFromName } from "./identity.js";

/** An opaque, prefixed identity key. Never parse it — only compare it. */
export type PersonKey = string;

/**
 * The key a participant carrying `pin` resolves to. This is the sanctioned
 * way to reach a PIN-tier key from a PIN — the registry is PIN-keyed while
 * roles are person-keyed, so translating between them is a real need, and
 * it belongs here beside the convention rather than as a prefix literal
 * spelled at each call site. It is deliberately one-way: keys stay opaque,
 * so callers holding a `PersonKey` build the keys they want to compare
 * against instead of parsing the one they have.
 */
export function personKeyForPin(pin: string): PersonKey {
  return `pin:${pin}`;
}

/** Collapse internal whitespace runs to a single space, lowercase, trim. */
function normalizeDisplayName(displayName: string): string {
  return displayName.replace(/\s+/g, " ").trim().toLowerCase();
}

/**
 * Resolve the most durable identity key available for a participant.
 * Order: PIN (from the parsed identity) → normalized display name → participantId.
 * A name that normalizes to the empty string is treated as absent, so a blank
 * or PIN-only display name never produces a key shared across distinct people.
 */
export function resolvePersonKey(
  participant: Pick<Participant, "participantId" | "rawName">
): PersonKey {
  const { pin, displayName } = identityFromName(participant.rawName);

  if (pin !== null) {
    return personKeyForPin(pin);
  }

  const normalized = normalizeDisplayName(displayName);
  if (normalized.length > 0) {
    return `name:${normalized}`;
  }

  return `id:${participant.participantId}`;
}
