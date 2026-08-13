/**
 * The panelist join — the show's single source of truth for identity.
 * Joins live Zoom participants against the Mukana registry (still PIN-keyed)
 * and the operator override table, which is keyed by each panelist's person
 * key rather than their PIN — so a show with no Mukana registry, and
 * therefore no PINs, can still assign a host or reader. Precedence is
 * override, then Mukana, then the name parsed out of the Zoom display name,
 * field by field, so an override that carries only a role change never
 * blanks a person's name. The person key itself is computed once here and
 * carried on the panelist record so every downstream consumer — the override
 * lookup included — agrees on the same key.
 */

import type { MukanaDb, Panelist, Participant } from "./contracts.js";
import { identityFromName } from "./identity.js";
import type { OverrideRecord } from "./overrideDb.js";
import { resolvePersonKey } from "./personKey.js";

function pick(...candidates: (string | undefined)[]): string {
  for (const candidate of candidates) {
    if (candidate !== undefined && candidate.length > 0) return candidate;
  }
  return "";
}

/** Build the master panelist database, keyed by participant id. */
export function buildPanelistDb(
  participants: readonly Participant[],
  mukana: MukanaDb,
  overrides: Record<string, OverrideRecord>
): Map<string, Panelist> {
  const db = new Map<string, Panelist>();

  for (const participant of participants) {
    const identity = identityFromName(participant.rawName);
    const personKey = resolvePersonKey(participant);
    const mukanaRecord = identity.pin === null ? undefined : mukana[identity.pin];
    const override = overrides[personKey];

    db.set(participant.participantId, {
      ...participant,
      pin: identity.pin,
      personKey,
      displayName: pick(override?.displayName, mukanaRecord?.displayName, identity.displayName),
      location: pick(override?.location, mukanaRecord?.location, identity.location),
      role: override?.role ?? mukanaRecord?.role ?? "panelist",
      hasMukana: mukanaRecord !== undefined
    });
  }

  return db;
}
